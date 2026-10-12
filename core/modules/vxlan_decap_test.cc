// SPDX-License-Identifier: BSD-3-Clause

// VXLANDecap in process (was bessctl/module_tests/vxlan.py; VXLANEncap's
// run under traffic is in tests/live/module_traffic_test.cc). VXLANDecap
// checks the outer headers through the tunnel library (M19, D-069): a valid
// frame on any UDP port is decapsulated, with or without a VLAN tag on the
// outer Ethernet header; anything that does not check is dropped.

#include <gtest/gtest.h>

#include <cstdint>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

Bytes Inner() {
  return PacketBuilder()
      .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4("10.0.0.1", "10.0.0.2")
      .Udp(1000, 2000)
      .Payload("helloworld")
      .Build();
}

// Ether [/ Dot1Q(7)] / IP / UDP(49152, dport) / VXLAN(flags, vni 0x1234) /
// Inner().
Bytes Outer(bool vlan, uint16_t dport, uint8_t flags = 0x08) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:bb", "02:00:00:00:00:aa");
  if (vlan) {
    b.Dot1Q(7);
  }
  b.IPv4("192.0.2.1", "192.0.2.2").Udp(49152, dport);
  constexpr uint32_t kVni = 0x1234;
  b.Payload(Bytes{flags, 0, 0, 0, static_cast<uint8_t>(kVni >> 16),
                  static_cast<uint8_t>(kVni >> 8), static_cast<uint8_t>(kVni), 0});
  b.Payload(Inner());
  return b.Build();
}

TEST(VXLANDecapTest, DecapsulatesValidFrames) {
  ModuleHarness h;
  for (bool vlan : {false, true}) {
    for (uint16_t dport : {4789, 8472}) {
      Module *vd = h.Create("VXLANDecap", bess::pb::VXLANDecapArg());
      ASSERT_NE(nullptr, vd);
      auto out = h.Run(vd, 0, {Outer(vlan, dport)}, {0});
      ASSERT_EQ(1u, out[0].size()) << "vlan " << vlan << " dport " << dport;
      EXPECT_EQ(Inner(), out[0][0]) << "vlan " << vlan << " dport " << dport;
    }
  }
}

TEST(VXLANDecapTest, DropsWhatDoesNotCheck) {
  ModuleHarness h;
  Module *vd = h.Create("VXLANDecap", bess::pb::VXLANDecapArg());
  ASSERT_NE(nullptr, vd);

  const Bytes i_flag_clear = Outer(false, 4789, 0x00);
  // An IPv6 outer header (the tunnel metadata is IPv4).
  const Bytes ipv6_outer = PacketBuilder()
                               .Ether("ff:ff:ff:ff:ff:ff", "00:00:00:00:00:00")
                               .IPv6("::1", "::1")
                               .Udp(53, 4789)
                               .Vxlan(1)
                               .Payload(Inner())
                               .Build();
  const Bytes not_udp = PacketBuilder()
                            .Ether("ff:ff:ff:ff:ff:ff", "00:00:00:00:00:00")
                            .IPv4("127.0.0.1", "127.0.0.1")
                            .Tcp(20, 80)
                            .Build();
  const Bytes valid = Outer(false, 4789);
  const Bytes truncated(valid.begin(), valid.begin() + 40);

  // Each alone, then all together.
  for (const Bytes &bad : {i_flag_clear, ipv6_outer, not_udp, truncated}) {
    EXPECT_TRUE(h.Run(vd, 0, {bad}, {0})[0].empty()) << bad.size() << " bytes";
  }
  EXPECT_TRUE(h.Run(vd, 0, {i_flag_clear, ipv6_outer, not_udp, truncated}, {0})[0].empty());
  // The same module still decapsulates a valid frame.
  auto out = h.Run(vd, 0, {valid}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(Inner(), out[0][0]);
}

}  // namespace
