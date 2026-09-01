#include "numaring/numaring.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

// Phase 3 scaffold: single-threaded enqueue+dequeue round-trip cost,
// and a threaded variant so contention behavior isn't a total
// surprise before Phase 5's real thread-scaling suite arrives (see
// docs/ROADMAP.md).

static void BM_SingleThreadedRoundTrip(benchmark::State& state) {
  numaring::NodeLocalRingBuffer<std::uint64_t> ring(1024);
  std::uint64_t out;
  for (auto _ : state) {
    ring.try_enqueue(std::uint64_t{1});
    ring.try_dequeue(out);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_SingleThreadedRoundTrip);

// Half the threads enqueue, half dequeue, all against one shared
// ring — a rough proxy for MPMC contention. Not a substitute for the
// Phase 5 harness (no latency percentiles, no baseline comparison),
// just a sanity signal that contention doesn't collapse throughput
// during Phase 3/4 development.
static void BM_ContendedRoundTrip(benchmark::State& state) {
  static numaring::NodeLocalRingBuffer<std::uint64_t> ring(1024);
  const bool is_producer = state.thread_index() % 2 == 0;
  std::uint64_t out;
  for (auto _ : state) {
    if (is_producer) {
      while (!ring.try_enqueue(std::uint64_t{1})) {
      }
    } else {
      while (!ring.try_dequeue(out)) {
      }
      benchmark::DoNotOptimize(out);
    }
  }
}
BENCHMARK(BM_ContendedRoundTrip)->Threads(4)->UseRealTime();

// No BENCHMARK_MAIN() here — see cache_alignment_benchmark.cpp.
