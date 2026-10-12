// SPDX-License-Identifier: BSD-3-Clause

// IPChecksum in process (was bessctl/module_tests/ip_checksum.py).

#include <gtest/gtest.h>

#include <functional>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

// eth / tags / IP(1.2.3.4 -> 2.3.4.5, ttl 98) / UDP / payload, the IP
// checksum zeroed when `wrong`.
Bytes Packet(const std::function<void(PacketBuilder &)> &tags, bool wrong) {
  PacketBuilder b;
  b.Ether("12:34:de:ad:be:ef", "de:ad:be:ef:12:34");
  tags(b);
  b.IPv4("1.2.3.4", "2.3.4.5");
  b.Last<PacketBuilder::IPv4Hdr>().ttl = 98;
  if (wrong) {
    b.Last<PacketBuilder::IPv4Hdr>().checksum = 0x0000;
  }
  return b.Udp(10001, 10002).Payload("helloworldhelloworldhelloworld").Build();
}

// A zero IP checksum is replaced by the right one, untagged, under an
// 802.1Q tag and under 802.1ad + 802.1Q.
TEST(IPChecksumTest, RecomputesTheIpChecksum) {
  ModuleHarness h;
  Module *ipc = h.Create("IPChecksum", bess::pb::IPChecksumArg());
  ASSERT_NE(nullptr, ipc);

  const std::pair<const char *, std::function<void(PacketBuilder &)>> cases[] = {
      {"untagged", [](PacketBuilder &) {}},
      {"802.1Q", [](PacketBuilder &b) { b.Dot1Q(6); }},
      {"QinQ", [](PacketBuilder &b) { b.Dot1AD(5).Dot1Q(6); }},
  };
  for (const auto &[name, tags] : cases) {
    const Bytes in = Packet(tags, true);
    const Bytes want = Packet(tags, false);
    ASSERT_NE(in, want) << name;
    auto out = h.Run(ipc, 0, {in}, {0});
    ASSERT_EQ(1u, out[0].size()) << name;
    EXPECT_EQ(want, out[0][0]) << name;
  }
}

}  // namespace
