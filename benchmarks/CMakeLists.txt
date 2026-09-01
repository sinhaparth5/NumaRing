include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
)
FetchContent_MakeAvailable(benchmark)

# moodycamel::ConcurrentQueue is a single header with no build step —
# used as a Phase 5 baseline comparison (docs/ROADMAP.md).
FetchContent_Declare(
  concurrentqueue
  GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
  GIT_TAG v1.0.4
)
FetchContent_MakeAvailable(concurrentqueue)
# concurrentqueue's own CMakeLists.txt already defines the
# INTERFACE `concurrentqueue` target with its include dir attached.

# boost::lockfree::queue is the other Phase 5 baseline. Boost isn't a
# hard dependency of this project (it's header-only for lockfree, but
# a heavy thing to force via FetchContent) — if it's not installed on
# this host, that one baseline is skipped rather than the build
# failing; see baseline_queues.hpp's NUMARING_HAVE_BOOST guard.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost QUIET)
if(Boost_FOUND)
  set(NUMARING_HAVE_BOOST 1)
else()
  set(NUMARING_HAVE_BOOST 0)
  message(WARNING "Boost not found — the boost::lockfree::queue baseline in "
                   "latency_suite_benchmark.cpp will be skipped (install libboost-dev to include it).")
endif()

add_executable(numaring_benchmarks
  cache_alignment_benchmark.cpp
  latency_suite_benchmark.cpp
  ring_buffer_benchmark.cpp
  topology_benchmark.cpp
  work_stealing_benchmark.cpp
)
target_link_libraries(numaring_benchmarks PRIVATE numaring::numaring benchmark::benchmark_main concurrentqueue)
target_compile_definitions(numaring_benchmarks PRIVATE NUMARING_HAVE_BOOST=${NUMARING_HAVE_BOOST})
if(Boost_FOUND)
  target_link_libraries(numaring_benchmarks PRIVATE Boost::boost)
endif()

# Standalone (non-Google-Benchmark) long-running driver for perf c2c /
# numastat sampling — see its file comment.
add_executable(numaring_sustained_load sustained_load_driver.cpp)
target_link_libraries(numaring_sustained_load PRIVATE numaring::numaring)
