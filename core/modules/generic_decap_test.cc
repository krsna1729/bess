// SPDX-License-Identifier: BSD-3-Clause

// GenericDecap in process (was bessctl/module_tests/generic_decap.py; the
// runs under traffic are in tests/live/module_traffic_test.cc).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

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

Module *Create(ModuleHarness &h, uint32_t bytes) {
  bess::pb::GenericDecapArg arg;
  arg.set_bytes(bytes);
  return h.Create("GenericDecap", arg);
}

// 0, 1, 2, ... as `n` bytes.
Bytes Frame(size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  return b;
}

// `frame` without its first `n` bytes.
Bytes Stripped(const Bytes &frame, size_t n) {
  return Bytes(frame.begin() + static_cast<std::ptrdiff_t>(n), frame.end());
}

// A packet shorter than the bytes to strip is dropped, never sent on with
// the header still on (the old code ignored the failed strip and forwarded
// it whole). A packet of exactly that length leaves empty.
TEST(GenericDecapTest, APacketShorterThanTheHeaderIsDropped) {
  ModuleHarness h;
  Module *gd = Create(h, 14);
  ASSERT_NE(nullptr, gd);
  for (size_t n : {0u, 1u, 13u}) {
    auto out = h.Run(gd, 0, {Frame(n)}, {0});
    EXPECT_TRUE(out[0].empty()) << n << " bytes";
  }
  auto out = h.Run(gd, 0, {Frame(14)}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_TRUE(out[0][0].empty());
}

// Only the first segment counts (D-121): a chain whose first segment ends
// inside the bytes to strip is dropped; one whose first segment holds them
// is stripped (a first segment of exactly those bytes is left empty).
TEST(GenericDecapTest, AChainSplitInsideTheHeaderIsDropped) {
  ModuleHarness h;
  Module *gd = Create(h, 14);
  ASSERT_NE(nullptr, gd);
  const Bytes frame = Frame(60);
  for (size_t split : {1u, 13u}) {
    auto out = h.RunPackets(gd, 0, {MakePacket(frame, split)}, gd, {0});
    EXPECT_TRUE(out[0].empty()) << "first segment of " << split << " bytes";
  }
  for (size_t split : {14u, 30u}) {
    auto out = h.RunPackets(gd, 0, {MakePacket(frame, split)}, gd, {0});
    ASSERT_EQ(1u, out[0].size()) << "first segment of " << split << " bytes";
    EXPECT_EQ(Stripped(frame, 14), out[0][0]) << "first segment of " << split << " bytes";
  }
}

// Stripping writes no byte: a clone is stripped as it is and its source,
// which shares the payload, keeps every byte.
TEST(GenericDecapTest, AClonedPacketIsStrippedWithoutTouchingItsSource) {
  ModuleHarness h;
  Module *gd = Create(h, 14);
  ASSERT_NE(nullptr, gd);
  const Bytes frame = Frame(60);
  for (size_t split : {0u, 30u}) {
    bess::PacketHandle original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    auto out = h.RunPackets(gd, 0, {clone}, gd, {0});
    ASSERT_EQ(1u, out[0].size()) << "split " << split;
    EXPECT_EQ(Stripped(frame, 14), out[0][0]) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(original)) << "split " << split;
    bess::PacketFree(original);
  }
}

// Every packet leaves stripped on gate 0 or is dropped: one segment, a chain
// split past the header, a clone; a runt and a chain split inside the header
// are dropped.
TEST(GenericDecapTest, EveryPacketIsAccountedFor) {
  ModuleHarness h;
  Module *gd = Create(h, 14);
  ASSERT_NE(nullptr, gd);
  const Bytes frame = Frame(60);
  bess::PacketHandle source = MakePacket(frame);
  ASSERT_NE(nullptr, source);
  bess::PacketHandle clone = bess::PacketClone(source);
  ASSERT_NE(nullptr, clone);
  auto out = h.RunPackets(gd, 0,
                          {MakePacket(frame), MakePacket(frame, 20), clone, MakePacket(Frame(10)),
                           MakePacket(frame, 8)},
                          gd, {0});
  const Bytes want = Stripped(frame, 14);
  EXPECT_EQ((std::vector<Bytes>{want, want, want}), out[0])
      << "5 in, the runt and the chain split inside the header dropped";
  bess::PacketFree(source);
}

}  // namespace
