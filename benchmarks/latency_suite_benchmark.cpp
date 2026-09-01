#include "numaring/numaring.hpp"
#include "baseline_queues.hpp"

#include <benchmark/benchmark.h>

#if defined(__linux__)
#include <numa.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Phase 5 (docs/ROADMAP.md): throughput + tail-latency suite, run
// across numaring::Queue and three baselines (boost::lockfree::queue,
// moodycamel::ConcurrentQueue, a mutex+deque queue), swept over
// thread counts. This is the harness the roadmap's targets
// (>120M ops/sec @ 32+ threads, sub-100ns tail latency) are measured
// against — see docs/PHASE5_RESULTS.md for the numbers from a real
// multi-NUMA-node run (this repo's own dev box is single-socket, so
// local runs here are a correctness/smoke check on the harness
// itself, not the target validation).
//
// Workload: each registered (impl, thread count) case splits threads
// evenly into producers and consumers against one shared queue
// instance. A producer enqueues a monotonic-clock timestamp as the
// payload; a consumer dequeues it and records (now() - payload) as
// one latency sample. That's real cross-thread producer -> consumer
// queueing latency, not a same-thread ping-pong (ring_buffer_benchmark
// .cpp's BM_ContendedRoundTrip already covers the simpler throughput
// -only case). Every implementation runs the identical workload
// through the identical harness so the comparison is apples-to-apples.
//
// Percentiles can't be computed via Google Benchmark's per-thread
// timer (it reports mean/stddev across repetitions, not quantiles
// over individual operations), so each thread instead timestamps its
// own samples manually and, once every thread in the run has
// finished, thread 0 merges, sorts, and reports p50/p95/p99/p99.9 as
// custom counters. Registering each (impl, thread count) case with a
// fixed ->Iterations() count (no automatic calibration) means the
// function body runs exactly once per case, so the merge/report step
// below never has to worry about stale state from a prior
// calibration pass.
namespace {

using Clock = std::chrono::steady_clock;

// Owned via shared_ptr in each registered benchmark's closure — one
// fresh instance per (impl, thread count) case, so there's no
// cross-case reset hazard.
struct LatencyState {
  explicit LatencyState(std::size_t threads) : per_thread_samples(threads) {}
  std::mutex mutex;
  std::vector<std::vector<std::uint64_t>> per_thread_samples;  // nanoseconds, indexed by thread_index()
  std::atomic<int> arrived{0};
};

double Percentile(const std::vector<std::uint64_t>& sorted_ns, double q) {
  if (sorted_ns.empty()) {
    return 0.0;
  }
  const std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted_ns.size() - 1));
  return static_cast<double>(sorted_ns[idx]);
}

// numaring::Queue routes purely off wherever the calling thread
// happens to be running (current_node()); it never pins threads
// itself (that's deliberately an application-level concern, not the
// library's). For numastat/perf c2c to say anything meaningful about
// locality, the benchmark has to make the thread -> node mapping
// deterministic instead of leaving it to whatever core the OS
// scheduler happened to place the thread on — otherwise a thread
// could be measured accessing node 0's memory from node 1 purely by
// scheduling luck, which would show up as "remote traffic" that has
// nothing to do with the algorithm. Pins the calling thread to all
// CPUs of one NUMA node (round-robin by thread_index() across
// however many nodes exist) via libnuma's numa_run_on_node() — a
// no-op on non-NUMA hardware.
void PinToNodeForThisThread(int thread_index) {
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

template <typename QueueT>
void RunLatencyCase(benchmark::State& state, QueueT& queue, const std::shared_ptr<LatencyState>& shared) {
  PinToNodeForThisThread(state.thread_index());
  const bool is_producer = state.thread_index() % 2 == 0;
  std::vector<std::uint64_t> local_samples;
  if (!is_producer) {
    local_samples.reserve(static_cast<std::size_t>(state.max_iterations));
  }

  for (auto _ : state) {
    if (is_producer) {
      const auto sent_ns = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
      while (!queue.try_enqueue(sent_ns)) {
        // Queue full — bounded queues push back on producers, matching
        // the roadmap's overflow-triggers-work-stealing design instead
        // of unbounded growth.
      }
    } else {
      std::uint64_t sent_ns = 0;
      while (!queue.try_dequeue(sent_ns)) {
        // Nothing to consume yet — busy-retry, matching the existing
        // BM_ContendedRoundTrip idiom in ring_buffer_benchmark.cpp.
      }
      const auto received_ns = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
      local_samples.push_back(received_ns - sent_ns);
    }
  }
  state.SetItemsProcessed(state.iterations());

  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->per_thread_samples[static_cast<std::size_t>(state.thread_index())] = std::move(local_samples);
  }
  shared->arrived.fetch_add(1, std::memory_order_acq_rel);

  if (state.thread_index() != 0) {
    return;
  }
  // Thread 0 waits for every other thread in this case to publish its
  // samples, then merges + reports. Spin-wait is fine here: it only
  // runs once per case, well outside the timed region.
  while (shared->arrived.load(std::memory_order_acquire) < state.threads()) {
    std::this_thread::yield();
  }
  std::vector<std::uint64_t> merged;
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    for (const auto& samples : shared->per_thread_samples) {
      merged.insert(merged.end(), samples.begin(), samples.end());
    }
  }
  std::sort(merged.begin(), merged.end());
  state.counters["p50_ns"] = Percentile(merged, 0.50);
  state.counters["p95_ns"] = Percentile(merged, 0.95);
  state.counters["p99_ns"] = Percentile(merged, 0.99);
  state.counters["p999_ns"] = Percentile(merged, 0.999);
}

template <typename QueueT>
void RegisterQueueSuite(const std::string& impl_name, const std::function<std::shared_ptr<QueueT>()>& make_queue,
                         const std::vector<int>& thread_counts, std::int64_t iterations_per_thread) {
  for (const int requested_threads : thread_counts) {
    // Needs at least one producer and one consumer.
    const int threads = (requested_threads % 2 == 0) ? requested_threads : requested_threads + 1;
    auto queue = make_queue();
    auto shared = std::make_shared<LatencyState>(static_cast<std::size_t>(threads));
    const std::string name = "BM_QueueSuite/" + impl_name + "/threads:" + std::to_string(threads);
    benchmark::RegisterBenchmark(name.c_str(), [queue, shared](benchmark::State& state) {
      RunLatencyCase(state, *queue, shared);
    })->Iterations(iterations_per_thread)->Threads(threads)->UseRealTime();
  }
}

// Registration must happen before benchmark::RunSpecifiedBenchmarks()
// (called from the shared main() linked in via benchmark::
// benchmark_main — see cache_alignment_benchmark.cpp for why there's
// no BENCHMARK_MAIN() in this file). A global's constructor runs
// during static initialization, before main() starts, which is the
// standard way to drive benchmark::RegisterBenchmark() outside of an
// explicit main().
struct RegisterAll {
  RegisterAll() {
    // 2 -> 32: the roadmap's "1 -> 32+ threads" sweep, floored at 2
    // since this workload always needs a producer/consumer pair (see
    // RegisterQueueSuite above). Single-thread round-trip cost is
    // already covered by BM_SingleThreadedRoundTrip in
    // ring_buffer_benchmark.cpp. On this dev box (8 logical cores)
    // the 16/32-thread cases oversubscribe and are a harness smoke
    // test only; the real >=32-thread target numbers come from the
    // GCP multi-NUMA-node run (docs/PHASE5_RESULTS.md).
    const std::vector<int> thread_counts = {2, 4, 8, 16, 32};
    constexpr std::size_t kCapacity = 4096;
    constexpr std::int64_t kIterationsPerThread = 100000;

    RegisterQueueSuite<numaring::Queue<std::uint64_t>>(
        "NumaRing",
        [kCapacity] { return std::make_shared<numaring::Queue<std::uint64_t>>(kCapacity); },
        thread_counts, kIterationsPerThread);

#if NUMARING_HAVE_BOOST
    RegisterQueueSuite<numaring::bench::BoostLockfreeQueue<std::uint64_t>>(
        "BoostLockfree",
        [kCapacity] { return std::make_shared<numaring::bench::BoostLockfreeQueue<std::uint64_t>>(kCapacity); },
        thread_counts, kIterationsPerThread);
#endif

    RegisterQueueSuite<numaring::bench::MoodycamelQueue<std::uint64_t>>(
        "Moodycamel",
        [kCapacity] { return std::make_shared<numaring::bench::MoodycamelQueue<std::uint64_t>>(kCapacity); },
        thread_counts, kIterationsPerThread);

    RegisterQueueSuite<numaring::bench::MutexQueue<std::uint64_t>>(
        "Mutex",
        [kCapacity] { return std::make_shared<numaring::bench::MutexQueue<std::uint64_t>>(kCapacity); },
        thread_counts, kIterationsPerThread);
  }
};

const RegisterAll register_all_queue_suites;

}  // namespace

// No BENCHMARK_MAIN() here — see cache_alignment_benchmark.cpp.
