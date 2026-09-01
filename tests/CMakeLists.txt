include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

add_executable(numaring_tests
  cache_alignment_test.cpp
)
target_link_libraries(numaring_tests PRIVATE numaring::numaring GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(numaring_tests)
