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
//
// The CAS retry loops below intentionally have no pause/backoff.
// Phase 5 (docs/PHASE5_RESULTS.md) tried both a flat cpu_relax()-per-
// retry version and a growing bounded-exponential-backoff version and
// measured both on real 2-node NUMA hardware: both *helped* tail
// latency in the instrumented benchmark suite, but both also *cost*
// real, repeatable raw throughput (~17-30%, with the "smarter"
// exponential version costing more, not less) in an uninstrumented
// sustained-load driver — under true wall-to-wall sustained
// contention (no pacing between a thread's own retries), the fastest
// thing a losing CAS attempt can do is retry immediately, since the
// atomic RMW itself already serializes access via cache coherence.
// Reverted. Documented here rather than silently dropped, since it's
// a reasonable thing to try again and not worth re-discovering from
// scratch.
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

  // A racy snapshot of how many items are currently enqueued — two
  // relaxed loads, no CAS, no synchronization with concurrent
  // enqueuers/dequeuers. Not meant for correctness decisions (the
  // real count can change the instant this returns); it exists for
  // heuristics like Queue<T>'s work-stealing candidate selection,
  // where "is this queue worth stealing from at all" only needs to be
  // approximately right.
  std::size_t approx_size() const noexcept {
    const std::size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
    const std::size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
    return enq >= deq ? enq - deq : 0;
  }

  // On failure (queue full), `value` is left completely untouched —
  // not just "valid but unspecified" — so callers can retry the
  // exact same value elsewhere (e.g. Queue<T>'s cross-node overflow
  // handling in queue.hpp) without a fallback copy.
  bool try_enqueue(const T& value) { return enqueue_impl(value); }
  bool try_enqueue(T&& value) { return enqueue_impl(std::move(value)); }

  // Attempts to claim up to `max_count` contiguous ready slots with
  // a single CAS on the shared dequeue position — one atomic
  // read-modify-write touching the hot, cross-thread-contended
  // position counter instead of one per item — then moves each
  // claimed item into `out[0..return value)`. Returns the number
  // actually dequeued (0 if empty); can be less than `max_count`
  // when fewer than that are currently available.
  //
  // This is the "batched cross-node transfer" primitive from
  // docs/NumaRing Theory.pdf: moving a batch of N items this way
  // costs one CAS on this counter versus N CASes for N individual
  // try_dequeue() calls (e.g. 1 vs 16 → 93.75% fewer RMWs on the
  // line every other core on this queue is also contending for).
  // The per-slot sequence loads/stores below still touch N separate
  // (already thread-local-ish, non-contended) cache lines either
  // way — batching targets the one shared counter, not those.
  std::size_t try_dequeue_bulk(T* out, std::size_t max_count) {
    if (max_count == 0) {
      return 0;
    }
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    std::size_t count;
    for (;;) {
      count = count_ready(pos, max_count, /*for_dequeue=*/true);
      if (count == 0) {
        return 0;  // empty
      }
      if (dequeue_pos_.compare_exchange_weak(pos, pos + count, std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
        break;
      }
      // CAS failed — another consumer moved dequeue_pos_ first (pos
      // was refreshed to the new value); recompute readiness from
      // there and retry. (Phase 5 tried a pause-based backoff here;
      // measured it costing real throughput under sustained
      // contention — see the class-level comment on why there's no
      // backoff in this loop.)
    }
    for (std::size_t i = 0; i < count; ++i) {
      Slot& slot = slots_[(pos + i) & mask_];
      out[i] = std::move(slot.value);
      slot.sequence.get().store(pos + i + mask_ + 1, std::memory_order_release);
    }
    return count;
  }

  // Symmetric to try_dequeue_bulk(): claims up to `max_count`
  // contiguous free slots with a single CAS on the shared enqueue
  // position, then moves `in[0..return value)` into them. Returns
  // the number actually enqueued (0 if full); can be less than
  // `max_count` when fewer slots are currently free.
  std::size_t try_enqueue_bulk(T* in, std::size_t max_count) {
    if (max_count == 0) {
      return 0;
    }
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    std::size_t count;
    for (;;) {
      count = count_ready(pos, max_count, /*for_dequeue=*/false);
      if (count == 0) {
        return 0;  // full
      }
      if (enqueue_pos_.compare_exchange_weak(pos, pos + count, std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
        break;
      }
    }
    for (std::size_t i = 0; i < count; ++i) {
      Slot& slot = slots_[(pos + i) & mask_];
      slot.value = std::move(in[i]);
      slot.sequence.get().store(pos + i + 1, std::memory_order_release);
    }
    return count;
  }

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

  // Counts how many contiguous slots starting at `pos` are ready to
  // claim (up to `max_count`, and never more than one full lap),
  // without claiming anything. Used to decide how big a batch to
  // attempt before the actual CAS; if the CAS in the caller fails
  // because another thread moved the position counter first, the
  // caller re-derives `pos` and calls this again — so a stale count
  // observed here is always re-validated by that CAS, never acted on
  // directly. See try_dequeue_bulk() / try_enqueue_bulk().
  std::size_t count_ready(std::size_t pos, std::size_t max_count, bool for_dequeue) const noexcept {
    std::size_t count = 0;
    const std::size_t limit = max_count < capacity() ? max_count : capacity();
    while (count < limit) {
      const Slot& slot = slots_[(pos + count) & mask_];
      const std::size_t seq = slot.sequence.get().load(std::memory_order_acquire);
      const std::size_t expected = for_dequeue ? pos + count + 1 : pos + count;
      if (seq != expected) {
        break;
      }
      ++count;
    }
    return count;
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
