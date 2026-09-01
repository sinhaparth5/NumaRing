#pragma once

#include "numaring/topology.hpp"

#include <numa.h>

#include <cstddef>
#include <new>
#include <utility>

namespace numaring::detail {

// RAII wrapper around numa_alloc_onnode()/numa_free(): pins a raw
// buffer to a single NUMA node's local memory controller, so slot
// storage built on top of it never touches remote DRAM during normal
// (non-work-stealing) operation.
//
// Falls back to plain aligned heap allocation when NUMA isn't
// supported (numa_supported() == false, see topology.hpp) or when
// numa_alloc_onnode() itself fails, so callers work correctly on
// non-NUMA dev/CI hosts — just without the node-pinning guarantee.
class NumaBuffer {
 public:
  // `bytes` must be > 0. `alignment` must be a power of two; the
  // fallback path honors it exactly, while numa_alloc_onnode()
  // already returns page-aligned memory (4 KiB+), which satisfies
  // any alignment this library asks for (128-byte cache lines).
  // `node` must be a valid, currently-configured NUMA node id (e.g.
  // one in [0, numaring::node_count())) — passing an invalid node id
  // is undefined behavior in numa_alloc_onnode() itself.
  NumaBuffer(std::size_t bytes, std::size_t alignment, int node)
      : bytes_(bytes), alignment_(alignment) {
    if (numa_supported()) {
      ptr_ = ::numa_alloc_onnode(bytes_, node);
    }
    if (ptr_ != nullptr) {
      numa_allocated_ = true;
    } else {
      ptr_ = ::operator new(bytes_, std::align_val_t{alignment_});
      numa_allocated_ = false;
    }
  }

  ~NumaBuffer() {
    if (ptr_ == nullptr) {
      return;
    }
    if (numa_allocated_) {
      ::numa_free(ptr_, bytes_);
    } else {
      ::operator delete(ptr_, std::align_val_t{alignment_});
    }
  }

  NumaBuffer(const NumaBuffer&) = delete;
  NumaBuffer& operator=(const NumaBuffer&) = delete;

  NumaBuffer(NumaBuffer&& other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)),
        bytes_(other.bytes_),
        alignment_(other.alignment_),
        numa_allocated_(other.numa_allocated_) {}
  NumaBuffer& operator=(NumaBuffer&&) = delete;

  void* get() const noexcept { return ptr_; }

  // True if this buffer actually landed on the requested NUMA node.
  // False means the fallback path was taken — callers that care
  // about the node-local guarantee (rather than just correctness)
  // can use this to detect a degraded (non-NUMA) environment.
  bool is_numa_allocated() const noexcept { return numa_allocated_; }

 private:
  void* ptr_ = nullptr;
  std::size_t bytes_ = 0;
  std::size_t alignment_ = 0;
  bool numa_allocated_ = false;
};

}  // namespace numaring::detail
