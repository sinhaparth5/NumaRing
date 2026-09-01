#include "numaring/numaring.hpp"

#include <benchmark/benchmark.h>

#include <sched.h>

// The roadmap calls for core-to-node routing with "minimal
// instruction overhead" since it's meant to run on every
// enqueue/dequeue. This benchmark tracks that cost directly rather
// than trusting it stays cheap as the implementation evolves.
static void BM_CurrentNode(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(numaring::current_node());
  }
}
BENCHMARK(BM_CurrentNode);

static void BM_NodeOfCpu(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(numaring::node_of_cpu(0));
  }
}
BENCHMARK(BM_NodeOfCpu);

// Isolates current_node()'s two components (see Phase 5's finding in
// docs/PHASE5_RESULTS.md that current_node() is ~11-20x the cost of
// the ring buffer op it gates) — is the cost sched_getcpu() (a
// syscall, possibly without a vDSO fast path on some kernels) or
// numa_node_of_cpu() (an in-memory bitmask lookup against state
// libnuma caches at init)? Compare against BM_NodeOfCpu above.
static void BM_SchedGetcpu(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(::sched_getcpu());
  }
}
BENCHMARK(BM_SchedGetcpu);

// No BENCHMARK_MAIN() here — see cache_alignment_benchmark.cpp.
