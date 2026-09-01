#pragma once

// numaring: a cache-conscious, topology-aware MPMC queue for high-core
// NUMA architectures.
//
// Cross-node work stealing across multiple NodeLocalRingBuffer
// instances (Phase 4) is not implemented yet — see docs/ROADMAP.md.

#include "numaring/detail/cache.hpp"
#include "numaring/ring_buffer.hpp"
#include "numaring/topology.hpp"
