// Phase 5 (docs/ROADMAP.md): a standalone, long-running MPMC driver
// for numaring::Queue<T> — not a Google Benchmark binary. `perf c2c`
// and `numastat` need many seconds of sustained, steady-state traffic
// to sample/accumulate anything meaningful; numaring_benchmarks'
// individual cases run for a few hundred milliseconds each, which is
// too short a window for either tool. This binary just keeps
// producer/consumer threads hammering a Queue for a fixed wall-clock
// duration so `perf c2c record`/`numastat` have something to attach
// to and observe (see docs/PHASE5_RESULTS.md for how it's invoked).
//
// Usage: numaring_sustained_load [duration_seconds] [threads] [per_node_capacity]
// Threads are pinned round-robin across NUMA nodes (numa_run_on_node)
// so producer/consumer pairs deterministically straddle nodes — see
// PinToNodeForThisThread's comment in latency_suite_benchmark.cpp for
// why that matters for a locality measurement to mean anything.

#include "numaring/numaring.hpp"

#if defined(__linux__)
#include <numa.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

void PinToNode(int thread_index) {
#if defined(__linux__)
  if (numaring::numa_supported()) {
    const int node_count = numaring::node_count();
    if (node_count > 0) {
      numa_run_on_node(thread_index % node_count);
    }
  }
#else
  (void)thread_index;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  const int duration_seconds = argc > 1 ? std::atoi(argv[1]) : 10;
  const int threads = argc > 2 ? std::atoi(argv[2]) : 32;
  const std::size_t capacity = argc > 3 ? static_cast<std::size_t>(std::atol(argv[3])) : 4096;

  std::printf("numaring_sustained_load: duration=%ds threads=%d capacity=%zu node_count=%d\n", duration_seconds,
              threads, capacity, numaring::node_count());

  numaring::Queue<std::uint64_t> queue(capacity);
  std::atomic<bool> stop{false};
  std::vector<std::atomic<std::uint64_t>> ops_per_thread(static_cast<std::size_t>(threads));
  for (auto& counter : ops_per_thread) {
    counter.store(0, std::memory_order_relaxed);
  }

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([i, &queue, &stop, &ops_per_thread] {
      PinToNode(i);
      const bool is_producer = i % 2 == 0;
      std::uint64_t out = 0;
      std::uint64_t ops = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        if (is_producer) {
          if (queue.try_enqueue(static_cast<std::uint64_t>(i))) {
            ++ops;
          }
        } else {
          if (queue.try_dequeue(out)) {
            ++ops;
          }
        }
      }
      ops_per_thread[static_cast<std::size_t>(i)].store(ops, std::memory_order_relaxed);
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& worker : workers) {
    worker.join();
  }

  std::uint64_t total_ops = 0;
  for (auto& counter : ops_per_thread) {
    total_ops += counter.load(std::memory_order_relaxed);
  }
  std::printf("numaring_sustained_load: total_ops=%llu ops_per_sec=%.2f\n",
              static_cast<unsigned long long>(total_ops),
              static_cast<double>(total_ops) / static_cast<double>(duration_seconds));
  return 0;
}
