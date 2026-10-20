// SPDX-License-Identifier: BSD-3-Clause

// VLANPop in process: a tagged frame loses its 802.1Q tag, and a frame too
// short to carry an Ethernet header is dropped rather than read past its head.

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <vector>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::ModuleHarness;

Bytes Frame() {
  std::random_device rd;
  std::mt19937 rng(rd());
  return bess::testing::TcpPacket(FlowSpec{}, rng);
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

}  // namespace
