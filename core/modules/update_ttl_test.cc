// SPDX-License-Identifier: BSD-3-Clause

// UpdateTTL in process (was bessctl/module_tests/update_ttl.py; the run
// under traffic is in tests/live/module_traffic_test.cc).

#include <gtest/gtest.h>

#include <cstdint>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

// A TCP packet with `ttl` and its IP checksum computed for it.
Bytes TcpWithTtl(uint8_t ttl) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("10.1.2.3", "10.4.5.6");
  b.Last<PacketBuilder::IPv4Hdr>().ttl = ttl;
  return b.Tcp(1234, 80).PadTo(60).Build();
}

// TTL 2 leaves with TTL 1 and the checksum updated to match.
TEST(UpdateTTLTest, DecrementsTheTtl) {
  ModuleHarness h;
  Module *uttl = h.Create("UpdateTTL", bess::pb::EmptyArg());
  ASSERT_NE(nullptr, uttl);
  auto out = h.Run(uttl, 0, {TcpWithTtl(2)}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(TcpWithTtl(1), out[0][0]);
}

// TTL 0 and TTL 1 are dropped; the module forwards in between.
TEST(UpdateTTLTest, DropsExpiringPackets) {
  ModuleHarness h;
  Module *uttl = h.Create("UpdateTTL", bess::pb::EmptyArg());
  ASSERT_NE(nullptr, uttl);

  EXPECT_TRUE(h.Run(uttl, 0, {TcpWithTtl(0)}, {0})[0].empty());
  auto out = h.Run(uttl, 0, {TcpWithTtl(2)}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(TcpWithTtl(1), out[0][0]);
  EXPECT_TRUE(h.Run(uttl, 0, {TcpWithTtl(1)}, {0})[0].empty());
}

}  // namespace
