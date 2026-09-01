<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="assets/logo-light.svg">
  <img alt="NumaRing" src="assets/logo-light.svg" width="420">
</picture>

[![DOI](https://zenodo.org/badge/1353374159.svg)](https://doi.org/10.5281/zenodo.22232854)

A cache-conscious, topology-aware MPMC (multi-producer multi-consumer) queue for high-core NUMA server CPUs.
See `docs/NumaRing Theory.pdf` for the design rationale and `docs/ROADMAP.md` for build status.
The evaluation and results are written up in `paper/main.pdf` (source: `paper/main.tex`).

## Building

Requires a C++20 compiler and `libnuma` development headers (`libnuma-dev` on Debian/Ubuntu).

```sh
cmake -S . -B build
cmake --build build
```

Tests (Google Test) and benchmarks (Google Benchmark) are fetched automatically via CMake `FetchContent` and
built by default; disable either with `-DNUMARING_BUILD_TESTS=OFF` / `-DNUMARING_BUILD_BENCHMARKS=OFF`.

```sh
ctest --test-dir build
./build/benchmarks/numaring_benchmarks
```

NUMA-topology-dependent behavior (node-local placement, cross-socket profiling) is validated on real
multi-socket hardware rather than a dev laptop. See `CLAUDE.md` for the GCP testing workflow.
