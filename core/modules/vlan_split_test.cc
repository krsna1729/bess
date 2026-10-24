// SPDX-License-Identifier: BSD-3-Clause

// VLANSplit in process (was bessctl/module_tests/vlan.py; the run under
// traffic is in tests/live/module_traffic_test.cc).

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
using bess::testing::PacketBytes;
using bess::testing::PacketBuilder;

// eth / tags / IP / UDP / "truculence".
template <typename Tags>
Bytes Packet(Tags tags) {
  PacketBuilder b;
  b.Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae");
  tags(b);
  return b.IPv4("127.0.0.1", "127.0.0.1").Udp(10001, 10002).Payload("truculence").Build();
}

Bytes Untagged() {
  return Packet([](PacketBuilder &) {});
}
Bytes SingleTag(uint16_t vid) {
  return Packet([vid](PacketBuilder &b) { b.Dot1Q(vid); });
}
Bytes DoubleTag(uint16_t vid) {
  return Packet([vid](PacketBuilder &b) { b.Dot1AD(vid).Dot1Q(vid); });
}

constexpr int kVids[] = {1, 17, -1, 29, 10, 13, 7};  // -1: untagged

// A tagged packet leaves untagged on the gate of its VID; an untagged one
// leaves on gate 0.
TEST(VLANSplitTest, SingleTag) {
  ModuleHarness h;
  Module *vlan = h.Create("VLANSplit", bess::pb::VLANSplitArg());
  ASSERT_NE(nullptr, vlan);
  for (int vid : kVids) {
    const gate_idx_t gate = vid < 0 ? 0 : static_cast<gate_idx_t>(vid);
    const Bytes in = vid < 0 ? Untagged() : SingleTag(static_cast<uint16_t>(vid));
    auto out = h.Run(vlan, 0, {in}, {gate});
    ASSERT_EQ(1u, out[gate].size()) << "vid " << vid;
    EXPECT_EQ(Untagged(), out[gate][0]) << "vid " << vid;
  }
}

// 802.1ad + 802.1Q: the outer tag picks the gate and is removed; the inner
// one stays.
TEST(VLANSplitTest, DoubleTag) {
  ModuleHarness h;
  Module *vlan = h.Create("VLANSplit", bess::pb::VLANSplitArg());
  ASSERT_NE(nullptr, vlan);
  for (int vid : kVids) {
    const gate_idx_t gate = vid < 0 ? 0 : static_cast<gate_idx_t>(vid);
    const Bytes in = vid < 0 ? Untagged() : DoubleTag(static_cast<uint16_t>(vid));
    const Bytes want = vid < 0 ? Untagged() : SingleTag(static_cast<uint16_t>(vid));
    auto out = h.Run(vlan, 0, {in}, {gate});
    ASSERT_EQ(1u, out[gate].size()) << "vid " << vid;
    EXPECT_EQ(want, out[gate][0]) << "vid " << vid;
  }
}

// 0, 1, 2, ... as an `n`-byte frame; at 12, `tpid` and (when it fits) VID 5.
Bytes Counting(size_t n, uint16_t tpid) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  if (n >= 14) {
    b[12] = static_cast<uint8_t>(tpid / 256);
    b[13] = static_cast<uint8_t>(tpid);
  }
  if (n >= 16) {
    b[14] = 0x00;
    b[15] = 0x05;
  }
  return b;
}

// `tagged` with its four tag bytes (12-15) removed.
Bytes Popped(const Bytes &tagged) {
  Bytes b(tagged.begin(), tagged.begin() + 12);
  b.insert(b.end(), tagged.begin() + 16, tagged.end());
  return b;
}

// A frame with no EtherType (13 bytes), and a tagged one whose first segment
// ends inside the tag or the EtherType after it (14-17 bytes, or a chain
// split there), are dropped: the old code read the first's EtherType past its
// data and popped the second past its data. An untagged frame of 14-17 bytes
// leaves on gate 0 as it is; 18 bytes is the shortest tagged frame split.
TEST(VLANSplitTest, FramesTooShortForTheirTagAreDropped) {
  ModuleHarness h;
  Module *vlan = h.Create("VLANSplit", bess::pb::VLANSplitArg());
  ASSERT_NE(nullptr, vlan);

  auto out = h.Run(vlan, 0, {Bytes(13)}, {0, 5});
  EXPECT_TRUE(out[0].empty() && out[5].empty()) << "13 bytes: no EtherType";
  for (size_t n = 14; n <= 17; n++) {
    const Bytes untagged = Counting(n, 0x0800);
    out = h.Run(vlan, 0, {untagged}, {0, 5});
    ASSERT_EQ(1u, out[0].size()) << n << "-byte untagged frame";
    EXPECT_EQ(untagged, out[0][0]) << n << "-byte untagged frame";
    out = h.Run(vlan, 0, {Counting(n, 0x8100)}, {0, 5});
    EXPECT_TRUE(out[0].empty() && out[5].empty()) << n << "-byte tagged frame";
  }
  const Bytes tagged = Counting(60, 0x8100);
  for (size_t split = 14; split < 18; split++) {
    out = h.RunPackets(vlan, 0, {MakePacket(tagged, split)}, vlan, {0, 5});
    EXPECT_TRUE(out[0].empty() && out[5].empty()) << "first segment of " << split << " bytes";
  }
  for (size_t split : {18u, 30u}) {
    out = h.RunPackets(vlan, 0, {MakePacket(tagged, split)}, vlan, {0, 5});
    ASSERT_EQ(1u, out[5].size()) << "first segment of " << split << " bytes";
    EXPECT_EQ(Popped(tagged), out[5][0]) << "first segment of " << split << " bytes";
  }
  out = h.Run(vlan, 0, {Counting(18, 0x8100)}, {0, 5});
  ASSERT_EQ(1u, out[5].size());
  EXPECT_EQ(Popped(Counting(18, 0x8100)), out[5][0]);
}

// The pop moves the addresses in place: a clone's shared payload is copied
// first, and the other holder still reads the tagged frame (D-153).
TEST(VLANSplitTest, ASharedFrameIsCopiedBeforeThePop) {
  ModuleHarness h;
  Module *vlan = h.Create("VLANSplit", bess::pb::VLANSplitArg());
  ASSERT_NE(nullptr, vlan);
  const Bytes tagged = Counting(60, 0x8100);
  for (size_t split : {0u, 30u}) {
    bess::PacketHandle original = MakePacket(tagged, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    auto out = h.RunPackets(vlan, 0, {clone}, vlan, {5});
    ASSERT_EQ(1u, out[5].size()) << "split " << split;
    EXPECT_EQ(Popped(tagged), out[5][0]) << "split " << split;
    EXPECT_EQ(tagged, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

}  // namespace
