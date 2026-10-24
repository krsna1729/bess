// SPDX-License-Identifier: BSD-3-Clause

// StaticNAT in process: the address (and the IP and TCP/UDP checksums) is
// translated where the checked parse finds the headers; a frame that is not
// IPv4 leaves untranslated, a non-first fragment keeps its payload, a frame
// whose headers do not fit its first segment is dropped, and a shared payload
// is copied first (D-153).

#include <gtest/gtest.h>

#include <algorithm>
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

constexpr const char *kInside = "10.0.0.5";
constexpr const char *kOutside = "192.0.2.5";
constexpr const char *kPeer = "198.51.100.9";

// 10.0.0.0/24 <-> 192.0.2.0/24.
Module *Create(ModuleHarness &h) {
  bess::pb::StaticNATArg arg;
  auto *pair = arg.add_pairs();
  pair->mutable_int_range()->set_start("10.0.0.0");
  pair->mutable_int_range()->set_end("10.0.0.255");
  pair->mutable_ext_range()->set_start("192.0.2.0");
  pair->mutable_ext_range()->set_end("192.0.2.255");
  return h.Create("StaticNAT", arg);
}

// Ether / IPv4 (src, dst) / UDP, padded to `len` (0: none); `frag` as on the
// wire.
Bytes Udp(const char *src, const char *dst, size_t len = 60, uint16_t frag = 0) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4(src, dst);
  b.Last<PacketBuilder::IPv4Hdr>().frag = frag;
  b.Udp(1000, 2000);
  if (len != 0) {
    b.PadTo(len);
  }
  return b.Build();
}

Bytes Tcp(const char *src, const char *dst) {
  return PacketBuilder()
      .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4(src, dst)
      .Tcp(1000, 2000)
      .PadTo(60)
      .Build();
}

// `frame` into the forward direction (input gate 0): what left on output gate
// 1 (empty: dropped).
Bytes Forward(ModuleHarness &h, Module *m, bess::PacketHandle frame) {
  auto out = h.RunPackets(m, 0, {frame}, m, {0, 1});
  EXPECT_TRUE(out[0].empty()) << "the forward direction leaves on gate 1";
  return out[1].empty() ? Bytes() : out[1][0];
}

// The address and both checksums, as the builder computes them for the
// translated frame; the reverse direction translates the destination back.
TEST(StaticNATTest, TranslatesTcpAndUdp) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  EXPECT_EQ(Udp(kOutside, kPeer), Forward(h, m, MakePacket(Udp(kInside, kPeer))));
  EXPECT_EQ(Tcp(kOutside, kPeer), Forward(h, m, MakePacket(Tcp(kInside, kPeer))));
  auto out = h.RunPackets(m, 1, {MakePacket(Tcp(kPeer, kOutside))}, m, {0, 1});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(Tcp(kPeer, kInside), out[0][0]);
  EXPECT_EQ(Udp("10.9.9.9", kPeer), Forward(h, m, MakePacket(Udp("10.9.9.9", kPeer))))
      << "outside every range: untranslated";
}

// The headers must be whole in the frame: a UDP frame cut one byte short of
// its UDP header (41 bytes) is dropped, the 42-byte one translated. The old
// code translated both, writing the UDP checksum past the 41-byte frame.
TEST(StaticNATTest, AFrameShorterThanItsHeadersIsDropped) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes full = Udp(kInside, kPeer, 0);
  ASSERT_EQ(42u, full.size());
  EXPECT_TRUE(Forward(h, m, MakePacket(Bytes(full.begin(), full.end() - 1))).empty());
  EXPECT_EQ(Udp(kOutside, kPeer, 0), Forward(h, m, MakePacket(full)));
}

// Only the first segment counts (D-121): a chain split inside the address or
// the UDP checksum is dropped; one split after the UDP header is translated.
TEST(StaticNATTest, AChainSplitInsideTheHeadersIsDropped) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes frame = Udp(kInside, kPeer);
  for (size_t split : {20u, 28u, 33u, 41u}) {
    EXPECT_TRUE(Forward(h, m, MakePacket(frame, split)).empty())
        << "first segment of " << split << " bytes";
  }
  for (size_t split : {42u, 50u}) {
    EXPECT_EQ(Udp(kOutside, kPeer), Forward(h, m, MakePacket(frame, split)))
        << "first segment of " << split << " bytes";
  }
}

// A clone's payload is copied before the translation: the other holder keeps
// its address. The old code translated it in the shared bytes.
TEST(StaticNATTest, ASharedFrameIsCopiedBeforeTheTranslation) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes frame = Udp(kInside, kPeer);
  for (size_t split : {0u, 50u}) {
    bess::PacketHandle original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    EXPECT_EQ(Udp(kOutside, kPeer), Forward(h, m, clone)) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

// ARP and IPv6 have no IPv4 address: they leave untranslated. The IPv6
// source here carries 10.0.0.5 in bytes 26-29, where the old code read an
// IPv4 source address and translated it.
TEST(StaticNATTest, AFrameThatIsNotIpv4LeavesUntranslated) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes v6 = PacketBuilder()
                       .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                       .IPv6("0:0:a00:5::1", "2001:db8::2")
                       .Udp(1000, 2000)
                       .PadTo(80)
                       .Build();
  ASSERT_EQ(0x0a, v6[26]);
  ASSERT_EQ(0x05, v6[29]);
  EXPECT_EQ(v6, Forward(h, m, MakePacket(v6)));
  const Bytes arp = PacketBuilder()
                        .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:01")
                        .Arp(1, "02:00:00:00:00:01", kInside, "00:00:00:00:00:00", kPeer)
                        .PadTo(60)
                        .Build();
  EXPECT_EQ(arp, Forward(h, m, MakePacket(arp)));
}

// A non-first fragment has no UDP header: its address and IP checksum are
// translated, its payload (where a first fragment's UDP checksum would be) is
// not. The old code rewrote those payload bytes as the UDP checksum.
TEST(StaticNATTest, ANonFirstFragmentKeepsItsPayload) {
  ModuleHarness h;
  Module *m = Create(h);
  ASSERT_NE(nullptr, m);
  const Bytes fragment = Udp(kInside, kPeer, 60, 0x0001);
  // The translated IPv4 header, the original bytes after it.
  Bytes expected = Udp(kOutside, kPeer, 60, 0x0001);
  std::copy(fragment.begin() + 34, fragment.end(), expected.begin() + 34);
  EXPECT_EQ(expected, Forward(h, m, MakePacket(fragment)));
}

}  // namespace
