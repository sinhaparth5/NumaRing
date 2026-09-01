# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This repository is currently **pre-implementation**: it contains only planning documents (`docs/NumaRing Theory.pdf`,
`docs/NumaRing ROADMAP.pdf`), a `README.md`, and `LICENSE`. No source tree, build system, or tests exist yet. The
`.gitignore` targets a CMake/CLion C++ workflow, confirming the intended toolchain even though `CMakeLists.txt` has
not been created. There are no build, lint, or test commands to run until Phase 1 scaffolding (below) lands — don't
invent or assume any.

When asked to start implementation, follow the phase order in `docs/NumaRing ROADMAP.pdf` / `docs/ROADMAP.md` rather
than jumping ahead (e.g. don't write benchmarks before the ring buffer core exists, don't write work-stealing before
topology discovery). Check items off in `docs/ROADMAP.md` as they land.

## Git workflow

- **Branch per phase.** Before starting a new roadmap phase, cut a new branch from `master`
  (`git checkout master && git pull && git checkout -b phase-N-<short-name>`). Don't commit phase work directly to
  `master`.
- **Commit with plain `git`** (`git commit`). Do **not** append a `Co-Authored-By: Claude` trailer or any
  "Generated with Claude Code" line to commit messages or PR bodies in this repo — this overrides the harness's
  default commit/PR footer for NumaRing specifically.
- **Never `git push`.** Commit the work and stop — tell the user it's ready and ask them to push themselves. Do
  not fall back to pushing over HTTPS (or any other protocol) to work around the SSH key needing a passphrase;
  that workaround is no longer wanted even when the SSH push fails.
- **Open PRs with `gh`** only after the user has pushed the branch: use `gh pr create` to open the PR into
  `master`. Use `gh` for other GitHub operations (PR status, issues) too.

## Testing on GCP

Local dev machines are typically single-socket, so they can't validate NUMA-local placement or cross-socket
behavior. Correctness/perf testing that depends on real multi-NUMA-node hardware (ring buffer placement from
Phase 3 onward, and all of Phase 5's profiling) runs on a **throwaway GCE VM**, not locally:

1. Pick a machine type/zone with multiple NUMA nodes exposed to the guest — this generally means a high-vCPU
   instance from a family that spans multiple sockets (e.g. `n2-standard-*`, `n2d-standard-*`, `c2-standard-*`,
   `c2d-standard-*`, or `m1`/`m2`/`m3` at large core counts). Don't hardcode a machine type/zone: query
   availability first (`gcloud compute machine-types list --filter="..."`, or
   `gcloud compute machine-types describe <type> --zone <zone>`) and pick a zone that actually has it in stock.
2. Create the instance for that run only (`gcloud compute instances create ...`), SSH in
   (`gcloud compute ssh ... --zone ...`) to build and run tests/benchmarks.
3. **Delete the instance when the work is done** (`gcloud compute instances delete ...`) — don't leave test VMs
   running. Treat this as required cleanup, not optional.

`gcloud` is already authenticated (account `parth.sinha0912@gmail.com`, project `project-6e56124d-9e93-4934-ba8`) —
use the active config rather than re-authenticating.

## What NumaRing is

NumaRing is a research/engineering project for a **cache-conscious, topology-aware MPMC (multi-producer
multi-consumer) queue** for high-core NUMA server CPUs (AMD EPYC, Intel Xeon, Ampere). The problem it targets:
traditional lock-free MPMC queues use a single global ring buffer with centralized CAS-updated head/tail pointers,
which causes severe cross-socket cache-line bouncing (MESI/MOESI invalidation) under contention.

The design replaces this with a **hierarchical, topology-aware, node-local queue architecture**:

1. **Hierarchical NUMA-local sub-queues** — one sub-queue per NUMA node, with backing memory allocated from that
   node's local memory controller (`numa_alloc_onnode()`), so normal operations never touch remote DRAM.
2. **Topology-aware routing** — producer/consumer threads query their executing core (`sched_getcpu()`,
   `numa_node_of_cpu()`) and route enqueue/dequeue directly to their socket-local sub-queue, keeping atomic
   state transitions inside that socket's L3 hierarchy.
3. **Batched cross-node work stealing** — when a local sub-queue overflows or underflows, items move between
   NUMA nodes in contiguous chunks (16–32 items) as a single atomic transfer, instead of item-by-item, to cut
   inter-socket coherence traffic.
4. **Microarchitectural cache optimizations** — 128-byte (dual-cache-line) alignment on atomic tracking
   variables to avoid L2 spatial-prefetcher false sharing, explicit software prefetching (`_mm_prefetch`) ahead
   of consumer reads, and acquire-release (not sequentially-consistent) atomics throughout.

Full rationale and the target benchmark numbers (throughput, p99/p99.9 tail latency, `perf c2c` HITM reduction,
`numastat` local-memory ratio) are in `docs/NumaRing Theory.pdf`.

## Planned architecture (from the roadmap)

The roadmap (`docs/NumaRing ROADMAP.pdf`) lays out six phases; the intended repository shape once scaffolded:

- `include/numaring/` — header-only library implementation.
- `benchmarks/` — Google Benchmark harness (throughput, p95/p99/p99.9 latency, comparisons against
  `boost::lockfree::queue` and moodycamel `ConcurrentQueue`).
- `tests/` — Google Test harness.
- `CMakeLists.txt` — C++20 (`-std=c++20`), `-O3 -march=native`, links `libnuma` (`-lnuma`).

Phase sequence: (1) CMake scaffolding + 128-byte `alignas` cache padding wrappers → (2) NUMA topology discovery via
`libnuma` (`numa_available()`, `numa_max_node()`) with a single-node fallback for non-NUMA/unified machines → (3)
node-local bounded lock-free ring buffer core with acquire/release atomics and software prefetch → (4) batched
inter-node work-stealing for overflow/underflow → (5) profiling suite (Google Benchmark, `perf c2c`, `numastat`) →
(6) paper drafting for USENIX ATC / PPoPP / IEEE TPDS submission.

Key implication for implementation work: this is fundamentally a **memory-placement and cache-topology problem**,
not just a lock-free algorithms problem — correctness of NUMA node pinning (`numa_alloc_onnode`) and cache-line
layout (`alignas(128)`) matters as much as the atomics themselves, and should be validated with `numastat`/`perf c2c`
per the roadmap's Phase 5 methodology, not just functional tests.
