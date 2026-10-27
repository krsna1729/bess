// SPDX-License-Identifier: BSD-3-Clause

#include "modules/macswap_kernels.h"
#include "testing/rng.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace bess::macswap {
namespace {

TEST(MacSwapKernelsTest, MatchesByteExchangeAtEveryAlignmentAndHeadLength) {
  std::mt19937 rng = bess::testing::SeededRng(0x5a17);
  for (size_t length = 12; length <= 32; ++length) {
    for (size_t offset = 0; offset < 16; ++offset) {
      for (unsigned sample = 0; sample < 128; ++sample) {
        // The advertised head ends at the allocation's end: ASan also catches
        // a full-width access on the 12..15-byte scalar tail.
        std::vector<uint8_t> original(offset + length);
        for (auto &byte : original) {
          byte = sample == 0   ? uint8_t{0}
                 : sample == 1 ? uint8_t{255}
                               : static_cast<uint8_t>(rng());
        }
        auto want = original;
        for (size_t i = 0; i < 6; ++i) {
          std::swap(want[offset + i], want[offset + i + 6]);
        }
        auto scalar = original;
        SwapScalar(scalar.data() + offset);
        ASSERT_EQ(scalar, want);
        auto actual = original;
        Swap(actual.data() + offset, length);
        ASSERT_EQ(actual, want) << "length=" << length << " offset=" << offset;
      }
    }
  }
}

}  // namespace
}  // namespace bess::macswap
