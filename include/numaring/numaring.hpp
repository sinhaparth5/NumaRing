#pragma once

// numaring: a cache-conscious, topology-aware MPMC queue for high-core
// NUMA architectures.
//
// This is Phase 1 scaffolding only. Topology discovery (Phase 2), the
// node-local ring buffer (Phase 3), and cross-node work stealing
// (Phase 4) are not implemented yet — see docs/ROADMAP.md.

#include "numaring/detail/cache.hpp"
