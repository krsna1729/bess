// SPDX-License-Identifier: BSD-3-Clause

// RandomSplit in process (was bessctl/module_tests/random_split.py, whose
// drop-rate cases ran Source -> Measure -> Rewrite -> RandomSplit -> Measure
// on a worker for a second; here the same ratio is counted over 6400
// packets).

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

#include "pb/module_msg.pb.h"
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
      for (const Bytes &p : h.Run(split, 0, burst, {0})[0]) {
        EXPECT_TRUE(p == udp || p == tcp) << "a forwarded packet is unchanged";
        forwarded++;
      }
    }
    const double ratio = static_cast<double>(forwarded) / (kRuns * burst.size());
    EXPECT_LE(std::abs(ratio - (1 - rate)), 0.05) << "drop rate " << rate << ": forwarded "
                                                  << ratio;
  }
}

}  // namespace
