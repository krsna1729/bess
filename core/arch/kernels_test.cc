// SPDX-License-Identifier: BSD-3-Clause

// core/arch's shared kernels (M21, D-071) agree with their portable
// definitions, whichever path this build takes (the vector kernels in an x86
// build, the portable loops with -DBESS_ARCH_GENERIC or on arm64). An owner's
// kernel is tested next to it, in <owner>_kernels_test.cc (D-190).

#include "arch/tag_match.h"
#include "testing/rng.h"

#include <gtest/gtest.h>

#include <cstdint>
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
  std::mt19937 rng = bess::testing::SeededRng(7);
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
  std::mt19937 rng = bess::testing::SeededRng(34);
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

}  // namespace
}  // namespace bess::arch
