// SPDX-License-Identifier: BSD-3-Clause

// GenericDecap in process (was bessctl/module_tests/generic_decap.py; the
// runs under traffic are in tests/live/module_traffic_test.cc).

#include <gtest/gtest.h>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

// bytes=14 strips the Ethernet header: the IP packet is what is left.
TEST(GenericDecapTest, StripsTheEthernetHeader) {
  ModuleHarness h;
  bess::pb::GenericDecapArg arg;
  arg.set_bytes(14);
  Module *gd = h.Create("GenericDecap", arg);
  ASSERT_NE(nullptr, gd);

  auto ip_udp = [](PacketBuilder b) {
    b.IPv4("1.2.3.4", "2.3.4.5");
    b.Last<PacketBuilder::IPv4Hdr>().ttl = 98;
    return b.Udp(10001, 10002).Payload("helloworldhelloworldhelloworld").Build();
  };
  const Bytes in = ip_udp(PacketBuilder().Ether("12:34:de:ad:be:ef", "de:ad:be:ef:12:34"));
  const Bytes want = ip_udp(PacketBuilder());
  ASSERT_EQ(in.size(), want.size() + 14);

  auto out = h.Run(gd, 0, {in}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(want, out[0][0]);
}

}  // namespace
