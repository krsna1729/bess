// SPDX-License-Identifier: BSD-3-Clause

// Buffer in process (was bessctl/module_tests/buffer.py; the run under
// traffic is in tests/live/module_traffic_test.cc).

#include <gtest/gtest.h>

#include <random>

#include "pb/module_msg.pb.h"
#include "pktbatch.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::ModuleHarness;

// Packets are withheld until a full batch has arrived; the full batch then
// goes out in arrival order.
TEST(BufferTest, WithholdsPacketsUntilAFullBatch) {
  ModuleHarness h;
  Module *buf = h.Create("Buffer", bess::pb::BufferArg());
  ASSERT_NE(nullptr, buf);
  std::mt19937 rng(1);
  const Bytes pkt1 =
      bess::testing::TcpPacket(FlowSpec{.sip = "22.22.22.22", .dip = "22.22.22.22"}, rng);
  const Bytes pkt2 =
      bess::testing::TcpPacket(FlowSpec{.sip = "32.22.22.22", .dip = "22.22.22.22"}, rng);
  constexpr size_t kBatch = bess::PacketBatch::kMaxBurst;
  ASSERT_EQ(32u, kBatch);

  for (size_t i = 0; i + 1 < kBatch; i++) {
    EXPECT_TRUE(h.Run(buf, 0, {pkt1}, {0})[0].empty()) << "packet " << i;
  }
  auto out = h.Run(buf, 0, {pkt2}, {0});
  ASSERT_EQ(kBatch, out[0].size());
  for (size_t i = 0; i + 1 < kBatch; i++) {
    EXPECT_EQ(pkt1, out[0][i]) << "packet " << i;
  }
  EXPECT_EQ(pkt2, out[0][kBatch - 1]);
}

}  // namespace
