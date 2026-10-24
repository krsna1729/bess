// SPDX-License-Identifier: BSD-3-Clause

// The firewall table's kernel (classifier/firewall_kernels.h, D-190) agrees
// with its portable definition, whichever code the compiler emits for this
// build (SSE on x86, NEON on arm64, plain loads with -DBESS_ARCH_GENERIC).

#include "classifier/firewall_kernels.h"
#include "testing/rng.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>

namespace bess::arch {
namespace {

TEST(MaskedAddrTest, PairAgreesWithThePortableDefinition) {
  std::mt19937 rng = bess::testing::SeededRng(41);
  alignas(32) uint8_t src[16];
  alignas(32) uint8_t dst[16];
  alignas(32) uint8_t mask_src[16];
  alignas(32) uint8_t mask_dst[16];
  alignas(32) uint8_t out[32];
  for (int trial = 0; trial < 20000; trial++) {
    for (size_t i = 0; i < 16; i++) {
      src[i] = static_cast<uint8_t>(rng());
      dst[i] = static_cast<uint8_t>(rng());
      mask_src[i] = static_cast<uint8_t>(rng());
      mask_dst[i] = static_cast<uint8_t>(rng());
    }
    // Whole-byte masks as well as arbitrary ones: a prefix mask has whole
    // bytes set and one partial byte.
    if (trial % 3 == 0) {
      const size_t bits = static_cast<size_t>(rng() % 129);
      for (size_t i = 0; i < 16; i++) {
        mask_src[i] = i * 8 + 8 <= bits ? 0xff
                                       : (i * 8 < bits ? static_cast<uint8_t>(
                                                             0xffu << (8 - (bits - i * 8)))
                                                       : 0);
        mask_dst[i] = mask_src[15 - i];
      }
    }
    MaskedAddrPair16x2(out, src, dst, mask_src, mask_dst);
    for (size_t i = 0; i < 16; i++) {
      ASSERT_EQ(static_cast<uint8_t>(src[i] & mask_src[i]), out[i]) << trial;
      ASSERT_EQ(static_cast<uint8_t>(dst[i] & mask_dst[i]), out[16 + i])
          << trial;
    }
  }
}

}  // namespace
}  // namespace bess::arch
