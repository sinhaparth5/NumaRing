#include "numaring/numaring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// These tests run against however many NUMA nodes this machine
// actually reports (numaring::node_count()) — on a single-node
// host (the common case for dev/CI) Queue<T> degrades to exactly one
// NodeLocalRingBuffer and every one of these still has to hold. The
// cross-node spill/steal *trigger* path (Queue::transfer_from_other /
// transfer_into_other only firing when node_count() > 1) needs a real
// multi-node machine to exercise directly — see the GCP testing
// workflow in CLAUDE.md. NodeLocalRingBuffer.CrossInstanceBulkTransfer
// in ring_buffer_test.cpp covers the underlying bulk-transfer
// mechanism those methods call, independent of node_count().

TEST(Queue, NodeCountMatchesTopology) {
  numaring::Queue<int> queue(16);
  EXPECT_EQ(queue.node_count(), static_cast<std::size_t>(numaring::node_count()));
}

TEST(Queue, SingleThreadedFifoOrderPerNode) {
  // With node_count() sub-queues, FIFO across the whole Queue is
  // only guaranteed per-node (each node's own items stay in order;
  // interleaving across nodes isn't ordered) — on the common
  // single-node host this is exactly plain FIFO.
  numaring::Queue<int> queue(8);
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(queue.try_enqueue(i));
  }
  int out = -1;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(queue.try_dequeue(out));
    EXPECT_EQ(out, i);
  }
}

TEST(Queue, TryDequeueFailsWhenEmpty) {
  numaring::Queue<int> queue(8);
  int out;
  EXPECT_FALSE(queue.try_dequeue(out));
}

TEST(Queue, FailsOnlyWhenEveryNodeIsGenuinelyFull) {
  numaring::Queue<int> queue(4);
  const std::size_t total_capacity = queue.node_count() * 4;
  for (std::size_t i = 0; i < total_capacity; ++i) {
    ASSERT_TRUE(queue.try_enqueue(static_cast<int>(i)))
        << "failed enqueueing item " << i << " of " << total_capacity;
  }
  EXPECT_FALSE(queue.try_enqueue(-1));
}

TEST(Queue, MoveOnlyTypeWorks) {
  numaring::Queue<std::unique_ptr<int>> queue(8);
  ASSERT_TRUE(queue.try_enqueue(std::make_unique<int>(7)));

  std::unique_ptr<int> out;
  ASSERT_TRUE(queue.try_dequeue(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 7);
}

namespace {

constexpr std::uint64_t kQueueStressTotal = 200'000;
constexpr int kQueueStressProducers = 4;
constexpr int kQueueStressConsumers = 4;

}  // namespace

TEST(Queue, ConcurrentMpmcStress) {
  // Same property NodeLocalRingBuffer.ConcurrentMpmcStress checks,
  // through the Queue<T> routing/overflow/underflow layer on top: no
  // drops, no duplicates, regardless of how many nodes route to.
  numaring::Queue<std::uint64_t> queue(64);
  std::atomic<std::uint64_t> next_to_produce{0};
  std::atomic<std::uint64_t> consumed_count{0};
  std::vector<std::atomic<bool>> seen(kQueueStressTotal);
  for (auto& flag : seen) {
    flag.store(false, std::memory_order_relaxed);
  }
  std::atomic<int> duplicate_count{0};

  std::vector<std::thread> producers;
  for (int p = 0; p < kQueueStressProducers; ++p) {
    producers.emplace_back([&] {
      for (;;) {
        const std::uint64_t value =
            next_to_produce.fetch_add(1, std::memory_order_relaxed);
        if (value >= kQueueStressTotal) {
          break;
        }
        while (!queue.try_enqueue(value)) {
          std::this_thread::yield();
        }
      }
    });
  }

  std::vector<std::thread> consumers;
  for (int c = 0; c < kQueueStressConsumers; ++c) {
    consumers.emplace_back([&] {
      while (consumed_count.load(std::memory_order_relaxed) < kQueueStressTotal) {
        std::uint64_t value;
        if (queue.try_dequeue(value)) {
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
  EXPECT_EQ(consumed_count.load(), kQueueStressTotal);
  for (std::uint64_t i = 0; i < kQueueStressTotal; ++i) {
    ASSERT_TRUE(seen[i].load()) << "value " << i << " was never dequeued";
  }
}
