# NumaRing — Engineering Roadmap

Checklist form of `docs/NumaRing ROADMAP.pdf`. Work through phases in order — each phase is a branch cut from
`master` (see `CLAUDE.md`). Check items off as they land.

## Phase 1 — Environment & Architecture Setup

- [x] Repository structure: header-only layout under `include/numaring/`
- [x] `benchmarks/` directory (Google Benchmark harness scaffold)
- [x] `tests/` directory (Google Test harness scaffold)
- [x] `CMakeLists.txt` targeting C++20 (`-std=c++20`)
- [x] Optimization flags wired in (`-O3`, `-march=native`)
- [x] Link `libnuma` (`-lnuma`)
- [x] 128-byte dual-cache-line alignment wrapper (`alignas(128)`) for atomic tracking variables

## Phase 2 — NUMA Topology Discovery & Dynamic Routing

- [x] Hardware topology detection via `libnuma` (`numa_available()`, `numa_max_node()`)
- [x] Core-to-node mapper (`sched_getcpu()` + `numa_node_of_cpu()`)
- [x] Fallback pathway: single-node MPMC ring buffer for non-NUMA / unified-memory machines

## Phase 3 — Node-Local Ring Buffer Core

- [x] Node-local queue memory allocation via `numa_alloc_onnode()` (zero remote DRAM access)
- [x] Bounded lock-free ring buffer slot sequence tracking
- [x] Acquire-release atomics (`std::memory_order_acquire` / `std::memory_order_release`)
- [x] Software prefetch integration (`_mm_prefetch`) ahead of consumer reads

## Phase 4 — Batched Cross-Node Work Stealing & Load Balancing

- [x] Overflow/underflow detection (fast local capacity checks)
- [x] Batched chunk transfer primitive (16–32 items) as a single atomic update
- [x] Verify ≥93.75% reduction in inter-socket CAS ops vs. element-by-element transfer — mechanism verified
      (1 CAS vs 16), throughput proxy benchmarked locally; real cross-socket `perf c2c` confirmation is Phase 5,
      on real multi-node hardware

## Phase 5 — Microarchitectural Profiling & Benchmarking

- [x] Throughput & tail-latency suite (Google Benchmark; p95/p99/p99.9; 1 → 32+ threads) — built
      and run on real 2-node NUMA hardware; see `docs/PHASE5_RESULTS.md`
- [x] Baseline comparisons: `boost::lockfree::queue`, moodycamel `ConcurrentQueue`, mutex-based queue
- [ ] `perf c2c` run: confirm >85% reduction in cross-socket HITM cache invalidations — **blocked**,
      no hardware PMU available on any vPMU-eligible machine type reachable under this project's
      32-vCPU quota; see `docs/PHASE5_RESULTS.md` §3
- [x] `numastat` run: confirm >98% local NUMA memory hit ratio — 99.9999%/100% measured (with
      caveats — see `docs/PHASE5_RESULTS.md` §4)
- [ ] Target validation: >120M ops/sec throughput @ 32 threads (this project's GCP quota caps at 32
      vCPUs total — nothing higher was reachable), sub-100ns tail latency — **still not met** after
      two rounds of real fixes: caching the `current_node()` routing lookup (~61x faster per-call)
      and then fixing work-stealing contention (de-shared round-robin candidate selection, steal-
      threshold hysteresis). Same-host controlled measurements: 32-thread throughput up ~1.23x,
      p50 latency down ~202x, p99 down ~3.1x — real, verified gains, still nowhere near 120M
      ops/sec. Single-op latency (16.2ns uncontended) meets the tail-latency target on its own; end-
      to-end queueing latency under 32-thread saturation does not, though it improved by orders of
      magnitude. A CPU-pause backoff was also tried and reverted — it helped instrumented latency
      but measurably cost real throughput under true sustained contention. Full numbers and
      methodology (including a caught-and-corrected cross-host measurement mistake) are in
      `docs/PHASE5_RESULTS.md` §1-2, §5-7

## Phase 6 — Paper Composition & Publication Preparation

- [ ] Manuscript draft: motivation, system design, formal queue semantics, evaluation
- [ ] Benchmark visualization: throughput scaling, tail latency distributions, cache invalidation charts
- [ ] Package repository as a reproducible artifact
- [ ] Target venue submission (USENIX ATC / PPoPP / IEEE TPDS)
