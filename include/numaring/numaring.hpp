#pragma once

// numaring: a cache-conscious, topology-aware MPMC queue for high-core
// NUMA architectures.
//
// numaring::Queue<T> (queue.hpp) is the top-level hierarchical queue
// described in docs/NumaRing Theory.pdf — one NodeLocalRingBuffer<T>
// per NUMA node plus batched cross-node work stealing. See
// docs/ROADMAP.md for what's implemented so far.

#include "numaring/detail/cache.hpp"
#include "numaring/queue.hpp"
#include "numaring/ring_buffer.hpp"
#include "numaring/topology.hpp"
