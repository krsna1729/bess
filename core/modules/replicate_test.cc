// SPDX-License-Identifier: BSD-3-Clause

// Replicate in process (was bessctl/module_tests/replicate.py; the runs
// under traffic are in tests/live/module_traffic_test.cc).

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
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

// The configured list is the module's whole contract: the original leaves on
// the first configured gate, not on the hardcoded gate 0.
TEST(ReplicateTest, TheOriginalLeavesOnTheFirstConfiguredGate) {
  ModuleHarness h;
  Module *rep = h.Create("Replicate", Gates({2, 3}));
  ASSERT_NE(nullptr, rep);
  std::random_device rd;
  std::mt19937 rng(rd());
  const Bytes pkt =
      bess::testing::TcpPacket(FlowSpec{.sip = "22.22.22.22", .dip = "22.22.22.22"}, rng);

  auto out = h.Run(rep, 0, {pkt}, {0, 1, 2, 3});
  EXPECT_TRUE(out[0].empty());
  EXPECT_TRUE(out[1].empty());
  ASSERT_EQ(1u, out[2].size()) << "the original";
  EXPECT_EQ(pkt, out[2][0]);
  ASSERT_EQ(1u, out[3].size()) << "the copy";
  EXPECT_EQ(pkt, out[3][0]);
}

// Gates are int64 on the wire and were narrowed unchecked: each must name
// one of the module's 32 output gates.
TEST(ReplicateTest, GatesAreValidated) {
  ModuleHarness h;
  const std::vector<std::vector<int64_t>> bad = {
      {32}, {-1}, {65536}, {0, 1, int64_t{1} << 32}};
  for (const auto &gates : bad) {
    EXPECT_NE("", h.CreateError("Replicate", Gates(gates))) << Gates(gates).ShortDebugString();
  }

  Module *rep = h.Create("Replicate", Gates({0, 1}));
  ASSERT_NE(nullptr, rep);

  std::mt19937 rng(1);
  const Bytes pkt = bess::testing::UdpPacket(FlowSpec{}, rng);
  auto out = h.Run(rep, 0, {pkt}, {0, 1, 2});
  ASSERT_EQ(1u, out[0].size());
  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(pkt, out[1][0]);
  EXPECT_TRUE(out[2].empty());
}

// True when "<resource>"'s codec says applying it requires paused workers.
bool RequiresPausedWorkers(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  EXPECT_NE(nullptr, r) << resource;
  if (r == nullptr) {
    return false;
  }
  const bess::framework::ResourceCodec *codec =
      bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  EXPECT_NE(nullptr, codec) << resource;
  return codec != nullptr && codec->requires_paused_workers();
}

// D-129: "<name>/settings" holds the gates.
TEST(ReplicateTest, SettingsResourceSetsGates) {
  ModuleHarness h;
  Module *rep = h.Create("Replicate", Gates({0, 1}), "rp");
  ASSERT_NE(nullptr, rep);
  ASSERT_EQ("", h.SetResource("rp/settings", Gates({0, 2})));
  std::mt19937 rng(1);
  const Bytes pkt = bess::testing::TcpPacket(FlowSpec{}, rng);
  auto out = h.Run(rep, 0, {pkt}, {0, 1, 2});
  EXPECT_EQ(1u, out[0].size());
  EXPECT_TRUE(out[1].empty());
  EXPECT_EQ(1u, out[2].size());
}

TEST(ReplicateTest, SettingsReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Replicate", Gates({0, 1}), "rp"));
  const bess::pb::ReplicateArg initial = Gates({0, 1});
  EXPECT_EQ("", h.ReadBackDiffers("rp/settings", {{nullptr, &initial}}));
  const bess::pb::ReplicateArg s = Gates({0, 5, 31});
  ASSERT_EQ("", h.SetResource("rp/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("rp/settings", {{nullptr, &s}}));
}

TEST(ReplicateTest, SettingsBadGatesRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Replicate", Gates({0, 1}), "rp"));
  EXPECT_NE("", h.SetResource("rp/settings", Gates({32})));
  EXPECT_NE("", h.SetResource("rp/settings", Gates({-1})));
  const bess::pb::ReplicateArg unchanged = Gates({0, 1});
  EXPECT_EQ("", h.ReadBackDiffers("rp/settings", {{nullptr, &unchanged}}));
}

TEST(ReplicateTest, SettingsRequirePausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Replicate", Gates({0}), "rp"));
  EXPECT_TRUE(RequiresPausedWorkers("rp/settings"));
}

}  // namespace
