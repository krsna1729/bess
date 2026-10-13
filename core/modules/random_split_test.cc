// SPDX-License-Identifier: BSD-3-Clause

// RandomSplit in process (was bessctl/module_tests/random_split.py, whose
// drop-rate cases ran Source -> Measure -> Rewrite -> RandomSplit -> Measure
// on a worker for a second; here the same ratio is counted over 6400
// packets).

#include <gtest/gtest.h>

#include <cmath>
#include <cerrno>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "pktbatch.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::ModuleHarness;

Module *CreateSplit(ModuleHarness &h, double drop_rate) {
  bess::pb::RandomSplitArg arg;
  arg.set_drop_rate(drop_rate);
  arg.add_gates(0);
  return h.Create("RandomSplit", arg);
}

TEST(RandomSplitTest, DropsNoneAtRateZero) {
  ModuleHarness h;
  Module *split = CreateSplit(h, 0);
  ASSERT_NE(nullptr, split);
  std::mt19937 rng(1);
  const Bytes pkt = bess::testing::UdpPacket(FlowSpec{}, rng);
  auto out = h.Run(split, 0, {pkt}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(pkt, out[0][0]);
}

TEST(RandomSplitTest, DropsAllAtRateOne) {
  ModuleHarness h;
  Module *split = CreateSplit(h, 1);
  ASSERT_NE(nullptr, split);
  std::mt19937 rng(1);
  EXPECT_TRUE(h.Run(split, 0, {bess::testing::UdpPacket(FlowSpec{}, rng)}, {0})[0].empty());
}

// The share of packets forwarded is 1 - drop_rate, within 0.05 (more than
// 8 standard deviations at 6400 packets).
TEST(RandomSplitTest, ForwardsOneMinusTheDropRate) {
  std::mt19937 rng(1);
  // A UDP and a TCP packet, alternating (the Rewrite templates).
  const Bytes udp = bess::testing::UdpPacket(FlowSpec{}, rng);
  const Bytes tcp = bess::testing::TcpPacket(FlowSpec{}, rng);
  std::vector<Bytes> burst;
  for (size_t i = 0; i < bess::PacketBatch::kMaxBurst; i++) {
    burst.push_back(i % 2 ? tcp : udp);
  }
  constexpr int kRuns = 200;

  for (double rate : {0.3, 0.5, 0.75, 0.9}) {
    ModuleHarness h;
    Module *split = CreateSplit(h, rate);
    ASSERT_NE(nullptr, split);
    size_t forwarded = 0;
    for (int i = 0; i < kRuns; i++) {
      // Named: a range-for over `h.Run(...)[0]` would iterate a vector of a
      // destroyed map before C++23's lifetime extension (clang 19).
      auto out = h.Run(split, 0, burst, {0});
      for (const Bytes &p : out[0]) {
        EXPECT_TRUE(p == udp || p == tcp) << "a forwarded packet is unchanged";
        forwarded++;
      }
    }
    const double ratio = static_cast<double>(forwarded) / (kRuns * burst.size());
    EXPECT_LE(std::abs(ratio - (1 - rate)), 0.05) << "drop rate " << rate << ": forwarded "
                                                  << ratio;
  }
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

bess::pb::RandomSplitArg SplitSettings(double drop_rate, std::initializer_list<int64_t> gates) {
  bess::pb::RandomSplitArg arg;
  arg.set_drop_rate(drop_rate);
  for (int64_t g : gates) {
    arg.add_gates(g);
  }
  return arg;
}

// D-129: "<name>/settings" holds the drop rate and the gates.
TEST(RandomSplitTest, SettingsResourceSetsRateAndGates) {
  ModuleHarness h;
  Module *m = h.Create("RandomSplit", SplitSettings(0, {0}), "rs");
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.SetResource("rs/settings", SplitSettings(0, {3})));
  std::mt19937 rng(1);
  std::vector<Bytes> in;
  for (int i = 0; i < 8; i++) {
    in.push_back(bess::testing::TcpPacket(FlowSpec{}, rng));
  }
  auto out = h.Run(m, 0, in, {0, 3});
  EXPECT_TRUE(out[0].empty());
  EXPECT_EQ(in.size(), out[3].size());

  ASSERT_EQ("", h.SetResource("rs/settings", SplitSettings(1, {3})));
  out = h.Run(m, 0, in, {0, 3});
  EXPECT_TRUE(out[3].empty());
}

TEST(RandomSplitTest, SettingsReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("RandomSplit", SplitSettings(0.5, {0, 1}), "rs"));
  const bess::pb::RandomSplitArg initial = SplitSettings(0.5, {0, 1});
  EXPECT_EQ("", h.ReadBackDiffers("rs/settings", {{nullptr, &initial}}));
  const bess::pb::RandomSplitArg s = SplitSettings(0.125, {2, 7, 4});
  ASSERT_EQ("", h.SetResource("rs/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("rs/settings", {{nullptr, &s}}));
}

TEST(RandomSplitTest, SettingsBadValuesRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("RandomSplit", SplitSettings(0, {0}), "rs"));
  EXPECT_NE("", h.SetResource("rs/settings", SplitSettings(-0.1, {0})));
  EXPECT_NE("", h.SetResource("rs/settings", SplitSettings(1.5, {0})));
  EXPECT_NE("", h.SetResource("rs/settings", SplitSettings(0, {-1})));
  EXPECT_NE("", h.SetResource("rs/settings", SplitSettings(0, {MAX_GATES + 1})));
  EXPECT_NE("", h.SetResource("rs/settings", SplitSettings(0, {int64_t{1} << 16})));
}

TEST(RandomSplitTest, SettingsRequirePausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("RandomSplit", SplitSettings(0, {0}), "rs"));
  EXPECT_TRUE(RequiresPausedWorkers("rs/settings"));
}

TEST(RandomSplitTest, SettersAreLegacyOnly) {
  ModuleHarness h;
  Module *m = h.Create("RandomSplit", SplitSettings(0, {0}), "rs");
  ASSERT_NE(nullptr, m);
  bess::pb::RandomSplitCommandSetDroprateArg rate;
  rate.set_drop_rate(0.5);
  EXPECT_EQ(ENOTSUP, h.Action(m, "set_droprate", rate).error().code());
  EXPECT_EQ(ENOTSUP,
            h.Action(m, "set_gates", bess::pb::RandomSplitCommandSetGatesArg()).error().code());
  h.MustCommand(m, "set_droprate", rate);
  const bess::pb::RandomSplitArg s = SplitSettings(0.5, {0});
  EXPECT_EQ("", h.ReadBackDiffers("rs/settings", {{nullptr, &s}}));
}

}  // namespace
