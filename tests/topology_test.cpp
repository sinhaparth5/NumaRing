#include "numaring/numaring.hpp"

#include <gtest/gtest.h>

// These assertions hold whether or not the machine running the test
// actually has NUMA hardware — the fallback path (numa_supported()
// == false) must be just as well-defined as the real one, since CI
// and most dev laptops are single-node.

TEST(Topology, NodeCountIsAtLeastOne) {
  EXPECT_GE(numaring::node_count(), 1);
}

TEST(Topology, UnsupportedImpliesSingleNode) {
  if (!numaring::numa_supported()) {
    EXPECT_EQ(numaring::node_count(), 1);
  }
}

TEST(Topology, CurrentNodeIsWithinRange) {
  const int node = numaring::current_node();
  EXPECT_GE(node, 0);
  EXPECT_LT(node, numaring::node_count());
}

TEST(Topology, CurrentNodeIsStableAcrossRepeatedCalls) {
  // Not pinned to a core, so this isn't a guarantee on real hardware,
  // but back-to-back calls with no intervening blocking/yielding
  // should very rarely observe a migration.
  const int first = numaring::current_node();
  const int second = numaring::current_node();
  EXPECT_EQ(first, second);
}

TEST(Topology, NodeOfCpuZeroIsWithinRange) {
  const int node = numaring::node_of_cpu(0);
  EXPECT_GE(node, 0);
  EXPECT_LT(node, numaring::node_count());
}

TEST(Topology, NodeOfNegativeCpuFallsBackToZero) {
  EXPECT_EQ(numaring::node_of_cpu(-1), 0);
}

TEST(Topology, NodeOfImplausibleCpuFallsBackToZero) {
  EXPECT_EQ(numaring::node_of_cpu(1 << 20), 0);
}
