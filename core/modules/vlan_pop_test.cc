// SPDX-License-Identifier: BSD-3-Clause

// VLANPop in process: a tagged frame loses its 802.1Q tag, a frame too short
// to carry an Ethernet header is dropped rather than read past its head, and
// a tagged frame too short (in its first segment) for the tag and the
// EtherType after it is dropped rather than popped past its data.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "packet.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBytes;

Bytes Frame() {
  std::random_device rd;
  std::mt19937 rng(rd());
  return bess::testing::TcpPacket(FlowSpec{}, rng);
}

// 0, 1, 2, ... as an `n`-byte frame; `tpid` (0: none) at 12.
Bytes Counting(size_t n, uint16_t tpid) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  if (tpid != 0 && n >= 14) {
    b[12] = static_cast<uint8_t>(tpid / 256);
    b[13] = static_cast<uint8_t>(tpid);
  }
  return b;
}

// `tagged` with its four tag bytes (12-15) removed.
Bytes Popped(const Bytes &tagged) {
  Bytes b(tagged.begin(), tagged.begin() + 12);
  b.insert(b.end(), tagged.begin() + 16, tagged.end());
  return b;
}

TEST(VLANPopTest, ATaggedFrameLosesItsTag) {
  ModuleHarness h;
  Module *m = h.Create("VLANPop", bess::pb::VLANPopArg());
  ASSERT_NE(nullptr, m);

  const Bytes untagged = Frame();
  Bytes tagged(untagged.begin(), untagged.begin() + 12);
  tagged.push_back(0x81);
  tagged.push_back(0x00);  // TPID
  tagged.push_back(0x2a);
  tagged.push_back(0xbc);  // TCI
  tagged.insert(tagged.end(), untagged.begin() + 12, untagged.end());

  auto out = h.Run(m, 0, {tagged}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(untagged, out[0][0]);
}

TEST(VLANPopTest, AShortFrameIsDroppedNotRead) {
  ModuleHarness h;
  Module *m = h.Create("VLANPop", bess::pb::VLANPopArg());
  ASSERT_NE(nullptr, m);

  auto out = h.Run(m, 0, {Bytes(13)}, {0});
  EXPECT_TRUE(out[0].empty()) << "no Ethernet header to read";
}

// 14-17 bytes: an untagged frame is forwarded as it is; a tagged one has no
// whole tag and EtherType, so it is dropped. 18 bytes is the shortest tagged
// frame popped. The old code popped the tagged ones, leaving a 10-13 byte
// frame and writing up to two bytes past the data.
TEST(VLANPopTest, ATaggedFrameShorterThanItsTagIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("VLANPop", bess::pb::VLANPopArg());
  ASSERT_NE(nullptr, m);

  for (size_t n = 14; n <= 17; n++) {
    const Bytes untagged = Counting(n, 0x0800);
    auto out = h.Run(m, 0, {untagged}, {0});
    ASSERT_EQ(1u, out[0].size()) << n << "-byte untagged frame";
    EXPECT_EQ(untagged, out[0][0]) << n << "-byte untagged frame";
    for (uint16_t tpid : {0x8100, 0x88a8}) {
      out = h.Run(m, 0, {Counting(n, tpid)}, {0});
      EXPECT_TRUE(out[0].empty()) << n << "-byte frame tagged " << std::hex << tpid;
    }
  }

  const Bytes tagged = Counting(18, 0x8100);
  auto out = h.Run(m, 0, {tagged}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(Popped(tagged), out[0][0]);
}

// A chain's first segment must hold the tag and the EtherType after it: one
// split inside them is dropped, one split after them is popped (the rest of
// the chain untouched).
TEST(VLANPopTest, AChainSplitInsideTheTagIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("VLANPop", bess::pb::VLANPopArg());
  ASSERT_NE(nullptr, m);
  const Bytes tagged = Counting(60, 0x8100);

  for (size_t split = 14; split < 18; split++) {
    auto out = h.RunPackets(m, 0, {MakePacket(tagged, split)}, m, {0});
    EXPECT_TRUE(out[0].empty()) << "first segment of " << split << " bytes";
  }
  for (size_t split : {18u, 30u}) {
    auto out = h.RunPackets(m, 0, {MakePacket(tagged, split)}, m, {0});
    ASSERT_EQ(1u, out[0].size()) << "first segment of " << split << " bytes";
    EXPECT_EQ(Popped(tagged), out[0][0]) << "first segment of " << split << " bytes";
  }
}

// The pop moves the addresses in place: a clone's shared payload is copied
// first, and the other holder still reads the tagged frame (D-153). The old
// code moved the addresses in the shared bytes.
TEST(VLANPopTest, ASharedFrameIsCopiedBeforeThePop) {
  ModuleHarness h;
  Module *m = h.Create("VLANPop", bess::pb::VLANPopArg());
  ASSERT_NE(nullptr, m);
  const Bytes tagged = Counting(60, 0x8100);

  for (size_t split : {0u, 30u}) {
    bess::PacketHandle original = MakePacket(tagged, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    auto out = h.RunPackets(m, 0, {clone}, m, {0});
    ASSERT_EQ(1u, out[0].size()) << "split " << split;
    EXPECT_EQ(Popped(tagged), out[0][0]) << "split " << split;
    EXPECT_EQ(tagged, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

}  // namespace
