#pragma once

#include <numa.h>
#include <sched.h>
#include <unistd.h>

#include <cstddef>
#include <vector>

namespace numaring {

namespace detail {

// numa_available() must be checked before any other libnuma call —
// numa_max_node(), numa_node_of_cpu(), etc. have undefined behavior
// on a kernel without NUMA support otherwise. Cached via a function-
// local static: initialization is thread-safe (C++11 "magic
// statics") and the answer can't change over the process lifetime.
inline bool numa_available_once() noexcept {
  static const bool available = (::numa_available() != -1);
  return available;
}

// CPU -> NUMA node lookup table, built once. Phase 5 profiling
// (docs/PHASE5_RESULTS.md) found ::numa_node_of_cpu() costs ~130ns
// per call — a bitmask scan over every NUMA node done fresh every
// time — and that current_node() (called on every single
// Queue::try_enqueue/try_dequeue) was ~11-20x the cost of the ring
// buffer operation it gates almost entirely because of this.
//
// Unlike thread-to-CPU placement (which migrates and must be
// re-queried per operation — see current_node()'s comment),
// CPU-to-node topology is fixed for the life of the process, so it's
// safe to compute it once per CPU and cache it here: this cuts
// node_of_cpu() to an O(1) array read with zero staleness risk,
// since sched_getcpu() itself is still called fresh on every
// current_node() call below.
inline const std::vector<int>& cpu_to_node_table() {
  static const std::vector<int> table = [] {
    std::vector<int> t;
    if (!numa_available_once()) {
      return t;  // left empty — node_of_cpu() falls back to node 0 either way
    }
    const long configured_cpus = ::sysconf(_SC_NPROCESSORS_CONF);
    if (configured_cpus <= 0) {
      return t;  // couldn't determine CPU count — fall back per-call below
    }
    t.resize(static_cast<std::size_t>(configured_cpus));
    for (std::size_t cpu = 0; cpu < t.size(); ++cpu) {
      const int node = ::numa_node_of_cpu(static_cast<int>(cpu));
      t[cpu] = node >= 0 ? node : 0;
    }
    return t;
  }();
  return table;
}

}  // namespace detail

// True if the host has NUMA support and libnuma initialized
// successfully. When false, every routing query below reports node 0
// / a single node, and callers (Phase 3+) should fall back to one
// shared ring buffer instead of a sub-queue per node.
inline bool numa_supported() noexcept {
  return detail::numa_available_once();
}

// Number of NUMA memory nodes visible to this process. Always >= 1;
// returns 1 when NUMA isn't supported.
//
// Assumes node ids are numbered contiguously from 0 to
// numa_max_node(), which holds on every Linux NUMA layout in
// practice (the kernel enumerates nodes this way) even though
// libnuma doesn't formally guarantee it.
inline int node_count() noexcept {
  if (!numa_supported()) {
    return 1;
  }
  return ::numa_max_node() + 1;
}

// The NUMA node that CPU core `cpu` belongs to, or 0 if NUMA isn't
// supported. `cpu` is a Linux CPU number as returned by
// sched_getcpu(), not a NUMA node id.
inline int node_of_cpu(int cpu) noexcept {
  if (!numa_supported() || cpu < 0) {
    return 0;
  }
  const auto& table = detail::cpu_to_node_table();
  if (static_cast<std::size_t>(cpu) < table.size()) {
    return table[static_cast<std::size_t>(cpu)];
  }
  // cpu wasn't covered by the table built at startup (e.g. hot-added
  // after this process started) — fall back to the direct libnuma
  // call rather than guess.
  const int node = ::numa_node_of_cpu(cpu);
  // numa_node_of_cpu() returns -1 on error (e.g. an offline or
  // out-of-range cpu); route those to node 0 rather than propagate
  // a sentinel that callers would have to special-case.
  return node >= 0 ? node : 0;
}

// The NUMA node local to the CPU core the calling thread is
// currently running on. Always returns 0 when NUMA isn't supported.
//
// The result can go stale the instant the scheduler migrates the
// thread to a different core. Callers on the enqueue/dequeue hot
// path should re-query per operation (it's a cheap syscall via vDSO,
// not a context switch) rather than caching it across anything that
// might block or yield.
inline int current_node() noexcept {
  if (!numa_supported()) {
    return 0;
  }
  const int cpu = ::sched_getcpu();
  if (cpu < 0) {
    // sched_getcpu() failing is not documented as possible on Linux,
    // but the interface allows it — degrade to node 0 rather than
    // pass a negative cpu into numa_node_of_cpu().
    return 0;
  }
  return node_of_cpu(cpu);
}

}  // namespace numaring
