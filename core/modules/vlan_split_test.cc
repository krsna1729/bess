// SPDX-License-Identifier: BSD-3-Clause

// VLANSplit in process (was bessctl/module_tests/vlan.py; the run under
// traffic is in tests/live/module_traffic_test.cc).

#include <gtest/gtest.h>

#include <cstdint>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
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

}  // namespace
