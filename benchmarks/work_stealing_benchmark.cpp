#include "numaring/numaring.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

// Phase 4 scaffold: a throughput proxy for the roadmap's "batched
// transfer reduces inter-socket atomic ops by ~93.75% (1 CAS vs 16)"
// claim. This benchmark can only show the *mechanism's* cost on
// whatever hardware it runs on — it moves items between two
// independent NodeLocalRingBuffers on a single machine, not across
// real NUMA sockets, so it can't reproduce cross-socket interconnect
// stalls. The actual perf-c2c/numastat validation on real multi-node
// hardware is Phase 5 (see docs/ROADMAP.md); this just tracks that
// the batched path stays cheaper than the per-item path as the code
// evolves.
namespace {
constexpr std::size_t kBatchSize = 16;
}  // namespace

static void BM_BatchedTransfer(benchmark::State& state) {
  numaring::NodeLocalRingBuffer<std::uint64_t> source(64);
  numaring::NodeLocalRingBuffer<std::uint64_t> destination(64);
  std::uint64_t batch[kBatchSize] = {};

  for (auto _ : state) {
    for (std::size_t i = 0; i < kBatchSize; ++i) {
      source.try_enqueue(std::uint64_t{i});
    }
    const std::size_t taken = source.try_dequeue_bulk(batch, kBatchSize);
    std::size_t placed = destination.try_enqueue_bulk(batch, taken);
    benchmark::DoNotOptimize(placed);
    destination.try_dequeue_bulk(batch, kBatchSize);
  }
}
BENCHMARK(BM_BatchedTransfer);

static void BM_PerItemTransfer(benchmark::State& state) {
  numaring::NodeLocalRingBuffer<std::uint64_t> source(64);
  numaring::NodeLocalRingBuffer<std::uint64_t> destination(64);
  std::uint64_t out;

  for (auto _ : state) {
    for (std::size_t i = 0; i < kBatchSize; ++i) {
      source.try_enqueue(std::uint64_t{i});
    }
    for (std::size_t i = 0; i < kBatchSize; ++i) {
      source.try_dequeue(out);
      destination.try_enqueue(out);
    }
    for (std::size_t i = 0; i < kBatchSize; ++i) {
      destination.try_dequeue(out);
    }
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_PerItemTransfer);

// No BENCHMARK_MAIN() here — see cache_alignment_benchmark.cpp.
