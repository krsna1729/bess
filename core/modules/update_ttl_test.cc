// SPDX-License-Identifier: BSD-3-Clause

// UpdateTTL in process (was bessctl/module_tests/update_ttl.py; the run
// under traffic is in tests/live/module_traffic_test.cc).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "packet.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::PacketBytes;

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

// An IPv4 header and nothing after it (protocol 0), with `ttl`, padded to
// `len` (0: the 34 bytes of the headers).
Bytes BareIpv4(uint8_t ttl, size_t len = 0) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("10.1.2.3", "10.4.5.6");
  b.Last<PacketBuilder::IPv4Hdr>().ttl = ttl;
  if (len != 0) {
    b.PadTo(len);
  }
  return b.Build();
}

// `frame` through UpdateTTL: what left on gate 0 (empty: dropped).
Bytes Through(ModuleHarness &h, Module *m, bess::PacketHandle frame) {
  auto out = h.RunPackets(m, 0, {frame}, m, {0});
  return out[0].empty() ? Bytes() : out[0][0];
}

// The IPv4 header must be whole in the frame: 33 bytes is dropped, 34
// decremented. The old code decremented byte 22 of both.
TEST(UpdateTTLTest, AFrameShorterThanItsIpv4HeaderIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("UpdateTTL", bess::pb::EmptyArg());
  ASSERT_NE(nullptr, m);
  const Bytes full = BareIpv4(9);
  ASSERT_EQ(34u, full.size());
  EXPECT_TRUE(Through(h, m, MakePacket(Bytes(full.begin(), full.end() - 1))).empty());
  EXPECT_EQ(BareIpv4(8), Through(h, m, MakePacket(full)));
}

// Only the first segment counts (D-121): a chain split before or inside the
// TTL and the checksum is dropped; one split after the header is decremented.
TEST(UpdateTTLTest, AChainSplitInsideTheHeaderIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("UpdateTTL", bess::pb::EmptyArg());
  ASSERT_NE(nullptr, m);
  const Bytes frame = BareIpv4(9, 60);
  for (size_t split : {20u, 23u, 25u, 33u}) {
    EXPECT_TRUE(Through(h, m, MakePacket(frame, split)).empty())
        << "first segment of " << split << " bytes";
  }
  for (size_t split : {34u, 50u}) {
    EXPECT_EQ(BareIpv4(8, 60), Through(h, m, MakePacket(frame, split)))
        << "first segment of " << split << " bytes";
  }
}

// A clone's payload is copied before the decrement: the other holder keeps
// its TTL. The old code decremented it in the shared bytes.
TEST(UpdateTTLTest, ASharedFrameIsCopiedBeforeTheDecrement) {
  ModuleHarness h;
  Module *m = h.Create("UpdateTTL", bess::pb::EmptyArg());
  ASSERT_NE(nullptr, m);
  // Split after the TCP header the parse checks (34 + 20).
  for (size_t split : {0u, 56u}) {
    bess::PacketHandle original = MakePacket(TcpWithTtl(9), split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    EXPECT_EQ(TcpWithTtl(8), Through(h, m, clone)) << "split " << split;
    EXPECT_EQ(TcpWithTtl(9), PacketBytes(original))
        << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

// Only IPv4 has a TTL here: ARP and IPv6 pass unchanged. The old code
// decremented byte 22 of every frame (the ARP sender's first MAC byte, an
// IPv6 source address byte) and dropped the frame when it was 0 or 1.
TEST(UpdateTTLTest, AFrameThatIsNotIpv4PassesUnchanged) {
  ModuleHarness h;
  Module *m = h.Create("UpdateTTL", bess::pb::EmptyArg());
  ASSERT_NE(nullptr, m);
  const Bytes arp = PacketBuilder()
                        .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:01")
                        .Arp(1, "42:00:00:00:00:01", "10.0.0.1", "00:00:00:00:00:00", "10.0.0.2")
                        .PadTo(60)
                        .Build();
  EXPECT_EQ(arp, Through(h, m, MakePacket(arp)));
  for (const char *src : {"2001:db8::1", "::1"}) {
    const Bytes v6 = PacketBuilder()
                         .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                         .IPv6(src, "2001:db8::2")
                         .Udp(1000, 2000)
                         .PadTo(80)
                         .Build();
    EXPECT_EQ(v6, Through(h, m, MakePacket(v6))) << src;
  }
}

// IPv4 behind a VLAN tag has its TTL where it is, and a non-first fragment
// is decremented as any fragment is.
TEST(UpdateTTLTest, DecrementsBehindAVlanTagAndInFragments) {
  ModuleHarness h;
  Module *m = h.Create("UpdateTTL", bess::pb::EmptyArg());
  ASSERT_NE(nullptr, m);
  auto tagged = [](uint8_t ttl) {
    PacketBuilder b;
    b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").Dot1Q(7).IPv4("10.1.2.3", "10.4.5.6");
    b.Last<PacketBuilder::IPv4Hdr>().ttl = ttl;
    return b.Udp(1, 2).PadTo(64).Build();
  };
  EXPECT_EQ(tagged(8), Through(h, m, MakePacket(tagged(9))));
  auto fragment = [](uint8_t ttl) {
    PacketBuilder b;
    b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("10.1.2.3", "10.4.5.6");
    b.Last<PacketBuilder::IPv4Hdr>().ttl = ttl;
    b.Last<PacketBuilder::IPv4Hdr>().frag = 0x0001;
    return b.Udp(1, 2).PadTo(60).Build();
  };
  EXPECT_EQ(fragment(8), Through(h, m, MakePacket(fragment(9))));
}

}  // namespace
