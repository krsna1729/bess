// SPDX-License-Identifier: BSD-3-Clause

// The architecture kernels (M21, D-071) agree with their portable
// definitions, whichever path this build takes (the vector kernels in an x86
// build, the portable loops with -DBESS_ARCH_GENERIC or on arm64).

#include "arch/tag_match.h"
#include "arch/vlan.h"
#include "arch/word_probe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include <random>

namespace bess::arch {
namespace {

uint32_t MatchDefinition(const auto *lanes, int n, auto value) {
  uint32_t mask = 0;
  for (int i = 0; i < n; i++) {
    mask |= static_cast<uint32_t>(lanes[i] == value) << i;
  }
  return mask;
}

TEST(MatchTest, Tags16x8EqualDefinition) {
  std::mt19937 rng(7);
  alignas(16) uint16_t tags[8];
  for (int trial = 0; trial < 20000; trial++) {
    // A small alphabet makes matches (including several per bucket) likely;
    // offsetting it by 0x7ffe crosses the sign bit of a 16-bit lane.
    const uint16_t base = trial & 1 ? 0x7ffe : 0;
    for (auto &t : tags) t = static_cast<uint16_t>(base + rng() % 5);
    const auto probe = static_cast<uint16_t>(base + rng() % 5);
    ASSERT_EQ(MatchDefinition(tags, 8, probe), MatchTags16x8(tags, probe));
  }
  for (uint32_t pattern = 0; pattern < 256; pattern++) {
    for (int i = 0; i < 8; i++) tags[i] = (pattern >> i & 1) ? 0xffff : 0x8000;
    ASSERT_EQ(pattern, MatchTags16x8(tags, 0xffff));
    ASSERT_EQ(~pattern & 0xffu, MatchTags16x8(tags, 0x8000));
  }
}

TEST(MatchTest, Hashes32x4EqualDefinition) {
  std::mt19937 rng(34);
  alignas(16) uint32_t hashes[4];
  for (int trial = 0; trial < 20000; trial++) {
    const uint32_t base = trial & 1 ? 0x7ffffffe : 0;
    for (auto &h : hashes) h = base + rng() % 3;
    const uint32_t probe = base + rng() % 3;
    ASSERT_EQ(MatchDefinition(hashes, 4, probe),
              MatchHashes32x4(hashes, probe));
  }
  for (uint32_t pattern = 0; pattern < 16; pattern++) {
    for (int i = 0; i < 4; i++) {
      hashes[i] = (pattern >> i & 1) ? 0xffffffffu : 0x80000000u;
    }
    ASSERT_EQ(pattern, MatchHashes32x4(hashes, 0xffffffffu));
  }
}

constexpr uint64_t kMask = 0x8000ffffFFFFffffull;  // L2Forward's key bits

struct Bucket {
  alignas(32) uint64_t words[4] = {};
  bool Find(uint64_t want, uint64_t *found) const {
    return FindMaskedWord64x4(
        words, kMask, want,
        [this](unsigned i) { return __atomic_load_n(&words[i], __ATOMIC_RELAXED); },
        found);
  }
};

TEST(MatchTest, MaskedWord64x4FindsTheFirstMatchUnderTheMask) {
  std::mt19937_64 rng(17);
  for (int trial = 0; trial < 20000; trial++) {
    Bucket b;
    // Keys from a small alphabet, occupied bit random, payload (the bits
    // outside the mask) random: the payload must not affect matching.
    for (auto &w : b.words) {
      w = (rng() % 3) | (rng() & ~kMask) | ((rng() & 1) << 63);
    }
    const uint64_t want = (rng() % 3) | (1ull << 63);
    int first = -1;
    for (int i = 3; i >= 0; i--) {
      if ((b.words[i] & kMask) == want) first = i;
    }
    uint64_t found = 0;
    ASSERT_EQ(first >= 0, b.Find(want, &found));
    if (first >= 0) {
      ASSERT_EQ(b.words[first], found);
    }
  }
}

// The word handed back is the one `load` returned, and a candidate that
// `load` no longer confirms is rejected: the writer replaced it between the
// bucket read and the re-read.
TEST(MatchTest, MaskedWord64x4TrustsOnlyLoadedWords) {
  alignas(32) uint64_t words[4] = {0, 0, 5 | 1ull << 63, 0};
  const uint64_t want = 5 | 1ull << 63;
  uint64_t found = 0;
  const uint64_t replaced = 6 | 1ull << 63;
  EXPECT_FALSE(FindMaskedWord64x4(
      words, kMask, want,
      [&](unsigned i) { return i == 2 ? replaced : words[i]; }, &found));
  const uint64_t new_payload = want | (uint64_t{0x1234} << 48);
  ASSERT_TRUE(FindMaskedWord64x4(
      words, kMask, want,
      [&](unsigned i) { return i == 2 ? new_payload : words[i]; }, &found));
  EXPECT_EQ(new_payload, found);
}

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
