# NumaRing — Engineering Roadmap

Checklist form of `docs/NumaRing ROADMAP.pdf`. Work through phases in order — each phase is a branch cut from
`master` (see `CLAUDE.md`). Check items off as they land.

## Phase 1 — Environment & Architecture Setup

- [ ] Repository structure: header-only layout under `include/numaring/`
- [ ] `benchmarks/` directory (Google Benchmark harness scaffold)
- [ ] `tests/` directory (Google Test harness scaffold)
- [ ] `CMakeLists.txt` targeting C++20 (`-std=c++20`)
- [ ] Optimization flags wired in (`-O3`, `-march=native`)
- [ ] Link `libnuma` (`-lnuma`)
- [ ] 128-byte dual-cache-line alignment wrapper (`alignas(128)`) for atomic tracking variables

## Phase 2 — NUMA Topology Discovery & Dynamic Routing

- [ ] Hardware topology detection via `libnuma` (`numa_available()`, `numa_max_node()`)
- [ ] Core-to-node mapper (`sched_getcpu()` + `numa_node_of_cpu()`)
- [ ] Fallback pathway: single-node MPMC ring buffer for non-NUMA / unified-memory machines

## Phase 3 — Node-Local Ring Buffer Core

- [ ] Node-local queue memory allocation via `numa_alloc_onnode()` (zero remote DRAM access)
- [ ] Bounded lock-free ring buffer slot sequence tracking
- [ ] Acquire-release atomics (`std::memory_order_acquire` / `std::memory_order_release`)
- [ ] Software prefetch integration (`_mm_prefetch`) ahead of consumer reads

## Phase 4 — Batched Cross-Node Work Stealing & Load Balancing

- [ ] Overflow/underflow detection (fast local capacity checks)
- [ ] Batched chunk transfer primitive (16–32 items) as a single atomic update
- [ ] Verify ≥93.75% reduction in inter-socket CAS ops vs. element-by-element transfer

## Phase 5 — Microarchitectural Profiling & Benchmarking

- [ ] Throughput & tail-latency suite (Google Benchmark; p95/p99/p99.9; 1 → 32+ threads)
- [ ] Baseline comparisons: `boost::lockfree::queue`, moodycamel `ConcurrentQueue`, mutex-based queue
- [ ] `perf c2c` run: confirm >85% reduction in cross-socket HITM cache invalidations
- [ ] `numastat` run: confirm >98% local NUMA memory hit ratio
- [ ] Target validation: >120M ops/sec throughput @ 32+ threads, sub-100ns tail latency

## Phase 6 — Paper Composition & Publication Preparation

- [ ] Manuscript draft: motivation, system design, formal queue semantics, evaluation
- [ ] Benchmark visualization: throughput scaling, tail latency distributions, cache invalidation charts
- [ ] Package repository as a reproducible artifact
- [ ] Target venue submission (USENIX ATC / PPoPP / IEEE TPDS)
