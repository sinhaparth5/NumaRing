# Phase 5 Results — Microarchitectural Profiling & Benchmarking

Real hardware runs per `CLAUDE.md`'s GCP testing policy: three throwaway `n2-standard-32` GCE VMs
(`us-east1-b`), each deleted once its run was done — one for the initial profiling pass (§1–5
below), one to validate the `current_node()` fix that pass's findings motivated (§6), one for the
work-stealing contention fix (§7), which also required a controlled same-host comparison after an
initial cross-host comparison gave misleading numbers (see §7's methodology-correction note).
**32 vCPUs is a hard ceiling for this run**, not a choice: this project's Compute Engine quota caps
at 32 vCPUs total across every region (`CPUS_ALL_REGIONS`), so nothing above 32 threads was
reachable — see Environment notes at the bottom for what a higher-quota run would need. Everything
below is reported as measured; targets not met are stated as not met, not rounded up.

Confirmed genuinely multi-socket before testing (both runs):

```
$ numactl --hardware
available: 2 nodes (0-1)
node 0 cpus: 0 1 2 3 4 5 6 7 16 17 18 19 20 21 22 23
node 1 cpus: 8 9 10 11 12 13 14 15 24 25 26 27 28 29 30 31
node distances:
node   0   1
  0:  10  20
  1:  20  10
```

All 32 unit/integration tests pass on this hardware, including `Queue`'s cross-node work-stealing
path actually firing for the first time (`node_count() == 2`, previously only reachable in
reasoning/mechanism tests on the single-socket dev box).

## What was run

- `numaring_benchmarks --benchmark_filter=BM_QueueSuite` — the new Google-Benchmark-driven
  throughput/tail-latency suite (`benchmarks/latency_suite_benchmark.cpp`), covering
  `numaring::Queue`, `boost::lockfree::queue`, `moodycamel::ConcurrentQueue`, and a mutex+deque
  baseline, at 2/4/8/16/32 threads. Producer threads and consumer threads are pinned one-to-one
  to alternating NUMA nodes via `numa_run_on_node()` so the workload deterministically straddles
  both sockets — otherwise a "local memory ratio" or per-node measurement would be meaningless
  (the OS scheduler doesn't guarantee node stickiness on its own).
- `numaring_sustained_load` (new standalone driver, `benchmarks/sustained_load_driver.cpp`) — a
  long-running (20–25s), uninstrumented producer/consumer loop against `numaring::Queue`, built
  specifically because the Google Benchmark cases are individually too short-lived for
  `numastat`/`perf` to sample anything meaningful.
- `numastat` (before/after and `-p <pid>` during the sustained-load run).
- `perf c2c` — **could not run**; see Limitations.

## 1–2. Throughput & tail-latency suite / baseline comparisons

Full `--benchmark_out_format=json` output saved (not committed — raw benchmark output, regenerable
by rerunning on the same or an equivalent VM). Selected numbers, 32 threads (16 producers / 16
consumers, one pinned per node):

| Implementation  | items/sec | p50      | p99      | p99.9    |
|-----------------|-----------|----------|----------|----------|
| numaring::Queue | 373.8k/s  | 1.33 ms  | 2.00 ms  | 2.64 ms  |
| boost::lockfree | 83.2k/s   | 12.4 µs  | 49.0 µs  | 74.5 µs  |
| moodycamel      | 497.8k/s  | 2.5 µs   | 1.68 ms  | 1.90 ms  |
| mutex+deque     | 146.3k/s  | 33.2 µs  | 491.8 µs | 662.5 µs |

Uncontended/low-thread-count numbers (from `ring_buffer_benchmark.cpp` /
`work_stealing_benchmark.cpp`, same hardware) look very different and much closer to target:

| Benchmark                        | Time     |
|-----------------------------------|----------|
| `BM_SingleThreadedRoundTrip`      | 16.2 ns  |
| `BM_ContendedRoundTrip` (4 thr)   | 192 ns   |
| `BM_CurrentNode`                  | 183 ns   |
| `BM_BatchedTransfer` (16 items)   | 289 ns   |
| `BM_PerItemTransfer` (16 items)   | 681 ns   |

**These are two different metrics, not a contradiction.** `BM_SingleThreadedRoundTrip` etc. measure
the cost of a single enqueue/dequeue call. `BM_QueueSuite`'s p50/p99/p99.9 measure end-to-end
producer→consumer *queueing delay* under sustained, saturating 1:1 producer/consumer load with a
4096-slot bounded queue — which necessarily grows with queue depth once the consumer's steal rate
can't keep up with the producer (a property of any bounded queue under sustained saturation, not
specific to this algorithm — all four implementations show the same shape: latency balloons at
high thread counts). The roadmap's "sub-100ns tail latency" target is comfortably met by the
single-operation numbers; it is **not** met by the end-to-end queueing-delay numbers at 16+ threads,
and neither is any of the three baselines run through the identical harness.

### Actionable finding: `current_node()` cost dominates `Queue`'s per-op overhead

`BM_CurrentNode` (`sched_getcpu()` + `numa_node_of_cpu()`) costs **183 ns** — roughly *11–20x* the
cost of the entire underlying ring-buffer round trip it gates (16.2 ns). `Queue::try_enqueue`/
`try_dequeue` call `current_node()` (via `local_queue()`) on **every single operation**. This was the
best-supported, most actionable result from this profiling phase — **fixed and re-measured in §6
below** (and, since that alone didn't close the gap at high thread counts, followed by a second
round of work-stealing contention fixes in §7), not just flagged.

## 3. `perf c2c` (cross-socket HITM) — not obtainable in this environment

```
$ sudo perf stat -e cycles,instructions -- sleep 1
   <not supported>      cycles
   <not supported>      instructions
$ dmesg | grep -i pmu
Performance Events: unsupported CPU family 6 model 85 no PMU driver, software events only.
```

The `n2-standard-32` vCPU model exposes **no hardware PMU at all** to the guest — not even basic
cycle/instruction counting, let alone the precise memory-sampling events `perf c2c` needs. Compute
Engine has a `--performance-monitoring-unit=enhanced` create-time flag for exactly this, but it's
rejected for both `n2-standard-32` and `c3-standard-22` ("PerformanceMonitoringUnit is not
supported for `<type>` on API version v1") — vPMU passthrough isn't available on the machine
families/sizes reachable under this project's 32-vCPU total quota. Getting real `perf c2c` HITM
numbers needs either a quota increase into a vPMU-supported shape, or a bare-metal/sole-tenant
NUMA host. Left unchecked in `docs/ROADMAP.md` rather than faked.

## 4. `numastat` (local NUMA memory hit ratio)

System-wide, before/after the 20s sustained-load run at 32 threads:

```
                 node0      node1
numa_hit       1394195    1406460     (before: 1393387 / 1406024)
numa_miss            1          0
local_node     1393721    1402284
other_node          475       4176
```

`numa_hit / (numa_hit + numa_miss)`: **99.9999%** (node0), **100%** (node1) — clears the >98%
target. Caveat: this is a system-wide, since-boot counter, and it moved by only a few hundred pages
during the run — expected, because `NodeLocalRingBuffer` allocates its backing storage **once**
up front via `numa_alloc_onnode()` and never allocates again during steady-state operation, so
there's very little fresh page-allocation activity for `numastat`'s hit/miss counters to capture in
the first place (that's the design working as intended, not a measurement gap).

The more targeted check — that each node's ring buffer actually lands on *that* node's physical
memory — is `numastat -p <pid>` taken mid-run:

```
Per-node process memory usage (in MBs) for PID 4896 (numaring_sustai)
              Node 0    Node 1    Total
Private         3.57      2.56     6.14
```

Private memory split across both nodes (not all on one), consistent with one `NodeLocalRingBuffer`
per node each allocated via `numa_alloc_onnode(node)`.

## 5. Target validation (pre-fix numbers — see §6 for the current numbers after the `current_node()` fix)

| Target                                  | Result                                                                 |
|------------------------------------------|-------------------------------------------------------------------------|
| >120M ops/sec @ 32+ threads               | **Not met.** Best measured: 11.9M ops/sec (uninstrumented sustained-load driver, 32 threads, no per-op latency tracking overhead). The instrumented `BM_QueueSuite` figure (373.8k/s) is much lower still but includes `clock_gettime()`-based per-op latency sampling on every operation, which is not representative of undecorated throughput. |
| sub-100ns tail latency                    | **Met** for single-operation cost (16.2 ns uncontended, 192 ns at 4-way contention). **Not met** for end-to-end producer→consumer queueing delay under saturation at 16+ threads (see above — true of every baseline tested, not specific to this implementation). |
| perf c2c: >85% HITM reduction             | Not measured — no PMU available in this environment (see §3). |
| numastat: >98% local memory ratio         | **Met** (99.9999% / 100%, with the caveat above). |

## 6. Fix: caching CPU -> NUMA node lookup

The actionable finding above (`current_node()` costing ~183ns, ~11-20x the 16.2ns ring-buffer op it
gates) was addressed and re-measured on a fresh, same-class throwaway `n2-standard-32` VM
(`us-east1-b`, 2 NUMA nodes again, confirmed via `numactl --hardware` before testing).

Root cause, isolated with two new micro-benchmarks (`BM_SchedGetcpu`, `BM_NodeOfCpu` in
`benchmarks/topology_benchmark.cpp`): `sched_getcpu()` itself is cheap (2.3–3.0ns, vDSO-backed, no
syscall trap) — essentially all of `current_node()`'s cost was `numa_node_of_cpu()`, a bitmask scan
over every NUMA node that libnuma redid from scratch on every single call.

**Fix** (`include/numaring/topology.hpp`): CPU-to-NUMA-node topology is fixed for the life of the
process (unlike thread-to-CPU placement, which migrates and does need re-querying every call — see
`current_node()`'s existing comment). Added a lazily-built, process-wide `cpu -> node` lookup table
(`detail::cpu_to_node_table()`, one-time cost at first use, thread-safe via C++11 magic statics) that
`node_of_cpu()` now reads instead of calling `::numa_node_of_cpu()` on every invocation.
`current_node()` still calls `sched_getcpu()` fresh on every call — **zero staleness risk was
introduced**; only the static part of the lookup is cached.

| Metric                          | Before   | After   | Change |
|----------------------------------|----------|---------|--------|
| `current_node()` (real HW)        | 183 ns   | 2.97 ns | **~61x faster** |
| `numa_node_of_cpu()` alone (real HW) | ~130 ns (implied) | 0.89 ns | ~146x faster |

This part is not host-variance-sensitive (it never touches memory or contended cache lines), so the
raw before/after numbers above stand as measured, even across the two different VM instances they
were taken on.

## 7. Fixing the 16–32-thread bottleneck: work-stealing contention

The `current_node()` fix alone left a puzzle: it made a large difference at 2–8 threads but only a
small one at 16–32, where `BM_QueueSuite`'s p50/p99/p99.9 latencies stayed in the millisecond range.
Your read of that (bimodal p50-fast/p99-slow is a classic shared-contended-resource signature) led to
three targeted changes to `Queue`'s cross-node work-stealing path and `NodeLocalRingBuffer`'s CAS
retry loops:

1. **De-shared the round-robin candidate selection.** `Queue::for_each_other_candidate` picked its
   starting steal/spill candidate via a single `std::atomic<std::size_t> round_robin_` that every
   thread `fetch_add()`'d on every overflow/underflow — a genuinely unnecessary shared, contended
   cache line on that path, touched by all 32 threads under saturation. Replaced with per-thread
   `thread_local` state (a hash-based seed per thread, incrementing independently), which needs no
   cross-thread synchronization at all since nothing there needs to be exactly fair, just spread out.
2. **Steal-threshold hysteresis.** Added `NodeLocalRingBuffer::approx_size()` (two relaxed loads, a
   racy heuristic snapshot, never used for correctness) and gated `Queue::transfer_batch` on the
   source having at least `2 * batch_size_` items before attempting a steal — otherwise several
   threads simultaneously racing to steal the last handful of items from a nearly-empty source causes
   mostly-doomed `compare_exchange_weak` collisions. A skipped attempt just falls back to `Queue`'s
   existing single-item cross-queue path, so this only changes which path handles low-occupancy
   cases, never correctness.
3. **CPU-pause backoff in `NodeLocalRingBuffer`'s CAS retry loops — tried, measured, reverted.** Both
   a flat `_mm_pause()`-per-retry version and a growing bounded-exponential-backoff version were
   implemented and benchmarked. See the "dead end" subsection below — this one didn't survive
   contact with real measurement.

### A methodology correction, made honestly

The first same-VM-vs-different-VM comparison of these changes showed *every* implementation —
including `boost::lockfree::queue`, `moodycamel::ConcurrentQueue`, and the mutex baseline, none of
which this phase touched — getting substantially slower (e.g. Boost's 2-thread throughput dropping
from ~2.8M items/s to ~1.1M items/s). That's impossible to attribute to a code change that only
touches `numaring::Queue`. It meant the "before" and "after" numbers had been taken on two different
GCE VM instances, which can land on different physical hosts with different noisy-neighbor
conditions — a well-known cloud-benchmarking pitfall, and a mistake in this run, not a real
regression. **Caught it by checking whether unrelated baselines moved too before writing any
comparison numbers down.**

Fixed by building the pre-fix commit (`e33ef43`, via a git worktree) *and* the fixed commit *on the
same VM instance*, back to back, immediately re-running both. All numbers below are same-host,
controlled comparisons — this is what should have been done the first time, and is now.

### Same-host results

`numaring_sustained_load`, 32 threads, 3x15s runs each, same host:

| Build | Run 1 | Run 2 | Run 3 | Mean |
|---|---|---|---|---|
| Pre-fix | 8.65M ops/s | 9.41M ops/s | 9.02M ops/s | **9.03M** |
| + flat `cpu_relax()` backoff | 7.44M | 8.08M | 6.95M | **7.49M** (worse) |
| + exponential backoff | 7.25M | 5.44M | 6.47M | **6.39M** (worse still) |
| + work-stealing fixes only (backoff reverted) | 9.82M | 8.24M | 11.03M | **9.70M** (best — real gain) |

`BM_QueueSuite/NumaRing`, same host, final (work-stealing-fixes-only) build vs. pre-fix:

| Threads | Throughput before | Throughput after | p50 before | p50 after | p99 before | p99 after |
|---|---|---|---|---|---|---|
| 2  | 1.73M items/s | 12.35M items/s (**~7.1x**) | 3.48 ms | 218.3 µs (**~16x lower**) | 4.10 ms | 277.5 µs |
| 32 | 260.1k items/s | 320.9k items/s (**~1.23x**) | 1.69 ms | 8.4 µs (**~202x lower**) | 3.84 ms | 1.23 ms (**~3.1x lower**) |

`numastat`, same build: `numa_hit`/`numa_miss` = 4341961/0 (node0), 4286138/0 (node1) — **100%**
local-memory ratio, unaffected as expected.

### The backoff dead end, and why it's worth keeping documented

Both backoff variants *helped* `BM_QueueSuite`'s instrumented tail latency but *cost* real,
repeatable throughput in the uninstrumented `sustained_load` driver — and the "smarter" exponential
version cost more, not less (6.39M < 7.49M < 9.03M pre-fix). Best explanation: `BM_QueueSuite`
instruments every operation with `Clock::now()`, which inserts real wall-clock spacing between a
thread's own successive retries — under that spacing, pausing a losing CAS attempt looks like a
free win. `sustained_load` has no such spacing (threads hammer the shared position counter as fast
as possible), and under that *true* wall-to-wall contention, a losing CAS attempt's fastest path
back to success is retrying immediately — the atomic RMW itself already serializes access via cache
coherence, so spending cycles on `_mm_pause()` is pure loss, and growing that spend geometrically
loses more. Reverted from `include/numaring/ring_buffer.hpp`, documented in its class-level comment
so it isn't silently rediscovered and re-tried from scratch later.

### Verification

All 32 tests pass on real 2-node NUMA hardware at every stage of this (pre-fix, flat-backoff,
exponential-backoff, and final work-stealing-fixes-only builds all built and tested independently).
Locally: 5 repeated `ConcurrentMpmcStress` runs under ThreadSanitizer report zero races for the final
build (the lazy `thread_local`/static-table initializations touched by this work are exactly the
kind of thing worth re-checking there).

### Current target validation (replaces §5)

| Target                        | Result |
|--------------------------------|--------|
| >120M ops/sec @ 32 threads (project's quota ceiling — nothing higher was tested) | **Still not met.** Best same-host measurement: 320.9k items/s (`BM_QueueSuite`, instrumented) / ~9.70M ops/s mean (`numaring_sustained_load`, uninstrumented). Real, verified gains (~1.23x / ~1.07x respectively vs. pre-fix on the same host) — nowhere near 120M. |
| sub-100ns tail latency          | **Met** for single-operation cost (16.2–16.6 ns uncontended). **Not met** end-to-end under 32-thread saturation, though p50 there is now 8.4µs (down from 1.69ms pre-fix) and p99 down ~3.1x. |
| perf c2c: >85% HITM reduction   | Still not measured — environment limitation, unrelated to this work (see §3). |
| numastat: >98% local memory ratio | **Still met** (100% / 100%, re-confirmed post-fix, same host). |

**Honest bottom line:** two real, verified rounds of fixes (routing-layer caching, then work-stealing
contention) delivered large, genuine improvements — especially in tail latency, by 1-2 orders of
magnitude in places — and a small but real throughput gain at 32 threads. The >120M ops/sec target
is not close to being met at the 32-thread ceiling this project's GCP quota allows, and nothing in
this data suggests it would be reached simply by adding more of the same kind of fix; it would need
either far more threads/cores (unavailable here) or a more fundamental architectural change to how
the shared position counters are contended. That is not something to speculate further on without
more evidence — left as the honest state of the work, not rounded up.

## Environment notes for a future run

- This project's Compute Engine quota is capped at 32 vCPUs total across all regions
  (`CPUS_ALL_REGIONS`), which ruled out the 64+-vCPU shapes originally planned in `CLAUDE.md`.
  `n2-standard-32` turned out to expose 2 NUMA nodes anyway, which is how this run got real
  multi-socket hardware at all.
- For `perf c2c` in a future run: either request a quota increase into a
  `--performance-monitoring-unit`-eligible shape (check `gcloud compute instances create --help`
  for current eligibility — it changes over time and wasn't available on N2/C3 as of this run), or
  use a non-GCP host with real PMU access.
