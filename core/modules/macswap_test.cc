// SPDX-License-Identifier: BSD-3-Clause

// MACSwap in process: the addresses are swapped in a frame whose first
// segment holds the Ethernet header; a shorter one (a runt, or a chain split
// inside the header) is dropped rather than swapped past its data, and a
// shared payload is copied before the swap (D-153).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>

#include "packet.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBytes;

// 0, 1, 2, ... as an `n`-byte frame.
Bytes Counting(size_t n) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) {
    b[i] = static_cast<uint8_t>(i);
  }
  return b;
}

// `frame` with bytes 0-5 and 6-11 exchanged.
Bytes Swapped(Bytes frame) {
  for (size_t i = 0; i < 6; i++) {
    std::swap(frame[i], frame[i + 6]);
  }
  return frame;
}

TEST(MACSwapTest, SwapsTheAddresses) {
  ModuleHarness h;
  Module *m = h.Create("MACSwap", bess::pb::MACSwapArg());
  ASSERT_NE(nullptr, m);
  const Bytes frame = Counting(60);
  auto out = h.Run(m, 0, {frame}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(Swapped(frame), out[0][0]);
}

// 13 bytes has no Ethernet header: dropped. 14 is swapped. The old code
// swapped any frame, past the data of one shorter than 12 bytes.
TEST(MACSwapTest, AFrameShorterThanAnEthernetHeaderIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("MACSwap", bess::pb::MACSwapArg());
  ASSERT_NE(nullptr, m);
  for (size_t n : {6u, 13u}) {
    EXPECT_TRUE(h.Run(m, 0, {Counting(n)}, {0})[0].empty()) << n << " bytes";
  }
  auto out = h.Run(m, 0, {Counting(14)}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(Swapped(Counting(14)), out[0][0]);
}

// Only the first segment counts (D-121): a chain split inside the header is
// dropped; one split after it is swapped, its tail untouched.
TEST(MACSwapTest, AChainSplitInsideTheHeaderIsDropped) {
  ModuleHarness h;
  Module *m = h.Create("MACSwap", bess::pb::MACSwapArg());
  ASSERT_NE(nullptr, m);
  const Bytes frame = Counting(60);
  for (size_t split : {6u, 11u, 13u}) {
    auto out = h.RunPackets(m, 0, {MakePacket(frame, split)}, m, {0});
    EXPECT_TRUE(out[0].empty()) << "first segment of " << split << " bytes";
  }
  for (size_t split : {14u, 30u}) {
    auto out = h.RunPackets(m, 0, {MakePacket(frame, split)}, m, {0});
    ASSERT_EQ(1u, out[0].size()) << "first segment of " << split << " bytes";
    EXPECT_EQ(Swapped(frame), out[0][0]) << "first segment of " << split << " bytes";
  }
}

// A clone's payload is copied before the swap: the other holder keeps its
// addresses. The old code swapped them in the shared bytes.
TEST(MACSwapTest, ASharedFrameIsCopiedBeforeTheSwap) {
  ModuleHarness h;
  Module *m = h.Create("MACSwap", bess::pb::MACSwapArg());
  ASSERT_NE(nullptr, m);
  const Bytes frame = Counting(60);
  for (size_t split : {0u, 30u}) {
    bess::PacketHandle original = MakePacket(frame, split);
    ASSERT_NE(nullptr, original);
    bess::PacketHandle clone = bess::PacketClone(original);
    ASSERT_NE(nullptr, clone);
    auto out = h.RunPackets(m, 0, {clone}, m, {0});
    ASSERT_EQ(1u, out[0].size()) << "split " << split;
    EXPECT_EQ(Swapped(frame), out[0][0]) << "split " << split;
    EXPECT_EQ(frame, PacketBytes(original)) << "split " << split << ": the original is untouched";
    bess::PacketFree(original);
  }
}

}  // namespace
