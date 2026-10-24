// SPDX-License-Identifier: BSD-3-Clause

// The MAC table's kernel (l2/mac_table_kernels.h, D-190) agrees with its
// portable definition, whichever path this build takes (the AVX2 probe in an
// x86-64-v3 build, every word a candidate with -DBESS_ARCH_GENERIC or on
// arm64).

#include "l2/mac_table_kernels.h"
#include "testing/rng.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

namespace bess::arch {
namespace {

constexpr uint64_t kMask = 0x8000ffffFFFFffffull;  // L2Forward's key bits

// Every word that matches under the mask is a candidate, whatever the bits
// outside the mask hold. (A candidate need not match: the caller re-checks.)
TEST(MatchTest, MaskedWordCandidatesIncludeEveryMatch) {
  std::mt19937_64 rng = bess::testing::SeededRng64(17);
  alignas(32) uint64_t words[4];
  for (int trial = 0; trial < 20000; trial++) {
    // Keys from a small alphabet, occupied bit random, payload (the bits
    // outside the mask) random.
    for (auto &w : words) {
      w = (rng() % 3) | (rng() & ~kMask) | ((rng() & 1u) << 63u);
    }
    const uint64_t want = (rng() % 3) | (1ull << 63u);
    unsigned matches = 0;
    for (unsigned i = 0; i < 4; i++) {
      matches |= unsigned{(words[i] & kMask) == want} << i;
    }
    const unsigned candidates = MaskedWordCandidates64x4(words, kMask, want);
    ASSERT_EQ(matches, candidates & matches) << std::hex << candidates;
    ASSERT_EQ(0u, candidates & ~0xfu);
  }
}

}  // namespace
}  // namespace bess::arch
