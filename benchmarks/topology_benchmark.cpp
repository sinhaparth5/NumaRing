#include "numaring/numaring.hpp"

#include <benchmark/benchmark.h>

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

// No BENCHMARK_MAIN() here — see cache_alignment_benchmark.cpp.
