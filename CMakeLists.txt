cmake_minimum_required(VERSION 3.20)
project(numaring LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NUMARING_BUILD_TESTS "Build numaring tests" ON)
option(NUMARING_BUILD_BENCHMARKS "Build numaring benchmarks" ON)

find_package(NUMA REQUIRED)

# numaring is header-only.
add_library(numaring INTERFACE)
add_library(numaring::numaring ALIAS numaring)

target_include_directories(numaring INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(numaring INTERFACE cxx_std_20)
target_link_libraries(numaring INTERFACE NUMA::NUMA)

# -O3 -march=native per the roadmap. GCC/Clang only — MSVC doesn't
# understand these flags, and this project targets Linux/NUMA hosts
# anyway.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(numaring INTERFACE
    $<$<CONFIG:Release>:-O3 -march=native>
  )
endif()

if(NUMARING_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(NUMARING_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
