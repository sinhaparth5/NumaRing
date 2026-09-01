#include "numaring/numaring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

TEST(NodeLocalRingBuffer, CapacityRoundsUpToPowerOfTwo) {
  EXPECT_EQ(numaring::NodeLocalRingBuffer<int>(1).capacity(), 1u);
  EXPECT_EQ(numaring::NodeLocalRingBuffer<int>(2).capacity(), 2u);
  EXPECT_EQ(numaring::NodeLocalRingBuffer<int>(3).capacity(), 4u);
  EXPECT_EQ(numaring::NodeLocalRingBuffer<int>(9).capacity(), 16u);
}

TEST(NodeLocalRingBuffer, SingleThreadedFifoOrder) {
  numaring::NodeLocalRingBuffer<int> ring(8);
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(ring.try_enqueue(i));
  }

  int out = -1;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(ring.try_dequeue(out));
    EXPECT_EQ(out, i);
  }
}

TEST(NodeLocalRingBuffer, TryEnqueueFailsWhenFull) {
  numaring::NodeLocalRingBuffer<int> ring(4);
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.try_enqueue(i));
  }
  EXPECT_FALSE(ring.try_enqueue(99));
}

TEST(NodeLocalRingBuffer, TryDequeueFailsWhenEmpty) {
  numaring::NodeLocalRingBuffer<int> ring(4);
  int out;
  EXPECT_FALSE(ring.try_dequeue(out));
}

TEST(NodeLocalRingBuffer, WrapsAroundCorrectly) {
  numaring::NodeLocalRingBuffer<int> ring(4);
  int out = -1;
  // Push the ring position well past one lap so slot reuse (and the
  // sequence-number bookkeeping that makes it safe) is exercised.
  for (int lap = 0; lap < 100; ++lap) {
    ASSERT_TRUE(ring.try_enqueue(lap));
    ASSERT_TRUE(ring.try_dequeue(out));
    EXPECT_EQ(out, lap);
  }
}

TEST(NodeLocalRingBuffer, MoveOnlyTypeWorks) {
  numaring::NodeLocalRingBuffer<std::unique_ptr<int>> ring(4);
  ASSERT_TRUE(ring.try_enqueue(std::make_unique<int>(42)));

  std::unique_ptr<int> out;
  ASSERT_TRUE(ring.try_dequeue(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 42);
}

TEST(NodeLocalRingBuffer, DefaultNodeIsZeroAndConstructible) {
  numaring::NodeLocalRingBuffer<int> ring(4, 0);
  EXPECT_TRUE(ring.try_enqueue(1));
}

TEST(NodeLocalRingBuffer, CurrentNodeIsAlwaysConstructible) {
  // current_node() is always a valid node id for this process,
  // whether or not NUMA is actually supported (see topology.hpp).
  numaring::NodeLocalRingBuffer<int> ring(4, numaring::current_node());
  EXPECT_TRUE(ring.try_enqueue(1));
}

namespace {

// Stress test: several producers and consumers hammering a small
// (heavily-wrapping) ring concurrently. Every value in [0, kTotal) is
// enqueued by exactly one producer; verifies every value is dequeued
// by exactly one consumer — no drops, no duplicates — which is the
// property the sequence-number CAS loop exists to guarantee under
// contention.
constexpr std::size_t kCapacity = 64;
constexpr std::uint64_t kTotal = 200'000;
constexpr int kProducers = 4;
constexpr int kConsumers = 4;

}  // namespace

TEST(NodeLocalRingBuffer, ConcurrentMpmcStress) {
  numaring::NodeLocalRingBuffer<std::uint64_t> ring(kCapacity);
  std::atomic<std::uint64_t> next_to_produce{0};
  std::atomic<std::uint64_t> consumed_count{0};
  std::vector<std::atomic<bool>> seen(kTotal);
  for (auto& flag : seen) {
    flag.store(false, std::memory_order_relaxed);
  }
  std::atomic<int> duplicate_count{0};

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&] {
      for (;;) {
        const std::uint64_t value = next_to_produce.fetch_add(1, std::memory_order_relaxed);
        if (value >= kTotal) {
          break;
        }
        while (!ring.try_enqueue(value)) {
          std::this_thread::yield();
        }
      }
    });
  }

  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      while (consumed_count.load(std::memory_order_relaxed) < kTotal) {
        std::uint64_t value;
        if (ring.try_dequeue(value)) {
          if (seen[value].exchange(true, std::memory_order_relaxed)) {
            duplicate_count.fetch_add(1, std::memory_order_relaxed);
          }
          consumed_count.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto& t : producers) {
    t.join();
  }
  for (auto& t : consumers) {
    t.join();
  }

  EXPECT_EQ(duplicate_count.load(), 0);
  EXPECT_EQ(consumed_count.load(), kTotal);
  for (std::uint64_t i = 0; i < kTotal; ++i) {
    ASSERT_TRUE(seen[i].load()) << "value " << i << " was never dequeued";
  }
}
