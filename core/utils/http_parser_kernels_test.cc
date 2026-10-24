// SPDX-License-Identifier: BSD-3-Clause

// The HTTP parser's kernel (utils/http_parser_kernels.h, D-190) keeps its
// contract with the scalar loop that finishes every scan, whichever path this
// build takes (SSE4.2 PCMPESTRI on x86-64-v3, the unchanged pointer with
// -DBESS_ARCH_GENERIC or on arm64): every byte it skips is outside the
// ranges, a match it reports is the first one, and a scan that reports none
// stops at the start or with fewer than 16 bytes left.

#include "utils/http_parser_kernels.h"
#include "testing/rng.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace bess::arch {
namespace {

// The definition: the index of the first byte of buf[0, n) inside any range
// [ranges[2i], ranges[2i+1]], or n.
size_t FirstInRange(const char *buf, size_t n, const char *ranges,
                    size_t ranges_size) {
  for (size_t i = 0; i < n; i++) {
    const auto c = static_cast<uint8_t>(buf[i]);
    for (size_t r = 0; r + 1 < ranges_size; r += 2) {
      if (static_cast<uint8_t>(ranges[r]) <= c &&
          c <= static_cast<uint8_t>(ranges[r + 1])) {
        return i;
      }
    }
  }
  return n;
}

void ExpectContract(const char *buf, size_t n, const char *ranges,
                    size_t ranges_size) {
  bool found = true;
  const char *p =
      SkipBytesOutsideRanges(buf, buf + n, ranges, ranges_size, &found);
  const size_t first = FirstInRange(buf, n, ranges, ranges_size);
  ASSERT_GE(p, buf);
  const auto at = static_cast<size_t>(p - buf);
  if (found) {
    ASSERT_EQ(first, at) << "n=" << n;
  } else {
    ASSERT_LE(at, first) << "n=" << n;
    ASSERT_TRUE(at == 0 || n - at < 16) << "n=" << n << " at=" << at;
  }
}

TEST(SkipBytesTest, KeepsTheScalarLoopsContract) {
  std::mt19937 rng = bess::testing::SeededRng(53);
  std::vector<char> buf(80);
  std::array<char, 16> ranges{};
  for (int trial = 0; trial < 20000; trial++) {
    // 2 to 16 range bytes (1 to 8 ranges), readable for 16 bytes.
    const size_t ranges_size = 2 * (1 + rng() % 8);
    for (size_t r = 0; r < ranges.size(); r++) {
      ranges[r] = static_cast<char>(rng());
    }
    for (size_t r = 0; r + 1 < ranges_size; r += 2) {
      if (static_cast<uint8_t>(ranges[r]) > static_cast<uint8_t>(ranges[r + 1])) {
        std::swap(ranges[r], ranges[r + 1]);
      }
    }
    // Mostly bytes outside the ranges, so scans cross whole blocks; a match
    // now and then at a random place.
    const size_t n = rng() % buf.size();
    for (size_t i = 0; i < n; i++) {
      buf[i] = static_cast<char>('a' + rng() % 26);
    }
    if (n != 0 && rng() % 2 == 0) {
      buf[rng() % n] = ranges[rng() % ranges_size];
    }
    ExpectContract(buf.data(), n, ranges.data(), ranges_size);
  }
}

// picohttpparser's own range sets (token and header-value delimiters), over
// every length up to three blocks and every position of the one delimiter.
TEST(SkipBytesTest, ParserRangesFindEveryDelimiterPosition) {
  alignas(16) static const char kToken[17] = "\x00 \"\"(),,//:@[]{\xff";
  alignas(16) static const char kValue[16] = "\0\010\012\037\177\177";
  for (size_t n = 0; n <= 48; n++) {
    for (size_t pos = 0; pos <= n; pos++) {
      std::vector<char> buf(n + 1, 'a');
      if (pos < n) {
        buf[pos] = '\n';
      }
      ExpectContract(buf.data(), n, kToken, 16);
      ExpectContract(buf.data(), n, kValue, 6);
    }
  }
}

}  // namespace
}  // namespace bess::arch
