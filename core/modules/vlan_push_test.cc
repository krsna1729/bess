// SPDX-License-Identifier: BSD-3-Clause

// VLANPush's "<name>/settings" singleton (D-129) in process, and the frames
// it tags: short ones, chains, and shared payloads.

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

bess::pb::VLANPushArg Tci(uint64_t tci) {
  bess::pb::VLANPushArg arg;
  arg.set_tci(tci);
  return arg;
}

TEST(VLANPushSettingsTest, ResourceSetsTheTag) {
  ModuleHarness h;
  Module *m = h.Create("VLANPush", Tci(1), "vp");
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.SetResource("vp/settings", Tci(0x2abc)));
  const Bytes in = Packet();
  auto out = h.Run(m, 0, {in}, {0});
  ASSERT_EQ(1u, out[0].size());
  const Bytes &pkt = out[0][0];
  ASSERT_EQ(in.size() + 4, pkt.size());
  EXPECT_EQ(0x81, pkt[12]);
  EXPECT_EQ(0x00, pkt[13]);
  EXPECT_EQ(0x2a, pkt[14]);
  EXPECT_EQ(0xbc, pkt[15]);
}

TEST(VLANPushSettingsTest, ReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("VLANPush", Tci(5), "vp"));
  const bess::pb::VLANPushArg initial = Tci(5);
  EXPECT_EQ("", h.ReadBackDiffers("vp/settings", {{nullptr, &initial}}));
  const bess::pb::VLANPushArg s = Tci(0xffff);
  ASSERT_EQ("", h.SetResource("vp/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("vp/settings", {{nullptr, &s}}));
}

TEST(VLANPushSettingsTest, WideTciRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("VLANPush", Tci(5), "vp"));
  EXPECT_NE("", h.SetResource("vp/settings", Tci(0x10000)));
}

TEST(VLANPushSettingsTest, RequiresPausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("VLANPush", Tci(5), "vp"));
  EXPECT_TRUE(RequiresPausedWorkers("vp/settings"));
}

// A frame too short to carry an Ethernet header has no EtherType to read; it
// is dropped rather than tagged from past its head (or forwarded bare).
TEST(VLANPushSettingsTest, AShortFrameIsDroppedNotTagged) {
  ModuleHarness h;
  Module *m = h.Create("VLANPush", Tci(1), "vp");
  ASSERT_NE(nullptr, m);
  auto out = h.Run(m, 0, {Bytes(13)}, {0});
  EXPECT_TRUE(out[0].empty());
}

// 0, 1, 2, ... as an `n`-byte frame with EtherType 0x0800.
Bytes Counting(size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  b[12] = 0x08;
  b[13] = 0x00;
  return b;
}

// `frame` with tag 0x8100 / `tci` after its addresses.
Bytes Tagged(const Bytes &frame, uint16_t tci) {
  Bytes b(frame.begin(), frame.begin() + 12);
  b.insert(b.end(), {0x81, 0x00, static_cast<uint8_t>(tci / 256), static_cast<uint8_t>(tci)});
  b.insert(b.end(), frame.begin() + 12, frame.end());
  return b;
}

// Every frame that has an EtherType is tagged, including one of 14 or 15
// bytes, which the 16-byte insert would read past. (Those bytes were
// discarded, so the old output matched too; the over-read itself needs ASan.)
TEST(VLANPushSettingsTest, FramesFrom14BytesAreTagged) {
  ModuleHarness h;
  Module *m = h.Create("VLANPush", Tci(0x2abc), "vp");
  ASSERT_NE(nullptr, m);
  for (size_t n = 14; n <= 17; n++) {
    const Bytes in = Counting(n);
    auto out = h.Run(m, 0, {in}, {0});
    ASSERT_EQ(1u, out[0].size()) << n << "-byte frame";
    EXPECT_EQ(Tagged(in, 0x2abc), out[0][0]) << n << "-byte frame";
  }
}

// A chain is tagged in its first segment, whatever its length from the
// Ethernet header up; the rest of the chain is untouched.
TEST(VLANPushSettingsTest, AChainIsTaggedInItsFirstSegment) {
  ModuleHarness h;
  Module *m = h.Create("VLANPush", Tci(0x2abc), "vp");
  ASSERT_NE(nullptr, m);
  const Bytes in = Counting(60);
  for (size_t split : {14u, 15u, 16u, 30u}) {
    auto out = h.RunPackets(m, 0, {MakePacket(in, split)}, m, {0});
    ASSERT_EQ(1u, out[0].size()) << "first segment of " << split << " bytes";
    EXPECT_EQ(Tagged(in, 0x2abc), out[0][0]) << "first segment of " << split << " bytes";
  }
  auto out = h.RunPackets(m, 0, {MakePacket(in, 13)}, m, {0});
  EXPECT_TRUE(out[0].empty()) << "no EtherType in the first segment";
}

// The tag goes into the headroom and the addresses move in place: a clone's
// shared payload is copied first, and the other holder still reads the
// untagged frame (D-153). The old code wrote the tag over the shared bytes.
TEST(VLANPushSettingsTest, ASharedFrameIsCopiedBeforeThePush) {
  ModuleHarness h;
  Module *m = h.Create("VLANPush", Tci(0x2abc), "vp");
  ASSERT_NE(nullptr, m);
  const Bytes in = Counting(60);
  for (size_t split : {0u, 30u}) {
    bess::PacketHandle original = MakePacket(in, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    auto out = h.RunPackets(m, 0, {clone}, m, {0});
    ASSERT_EQ(1u, out[0].size()) << "split " << split;
    EXPECT_EQ(Tagged(in, 0x2abc), out[0][0]) << "split " << split;
    EXPECT_EQ(in, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

}  // namespace
