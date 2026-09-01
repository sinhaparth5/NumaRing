#pragma once

#include <cstddef>

namespace numaring {

// Cache lines on modern server CPUs are 64 bytes, but the L2 spatial
// prefetcher on many microarchitectures pulls memory in 128-byte
// (dual-cache-line) pairs. Padding to 128 bytes rather than 64 keeps
// independent atomic tracking variables from landing in the same
// prefetch window, which would otherwise false-share even though the
// variables sit on different 64-byte cache lines.
inline constexpr std::size_t kCacheLineSize = 128;

// Wraps a value so it occupies its own 128-byte-aligned block. Use this
// for atomic head/tail/state variables written by different threads —
// and, in the NUMA-local design, potentially different sockets — so
// they never share a hardware prefetch window.
template <typename T>
struct alignas(kCacheLineSize) CacheLinePadded {
  T value;

  CacheLinePadded() = default;
  constexpr CacheLinePadded(const T& v) : value(v) {}
  constexpr CacheLinePadded(T&& v) : value(static_cast<T&&>(v)) {}

  T& get() noexcept { return value; }
  const T& get() const noexcept { return value; }

  operator T&() noexcept { return value; }
  operator const T&() const noexcept { return value; }
};

static_assert(sizeof(CacheLinePadded<int>) == kCacheLineSize,
              "CacheLinePadded must occupy exactly one 128-byte block");

}  // namespace numaring
