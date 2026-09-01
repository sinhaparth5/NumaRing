#include "numaring/numaring.hpp"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>

// Phase 1 scaffold: confirms the Google Benchmark harness builds and
// runs end to end. The real throughput/tail-latency suite (thread
// scaling, contention, baseline comparisons) is Phase 5 — see
// docs/ROADMAP.md.
static void BM_CacheLinePaddedIncrement(benchmark::State& state) {
  numaring::CacheLinePadded<std::atomic<std::uint64_t>> counter{};
  for (auto _ : state) {
    counter.get().fetch_add(1, std::memory_order_relaxed);
  }
}
BENCHMARK(BM_CacheLinePaddedIncrement);

// No BENCHMARK_MAIN() here — main() comes from the linked
// benchmark::benchmark_main library so this file can share an
// executable with other *_benchmark.cpp translation units (see
// benchmarks/CMakeLists.txt).
