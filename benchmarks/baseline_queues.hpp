#pragma once

// Phase 5 baseline adapters (docs/ROADMAP.md): thin wrappers giving
// each comparison queue the same try_enqueue(T)/try_dequeue(T&) bool
// interface as numaring::Queue, so latency_suite_benchmark.cpp can
// drive all of them through one generic harness.
//
// - BoostLockfreeQueue<T>: boost::lockfree::queue<T>, fixed-capacity
//   (fixed_sized<true> — a real bounded ring, comparable to
//   NodeLocalRingBuffer, not the default "grows via a free list"
//   mode). Only compiled when NUMARING_HAVE_BOOST is set (see
//   benchmarks/CMakeLists.txt) — Boost isn't a hard project
//   dependency, so this baseline is skipped rather than required.
// - MoodycamelQueue<T>: moodycamel::ConcurrentQueue<T>, which has no
//   built-in hard capacity, so enqueue is gated on size_approx()
//   against the same capacity the other three use, to keep the
//   comparison fair (bounded memory, same backpressure behavior).
// - MutexQueue<T>: std::deque<T> behind a std::mutex, bounded the
//   same way — the "traditional" baseline the roadmap and theory doc
//   are contrasting NumaRing against.

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

#if NUMARING_HAVE_BOOST
#include <boost/lockfree/queue.hpp>
#endif

#include <concurrentqueue.h>

namespace numaring::bench {

#if NUMARING_HAVE_BOOST
template <typename T>
class BoostLockfreeQueue {
 public:
  explicit BoostLockfreeQueue(std::size_t capacity) : queue_(capacity) {}

  bool try_enqueue(T value) { return queue_.push(value); }

  bool try_dequeue(T& out) { return queue_.pop(out); }

 private:
  boost::lockfree::queue<T, boost::lockfree::fixed_sized<true>> queue_;
};
#endif

template <typename T>
class MoodycamelQueue {
 public:
  explicit MoodycamelQueue(std::size_t capacity) : capacity_(capacity) {}

  bool try_enqueue(T value) {
    if (queue_.size_approx() >= capacity_) {
      return false;
    }
    return queue_.try_enqueue(std::move(value));
  }

  bool try_dequeue(T& out) { return queue_.try_dequeue(out); }

 private:
  moodycamel::ConcurrentQueue<T> queue_;
  std::size_t capacity_;
};

template <typename T>
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t capacity) : capacity_(capacity) {}

  bool try_enqueue(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deque_.size() >= capacity_) {
      return false;
    }
    deque_.push_back(std::move(value));
    return true;
  }

  bool try_dequeue(T& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deque_.empty()) {
      return false;
    }
    out = std::move(deque_.front());
    deque_.pop_front();
    return true;
  }

 private:
  std::mutex mutex_;
  std::deque<T> deque_;
  std::size_t capacity_;
};

}  // namespace numaring::bench
