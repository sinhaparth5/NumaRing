#include "numaring/numaring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

TEST(CacheLinePadded, OccupiesOneCacheLineBlock) {
  EXPECT_EQ(sizeof(numaring::CacheLinePadded<std::uint64_t>),
            numaring::kCacheLineSize);
}

TEST(CacheLinePadded, IsAlignedToCacheLine) {
  numaring::CacheLinePadded<std::uint64_t> padded{};
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&padded) % numaring::kCacheLineSize, 0u);
}

TEST(CacheLinePadded, ArrayElementsDoNotShareAPrefetchWindow) {
  numaring::CacheLinePadded<std::atomic<std::uint64_t>> slots[4];
  for (std::size_t i = 1; i < 4; ++i) {
    auto prev = reinterpret_cast<std::uintptr_t>(&slots[i - 1]);
    auto curr = reinterpret_cast<std::uintptr_t>(&slots[i]);
    EXPECT_EQ(curr - prev, numaring::kCacheLineSize);
  }
}

TEST(CacheLinePadded, ReadsAndWritesThroughToTheValue) {
  numaring::CacheLinePadded<int> padded{41};
  EXPECT_EQ(padded.get(), 41);
  padded.get() = 42;
  EXPECT_EQ(static_cast<int&>(padded), 42);
}
