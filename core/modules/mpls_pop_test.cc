// SPDX-License-Identifier: BSD-3-Clause

// MPLSPop's "<name>/settings" singleton (D-129) in process.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "packet.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBytes;

// True when "<resource>"'s codec says applying it requires paused workers.
bool RequiresPausedWorkers(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  EXPECT_NE(nullptr, r) << resource;
  if (r == nullptr) {
    return false;
  }
  const bess::framework::ResourceCodec *codec =
      bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  EXPECT_NE(nullptr, codec) << resource;
  return codec != nullptr && codec->requires_paused_workers();
}

Bytes Packet() {
  std::mt19937 rng(1);
  return bess::testing::TcpPacket(FlowSpec{}, rng);
}

// An MPLS packet: Packet() with ether type 0x8847 and one label after the
// Ethernet header.
Bytes MplsPacket() {
  Bytes pkt = Packet();
  pkt[12] = 0x88;
  pkt[13] = 0x47;
  const uint8_t label[4] = {0x00, 0x01, 0x01, 0x40};
  pkt.insert(pkt.begin() + 14, label, label + 4);
  return pkt;
}

bess::pb::MplsPopArg Settings(bool remove, uint32_t type) {
  bess::pb::MplsPopArg arg;
  arg.set_remove_eth_header(remove);
  arg.set_next_eth_type(type);
  return arg;
}

TEST(MplsPopSettingsTest, ResourceChangesThePop) {
  ModuleHarness h;
  Module *m = h.Create("MPLSPop", bess::pb::EmptyArg(), "mp");
  ASSERT_NE(nullptr, m);
  const Bytes in = MplsPacket();

  ASSERT_EQ("", h.SetResource("mp/settings", Settings(false, 0x86dd)));
  auto out = h.Run(m, 0, {in}, {0, 1});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(in.size() - 4, out[0][0].size());
  EXPECT_EQ(0x86, out[0][0][12]);
  EXPECT_EQ(0xdd, out[0][0][13]);

  ASSERT_EQ("", h.SetResource("mp/settings", Settings(true, 0x0800)));
  out = h.Run(m, 0, {in}, {0, 1});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(in.size() - 18, out[0][0].size());
}

TEST(MplsPopSettingsTest, ReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("MPLSPop", bess::pb::EmptyArg(), "mp"));
  const bess::pb::MplsPopArg initial = Settings(false, 0x0800);
  EXPECT_EQ("", h.ReadBackDiffers("mp/settings", {{nullptr, &initial}}));
  const bess::pb::MplsPopArg s = Settings(true, 0x86dd);
  ASSERT_EQ("", h.SetResource("mp/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("mp/settings", {{nullptr, &s}}));
}

TEST(MplsPopSettingsTest, WideEtherTypeRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("MPLSPop", bess::pb::EmptyArg(), "mp"));
  EXPECT_NE("", h.SetResource("mp/settings", Settings(false, 0x10000)));
}

TEST(MplsPopSettingsTest, RequiresPausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("MPLSPop", bess::pb::EmptyArg(), "mp"));
  EXPECT_TRUE(RequiresPausedWorkers("mp/settings"));
}

// 0, 1, 2, ... as an `n`-byte frame with the MPLS EtherType at 12 (one label
// at 14-17).
Bytes Labelled(size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  b[12] = 0x88;
  b[13] = 0x47;
  return b;
}

// `frame` popped keeping its Ethernet header: the addresses, IPv4, then what
// followed the label.
Bytes Popped(const Bytes &frame) {
  Bytes b(frame.begin(), frame.begin() + 12);
  b.push_back(0x08);
  b.push_back(0x00);
  b.insert(b.end(), frame.begin() + 18, frame.end());
  return b;
}

// `frame` popped without its Ethernet header.
Bytes Removed(const Bytes &frame) { return Bytes(frame.begin() + 18, frame.end()); }

// `frame` through `m`: what left on gate 0 (empty: dropped or gate 1).
Bytes Through(ModuleHarness &h, Module *m, bess::PacketHandle frame) {
  auto out = h.RunPackets(m, 0, {frame}, m, {0});
  return out[0].empty() ? Bytes() : out[0][0];
}

// The pop needs the Ethernet header and the label, 18 bytes: 17 is dropped,
// never forwarded still labelled, in both modes. The old code wrote 14 bytes
// past a 13-byte remainder (keeping the header) or forwarded the frame
// labelled when adj(18) failed (removing it).
TEST(MplsPopTest, ALabelledFrameShorterThanItsLabelIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("MPLSPop", bess::pb::EmptyArg(), "mp");
  ASSERT_NE(nullptr, m);
  for (bool remove : {false, true}) {
    ASSERT_EQ("", h.SetResource("mp/settings", Settings(remove, 0x0800)));
    for (size_t n : {14u, 17u}) {
      auto out = h.Run(m, 0, {Labelled(n)}, {0, 1});
      EXPECT_TRUE(out[0].empty() && out[1].empty()) << n << " bytes, remove " << remove;
    }
    const Bytes shortest = Labelled(18);
    auto out = h.Run(m, 0, {shortest}, {0, 1});
    ASSERT_EQ(1u, out[0].size()) << "remove " << remove;
    EXPECT_EQ(remove ? Removed(shortest) : Popped(shortest), out[0][0]) << "remove " << remove;
  }
  // No EtherType to steer by: dropped, not read past the frame.
  auto out = h.Run(m, 0, {Bytes(13, 0x88)}, {0, 1});
  EXPECT_TRUE(out[0].empty() && out[1].empty());
}

// Only the first segment counts (D-121): a chain split inside the header or
// the label is dropped in both modes; one split after them is popped.
TEST(MplsPopTest, AChainSplitInsideTheLabelIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("MPLSPop", bess::pb::EmptyArg(), "mp");
  ASSERT_NE(nullptr, m);
  const Bytes frame = Labelled(64);
  for (bool remove : {false, true}) {
    ASSERT_EQ("", h.SetResource("mp/settings", Settings(remove, 0x0800)));
    for (size_t split : {14u, 16u, 17u}) {
      auto out = h.RunPackets(m, 0, {MakePacket(frame, split)}, m, {0, 1});
      EXPECT_TRUE(out[0].empty() && out[1].empty())
          << "first segment of " << split << " bytes, remove " << remove;
    }
    for (size_t split : {18u, 30u}) {
      EXPECT_EQ(remove ? Removed(frame) : Popped(frame), Through(h, m, MakePacket(frame, split)))
          << "first segment of " << split << " bytes, remove " << remove;
    }
  }
}

// Keeping the Ethernet header rewrites it in place: a clone's payload is
// copied first and the other holder keeps its labelled frame. Removing it
// only moves the clone's start. The old code moved the addresses in the
// shared bytes.
TEST(MplsPopTest, ASharedFrameIsCopiedBeforeThePop) {
  ModuleHarness h;
  Module *m = h.Create("MPLSPop", bess::pb::EmptyArg(), "mp");
  ASSERT_NE(nullptr, m);
  const Bytes frame = Labelled(64);
  for (bool remove : {false, true}) {
    ASSERT_EQ("", h.SetResource("mp/settings", Settings(remove, 0x0800)));
    for (size_t split : {0u, 30u}) {
      bess::PacketHandle original = MakePacket(frame, split);
      ASSERT_NE(nullptr, original);
      bess::PacketHandle clone = bess::PacketClone(original);
      ASSERT_NE(nullptr, clone);
      EXPECT_EQ(remove ? Removed(frame) : Popped(frame), Through(h, m, clone))
          << "split " << split << ", remove " << remove;
      EXPECT_EQ(frame, PacketBytes(original))
          << "split " << split << ", remove " << remove << ": the original is untouched";
      bess::PacketFree(original);
    }
  }
}

// A frame that is not MPLS leaves on gate 1 unchanged.
TEST(MplsPopTest, AFrameThatIsNotMplsLeavesOnGateOne) {
  ModuleHarness h;
  Module *m = h.Create("MPLSPop", bess::pb::EmptyArg(), "mp");
  ASSERT_NE(nullptr, m);
  Bytes frame = Labelled(14);
  frame[13] = 0x48;  // 0x8848, multicast MPLS
  auto out = h.Run(m, 0, {frame}, {0, 1});
  EXPECT_TRUE(out[0].empty());
  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ(frame, out[1][0]);
}

}  // namespace
