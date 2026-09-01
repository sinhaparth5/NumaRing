#pragma once

#include "numaring/detail/cache.hpp"
#include "numaring/detail/numa_allocator.hpp"
#include "numaring/topology.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define NUMARING_HAVE_MM_PREFETCH 1
#endif

namespace numaring {

// A bounded, lock-free, multi-producer multi-consumer ring buffer
// whose backing storage is pinned to a single NUMA node.
//
// This is the *node-local* queue: in the eventual hierarchical
// design, one instance lives on each NUMA node and Phase 4 adds a
// cross-node work-stealing layer on top of several of these. Used on
// its own it's already a complete general-purpose bounded MPMC
// queue — that's the fallback path when numa_supported() is false.
//
// Implementation follows Dmitry Vyukov's bounded MPMC queue
// algorithm: every slot carries its own sequence number recording
// whose turn it is, so producers/consumers only contend on the slot
// they actually land on rather than CAS-looping a single shared
// head/tail cache line.
template <typename T>
class NodeLocalRingBuffer {
  static_assert(std::is_default_constructible_v<T>,
                "NodeLocalRingBuffer pre-constructs every slot's T in the "
                "backing NUMA-pinned storage, so T must be default-constructible");
  static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
                "NodeLocalRingBuffer moves/copies T in and out of slots");

 public:
  // `capacity` is rounded up internally to the next power of two (0
  // and 1 both become 1) so the slot index can be computed with a
  // mask instead of a modulo. `node` is the NUMA node id to pin the
  // backing storage to — see current_node() / node_count() in
  // topology.hpp for how callers pick one; it must be a valid,
  // currently-configured node.
  explicit NodeLocalRingBuffer(std::size_t capacity, int node = 0)
      : mask_(round_up_to_power_of_two(capacity) - 1),
        storage_(make_storage(capacity, node)),
        slots_(static_cast<Slot*>(storage_.get())) {
    for (std::size_t i = 0; i <= mask_; ++i) {
      new (&slots_[i]) Slot();
      slots_[i].sequence.get().store(i, std::memory_order_relaxed);
    }
  }

  // Not safe to run concurrently with any in-flight try_enqueue()/
  // try_dequeue() call on other threads — same contract as
  // destroying any other container while it's still in use.
  ~NodeLocalRingBuffer() {
    T discarded;
    while (try_dequeue(discarded)) {
    }
    for (std::size_t i = 0; i <= mask_; ++i) {
      slots_[i].~Slot();
    }
  }

  NodeLocalRingBuffer(const NodeLocalRingBuffer&) = delete;
  NodeLocalRingBuffer& operator=(const NodeLocalRingBuffer&) = delete;

  // Total number of slots (the rounded-up capacity passed to the
  // constructor), not the number of elements currently stored.
  std::size_t capacity() const noexcept { return mask_ + 1; }

  // True if the backing storage actually landed on the requested
  // NUMA node (false on the non-NUMA fallback path).
  bool is_numa_local() const noexcept { return storage_.is_numa_allocated(); }

  bool try_enqueue(const T& value) { return enqueue_impl(value); }
  bool try_enqueue(T&& value) { return enqueue_impl(std::move(value)); }

  // Dequeues into `out`. Returns false if the queue is empty.
  bool try_dequeue(T& out) {
    Slot* slot;
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      slot = &slots_[pos & mask_];
      const std::size_t seq = slot->sequence.get().load(std::memory_order_acquire);
      const auto diff =
          static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;  // empty
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
    out = std::move(slot->value);
    // Prefetch the slot this consumer will land on one full lap from
    // now (same ring position, next generation) into L1 ahead of
    // time, hiding memory latency for tight consumer loops. This is
    // advisory only — a mispredicted target just costs a wasted
    // fill, never correctness, since the real synchronization is the
    // sequence-number acquire/release above.
    prefetch_slot(pos + capacity());
    slot->sequence.get().store(pos + mask_ + 1, std::memory_order_release);
    return true;
  }

 private:
  struct Slot {
    CacheLinePadded<std::atomic<std::size_t>> sequence{};
    T value{};
  };

  template <typename U>
  bool enqueue_impl(U&& value) {
    Slot* slot;
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      slot = &slots_[pos & mask_];
      const std::size_t seq = slot->sequence.get().load(std::memory_order_acquire);
      const auto diff =
          static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        return false;  // full
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
    slot->value = std::forward<U>(value);
    slot->sequence.get().store(pos + 1, std::memory_order_release);
    return true;
  }

  void prefetch_slot(std::size_t pos) const noexcept {
#if defined(NUMARING_HAVE_MM_PREFETCH)
    _mm_prefetch(reinterpret_cast<const char*>(&slots_[pos & mask_]), _MM_HINT_T0);
#else
    (void)pos;
#endif
  }

  // Validates `node` (assert-only — a no-op in release builds, same
  // as any other precondition check) and builds the backing storage.
  // Split out so the node id can be checked before it's handed to
  // numa_alloc_onnode(), which has undefined behavior on an invalid
  // node — doing that check inline in the mem-initializer list for
  // storage_ would run after construction already happened.
  static detail::NumaBuffer make_storage(std::size_t capacity, int node) {
    assert(node >= 0 && node < node_count() &&
           "node must be a valid, currently-configured NUMA node id");
    const std::size_t slot_count = round_up_to_power_of_two(capacity);
    return detail::NumaBuffer(sizeof(Slot) * slot_count, alignof(Slot), node);
  }

  static std::size_t round_up_to_power_of_two(std::size_t v) {
    if (v <= 1) {
      return 1;
    }
    --v;
    for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
      v |= v >> shift;
    }
    return v + 1;
  }

  const std::size_t mask_;
  detail::NumaBuffer storage_;
  Slot* const slots_;

  alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0};
};

}  // namespace numaring
