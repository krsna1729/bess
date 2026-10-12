// SPDX-License-Identifier: BSD-3-Clause

// Bypass in process (was bessctl/module_tests/bypass.py).

#include <gtest/gtest.h>

#include <random>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::ModuleHarness;

// A packet goes through unchanged; nothing in, nothing out.
TEST(BypassTest, PassesPacketsUnchanged) {
  ModuleHarness h;
  Module *bp = h.Create("Bypass", bess::pb::BypassArg());
  ASSERT_NE(nullptr, bp);
  std::mt19937 rng(1);
  const Bytes pkt = bess::testing::UdpPacket(FlowSpec{}, rng);

  auto out = h.Run(bp, 0, {pkt}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(pkt, out[0][0]);

  EXPECT_TRUE(h.Run(bp, 0, {}, {0})[0].empty());
}

}  // namespace
