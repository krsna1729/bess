// SPDX-License-Identifier: BSD-3-Clause

// Replicate in process (was bessctl/module_tests/replicate.py; the runs
// under traffic are in tests/live/module_traffic_test.cc).

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <random>
#include <vector>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::ModuleHarness;

bess::pb::ReplicateArg Gates(const std::vector<int64_t> &gates) {
  bess::pb::ReplicateArg arg;
  for (int64_t g : gates) {
    arg.add_gates(g);
  }
  return arg;
}

// Each gate gets one copy of the packet.
TEST(ReplicateTest, CopiesEachPacketToEveryGate) {
  ModuleHarness h;
  Module *rep = h.Create("Replicate", Gates({0, 1, 2}));
  ASSERT_NE(nullptr, rep);
  std::mt19937 rng(1);
  const Bytes pkt =
      bess::testing::TcpPacket(FlowSpec{.sip = "22.22.22.22", .dip = "22.22.22.22"}, rng);

  auto out = h.Run(rep, 0, {pkt}, {0, 1, 2});
  for (gate_idx_t g : {0, 1, 2}) {
    ASSERT_EQ(1u, out[g].size()) << "gate " << g;
    EXPECT_EQ(pkt, out[g][0]) << "gate " << g;
  }
}

// Gates are int64 on the wire and were narrowed unchecked: each must name
// one of the module's 32 output gates, and a refused set_gates changes
// nothing.
TEST(ReplicateTest, GatesAreValidated) {
  ModuleHarness h;
  const std::vector<std::vector<int64_t>> bad = {
      {32}, {-1}, {65536}, {0, 1, int64_t{1} << 32}};
  for (const auto &gates : bad) {
    EXPECT_NE("", h.CreateError("Replicate", Gates(gates))) << Gates(gates).ShortDebugString();
  }

  Module *rep = h.Create("Replicate", Gates({0, 1}));
  ASSERT_NE(nullptr, rep);
  bess::pb::ReplicateCommandSetGatesArg set;
  set.add_gates(2);
  set.add_gates(40);
  EXPECT_EQ(EINVAL, h.Command(rep, "set_gates", set).error().code());

  std::mt19937 rng(1);
  const Bytes pkt = bess::testing::UdpPacket(FlowSpec{}, rng);
  auto out = h.Run(rep, 0, {pkt}, {0, 1, 2});
  ASSERT_EQ(1u, out[0].size());
  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(pkt, out[1][0]);
  EXPECT_TRUE(out[2].empty());
}

}  // namespace
