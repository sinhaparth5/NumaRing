# NumaRing

A cache-conscious, topology-aware MPMC (multi-producer multi-consumer) queue for high-core NUMA server CPUs.
See `docs/NumaRing Theory.pdf` for the design rationale and `docs/ROADMAP.md` for build status.

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
multi-socket hardware rather than a dev laptop — see `CLAUDE.md` for the GCP testing workflow.
