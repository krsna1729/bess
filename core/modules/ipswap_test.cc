// SPDX-License-Identifier: BSD-3-Clause

// IPSwap in process: the IPv4 addresses (and TCP/UDP ports) are swapped where
// the checked parse finds them; a frame that is not IPv4 passes unchanged, a
// non-first fragment keeps its payload, a frame whose headers do not fit its
// first segment is dropped, and a shared payload is copied first (D-153).

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

constexpr const char *kA = "10.1.2.3";
constexpr const char *kB = "10.4.5.6";

// Ether / IPv4 (src, dst) / UDP (sport, dport), padded to `len` (0: none).
Bytes Udp(const char *src, const char *dst, uint16_t sport, uint16_t dport, size_t len = 60,
          uint16_t frag = 0) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4(src, dst);
  b.Last<PacketBuilder::IPv4Hdr>().frag = frag;
  b.Udp(sport, dport);
  if (len != 0) {
    b.PadTo(len);
  }
  return b.Build();
}

Module *Create(ModuleHarness &h) { return h.Create("IPSwap", bess::pb::EmptyArg()); }

// `frame` through IPSwap: what left on gate 0 (empty: dropped).
Bytes Through(ModuleHarness &h, Module *m, bess::PacketHandle frame) {
  auto out = h.RunPackets(m, 0, {frame}, m, {0});
  return out[0].empty() ? Bytes() : out[0][0];
}

TEST(IPSwapTest, SwapsAddressesAndPorts) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  EXPECT_EQ(Udp(kB, kA, 2000, 1000), Through(h, m, MakePacket(Udp(kA, kB, 1000, 2000))));

  PacketBuilder tcp;
  tcp.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4(kA, kB).Tcp(1000, 2000).PadTo(60);
  PacketBuilder swapped;
  swapped.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4(kB, kA).Tcp(2000, 1000).PadTo(60);
  EXPECT_EQ(swapped.Build(), Through(h, m, MakePacket(tcp.Build())));
}

// The headers must be whole in the frame: a UDP frame cut one byte short of
// its UDP header (41 bytes) is dropped, the 42-byte one swapped. The old code
// swapped both.
TEST(IPSwapTest, AFrameShorterThanItsHeadersIsDropped) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes full = Udp(kA, kB, 1000, 2000, 0);
  ASSERT_EQ(42u, full.size());
  const Bytes cut(full.begin(), full.end() - 1);
  EXPECT_TRUE(Through(h, m, MakePacket(cut)).empty());
  EXPECT_EQ(Udp(kB, kA, 2000, 1000, 0), Through(h, m, MakePacket(full)));
}

// Only the first segment counts (D-121): a chain split inside the addresses
// or the UDP header is dropped; one split after them is swapped.
TEST(IPSwapTest, AChainSplitInsideTheHeadersIsDropped) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes frame = Udp(kA, kB, 1000, 2000);
  for (size_t split : {20u, 30u, 36u, 41u}) {
    EXPECT_TRUE(Through(h, m, MakePacket(frame, split)).empty())
        << "first segment of " << split << " bytes";
  }
  for (size_t split : {42u, 50u}) {
    EXPECT_EQ(Udp(kB, kA, 2000, 1000), Through(h, m, MakePacket(frame, split)))
        << "first segment of " << split << " bytes";
  }
}

// A clone's payload is copied before the swap: the other holder keeps its
// addresses. The old code swapped them in the shared bytes.
TEST(IPSwapTest, ASharedFrameIsCopiedBeforeTheSwap) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes frame = Udp(kA, kB, 1000, 2000);
  for (size_t split : {0u, 50u}) {
    bess::PacketHandle original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    EXPECT_EQ(Udp(kB, kA, 2000, 1000), Through(h, m, clone)) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

// ARP and IPv6 have no IPv4 header: they pass unchanged. The old code swapped
// bytes 26-33 of every frame.
TEST(IPSwapTest, AFrameThatIsNotIpv4PassesUnchanged) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes arp = PacketBuilder()
                        .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:01")
                        .Arp(1, "02:00:00:00:00:01", "10.0.0.1", "00:00:00:00:00:00", "10.0.0.2")
                        .PadTo(60)
                        .Build();
  EXPECT_EQ(arp, Through(h, m, MakePacket(arp)));
  const Bytes v6 = PacketBuilder()
                       .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                       .IPv6("2001:db8::1", "2001:db8::2")
                       .Udp(1000, 2000)
                       .PadTo(80)
                       .Build();
  EXPECT_EQ(v6, Through(h, m, MakePacket(v6)));
}

// A non-first fragment has no UDP header: its addresses are swapped, its
// payload (where a first fragment's ports would be) is not. The old code
// swapped those payload bytes as ports.
TEST(IPSwapTest, ANonFirstFragmentKeepsItsPayload) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  // Fragment offset 8 bytes; the "UDP header" the builder wrote is payload.
  const Bytes fragment = Udp(kA, kB, 1000, 2000, 60, 0x0001);
  EXPECT_EQ(Udp(kB, kA, 1000, 2000, 60, 0x0001), Through(h, m, MakePacket(fragment)));
}

// IPv4 behind a VLAN tag is found where it is.
TEST(IPSwapTest, SwapsBehindAVlanTag) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  auto tagged = [](const char *src, const char *dst, uint16_t sport, uint16_t dport) {
    return PacketBuilder()
        .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
        .Dot1Q(7)
        .IPv4(src, dst)
        .Udp(sport, dport)
        .PadTo(64)
        .Build();
  };
  EXPECT_EQ(tagged(kB, kA, 2000, 1000), Through(h, m, MakePacket(tagged(kA, kB, 1000, 2000))));
}

}  // namespace
