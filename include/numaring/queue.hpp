#pragma once

#include "numaring/ring_buffer.hpp"
#include "numaring/topology.hpp"

#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace numaring {

// The top-level hierarchical MPMC queue: one NodeLocalRingBuffer<T>
// per NUMA node (see topology.hpp), with batched cross-node work
// stealing to rebalance load when a node-local sub-queue overflows or
// underflows. This is the design in docs/NumaRing Theory.pdf — Phase
// 4 in docs/ROADMAP.md.
//
// Producers/consumers route to their own NUMA node's sub-queue
// automatically via current_node(), so the common case (no
// contention across sockets) never touches another node's memory at
// all. Cross-node traffic only happens on the overflow/underflow slow
// path, and even then moves a whole batch per cross-node CAS instead
// of one item at a time. On a single-node host (numa_supported() ==
// false, or node_count() == 1) this degrades to exactly one
// NodeLocalRingBuffer with work stealing never triggered.
template <typename T>
class Queue {
  static_assert(std::is_default_constructible_v<T>,
                "Queue pre-constructs every slot's T in each node-local "
                "sub-queue, so T must be default-constructible");

 public:
  // Default batch size for cross-node transfers. 16 matches the
  // roadmap's target of "16 or 32 items" and the theory doc's
  // benchmark target: one batched CAS on a sub-queue's shared
  // position counter replaces 16 single-item CASes, a 15/16 =
  // 93.75% cut in atomic RMWs on that contended line.
  static constexpr std::size_t kDefaultBatchSize = 16;
  // Upper bound on batch_size_ — sized for a fixed-size stack buffer
  // in the transfer path (no heap allocation on the work-stealing
  // slow path). Matches the roadmap's upper option of 32.
  static constexpr std::size_t kMaxBatchSize = 32;

  // `per_node_capacity` is passed through to each node's
  // NodeLocalRingBuffer (see its own capacity rounding). One
  // sub-queue is constructed per NUMA node reported by node_count().
  // `batch_size` must be in [1, kMaxBatchSize].
  explicit Queue(std::size_t per_node_capacity, std::size_t batch_size = kDefaultBatchSize)
      : batch_size_(batch_size) {
    assert(batch_size_ >= 1 && batch_size_ <= kMaxBatchSize &&
           "batch_size must be in [1, kMaxBatchSize]");
    // Qualified: an unqualified node_count() here would resolve to
    // Queue::node_count() below (sub_queues_.size(), still 0 at this
    // point) rather than the topology query, leaving sub_queues_
    // permanently empty.
    const int nodes = numaring::node_count();
    sub_queues_.reserve(static_cast<std::size_t>(nodes));
    for (int node = 0; node < nodes; ++node) {
      sub_queues_.push_back(std::make_unique<NodeLocalRingBuffer<T>>(per_node_capacity, node));
    }
  }

  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  // Number of node-local sub-queues (== node_count() at construction
  // time; NUMA topology isn't expected to change at runtime).
  std::size_t node_count() const noexcept { return sub_queues_.size(); }

  // Enqueues on the calling thread's local NUMA sub-queue. If that's
  // full, spills a batch of its own oldest items onto another node's
  // sub-queue to make room, then retries locally. If every sub-queue
  // is completely full, falls back to a single-item attempt against
  // every other sub-queue directly before giving up.
  bool try_enqueue(T value) {
    NodeLocalRingBuffer<T>& local = local_queue();
    if (local.try_enqueue(std::move(value))) {
      return true;
    }
    if (transfer_from_other(local) > 0 && local.try_enqueue(std::move(value))) {
      return true;
    }
    for (auto& sub_queue : sub_queues_) {
      if (sub_queue.get() != &local && sub_queue->try_enqueue(std::move(value))) {
        return true;
      }
    }
    return false;
  }

  // Dequeues from the calling thread's local NUMA sub-queue. If it's
  // empty, steals a batch from another node's sub-queue into it,
  // then retries locally. If every sub-queue is completely empty,
  // falls back to checking every other sub-queue directly before
  // reporting empty.
  bool try_dequeue(T& out) {
    NodeLocalRingBuffer<T>& local = local_queue();
    if (local.try_dequeue(out)) {
      return true;
    }
    if (transfer_into_other(local) > 0 && local.try_dequeue(out)) {
      return true;
    }
    for (auto& sub_queue : sub_queues_) {
      if (sub_queue.get() != &local && sub_queue->try_dequeue(out)) {
        return true;
      }
    }
    return false;
  }

 private:
  NodeLocalRingBuffer<T>& local_queue() {
    return *sub_queues_[static_cast<std::size_t>(current_node())];
  }

  // Moves up to batch_size_ items from `from` to `to` with one
  // batched claim on each side. Returns the number actually moved.
  // If `to` had less room than `from` had available (a race — some
  // other thread filled `to` in between), the leftover items go back
  // onto `from`, which just freed exactly that many slots, so the
  // retry below almost always succeeds on its first attempt.
  //
  // Hysteresis: skips the attempt entirely if `from` doesn't clearly
  // have enough to offer (< 2x batch_size_). Under sustained
  // saturation, several threads can end up racing to steal from the
  // same near-empty source at once; each only has a handful of ready
  // slots to fight over, so most of those racing CAS attempts in
  // try_dequeue_bulk are doomed to fail and retry. approx_size() is a
  // racy snapshot (see its doc comment) — that's fine here, this is a
  // heuristic gate, not a correctness check: a stale "no" just sends
  // the caller to Queue's existing single-item fallback loop instead
  // (see try_enqueue/try_dequeue below), never a wrong result.
  std::size_t transfer_batch(NodeLocalRingBuffer<T>& from, NodeLocalRingBuffer<T>& to) {
    if (from.approx_size() < 2 * batch_size_) {
      return 0;
    }
    T batch[kMaxBatchSize];
    const std::size_t taken = from.try_dequeue_bulk(batch, batch_size_);
    if (taken == 0) {
      return 0;
    }
    const std::size_t placed = to.try_enqueue_bulk(batch, taken);
    for (std::size_t i = placed; i < taken; ++i) {
      while (!from.try_enqueue(std::move(batch[i]))) {
        std::this_thread::yield();
      }
    }
    return placed;
  }

  // Overflow path: spills a batch of `source`'s own items onto
  // whichever other sub-queue accepts them first, trying candidates
  // in a rotating order (via round_robin_) so overflow load spreads
  // across nodes instead of always piling onto one.
  std::size_t transfer_from_other(NodeLocalRingBuffer<T>& source) {
    return for_each_other_candidate(
        source, [&](NodeLocalRingBuffer<T>& candidate) { return transfer_batch(source, candidate); });
  }

  // Underflow path: steals a batch from whichever other sub-queue
  // has items first, same rotating candidate order.
  std::size_t transfer_into_other(NodeLocalRingBuffer<T>& destination) {
    return for_each_other_candidate(destination, [&](NodeLocalRingBuffer<T>& candidate) {
      return transfer_batch(candidate, destination);
    });
  }

  template <typename F>
  std::size_t for_each_other_candidate(NodeLocalRingBuffer<T>& self, F&& try_candidate) {
    const std::size_t n = sub_queues_.size();
    if (n <= 1) {
      return 0;  // no other node to trade with
    }
    // Candidate order starts from thread-local state, not a shared
    // counter: an earlier version used a single atomic round_robin_
    // fetch_add()'d by every thread on every overflow/underflow,
    // which under saturation (e.g. all producers overflowing at
    // once) meant every one of those threads contended on that same
    // cache line in addition to the actual cross-node transfer work.
    // thread_local state spreads the starting candidate across
    // threads (a different hash-based seed per thread, advancing
    // independently after) with zero cross-thread traffic — no
    // atomic at all, since nothing here needs to be exactly fair,
    // just spread out.
    thread_local std::size_t local_start =
        std::hash<std::thread::id>{}(std::this_thread::get_id());
    const std::size_t start = local_start++;
    for (std::size_t offset = 0; offset < n; ++offset) {
      NodeLocalRingBuffer<T>& candidate = *sub_queues_[(start + offset) % n];
      if (&candidate == &self) {
        continue;
      }
      const std::size_t moved = try_candidate(candidate);
      if (moved > 0) {
        return moved;
      }
    }
    return 0;
  }

  std::vector<std::unique_ptr<NodeLocalRingBuffer<T>>> sub_queues_;
  std::size_t batch_size_;
};

}  // namespace numaring
