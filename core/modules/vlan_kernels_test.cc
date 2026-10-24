// SPDX-License-Identifier: BSD-3-Clause

// The VLAN modules' kernels (modules/vlan_kernels.h, D-190) move the bytes
// their definition says, whichever code the compiler emits for this build
// (SSE on x86, NEON on arm64, plain stores with -DBESS_ARCH_GENERIC).

#include "modules/vlan_kernels.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <numeric>

namespace bess::arch {
namespace {

// Bytes 0..23 of a buffer hold 1..24. The kernels touch 16 bytes; the bytes
// around them must stay put.
std::array<uint8_t, 24> Numbered() {
  std::array<uint8_t, 24> b;
  std::iota(b.begin(), b.end(), uint8_t{1});
  return b;
}

TEST(VlanTest, RemoveMovesTheAddressesOverTheTag) {
  auto b = Numbered();
  RemoveVlanTag(b.data() + 4);
  // [0 0 0 0 | the 12 address bytes that were at 4..15] at 4..19.
  const std::array<uint8_t, 24> want = {1,  2,  3,  4,  0,  0,  0,  0,
                                        5,  6,  7,  8,  9,  10, 11, 12,
                                        13, 14, 15, 16, 21, 22, 23, 24};
  EXPECT_EQ(want, b);
}

TEST(VlanTest, InsertMovesTheAddressesBackAndFillsTheGap) {
  auto b = Numbered();
  const uint8_t tag[4] = {0x81, 0x00, 0x0a, 0xbc};  // TPID 0x8100, TCI 0x0abc
  uint32_t tag_raw;
  std::memcpy(&tag_raw, tag, sizeof(tag_raw));
  // The frame was at 4; it now starts at 0.
  InsertVlanTag(b.data(), tag_raw);
  const std::array<uint8_t, 24> want = {
      5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,  // addresses
      0x81, 0x00, 0x0a, 0xbc,                          // the tag
      17, 18, 19, 20, 21, 22, 23, 24};                 // EtherType onwards
  EXPECT_EQ(want, b);
  // Removing it again restores the frame at 4.
  RemoveVlanTag(b.data());
  auto original = Numbered();
  EXPECT_TRUE(std::equal(b.begin() + 4, b.end(), original.begin() + 4));
}

}  // namespace
}  // namespace bess::arch
