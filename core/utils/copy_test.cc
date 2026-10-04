// SPDX-License-Identifier: BSD-3-Clause

#include "copy.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace {

constexpr uint8_t kGuard = 0xA5;
constexpr size_t kMaxBytes = 1100;     // past the 256-byte alignment step and
                                       // several 8-block (256-byte) loops
constexpr size_t kSloppyOverrun = 31;  // Copy()'s documented bound
constexpr size_t kPad = 64;

// Checks one copy of `bytes` bytes to dst + dst_off from src + src_off:
// exactly the source bytes land in [0, bytes); a strict copy writes nothing
// else, a sloppy one writes only source bytes into the next 31 and nothing
// before dst or after that.
void CheckCopy(size_t bytes, size_t dst_off, size_t src_off, bool sloppy) {
  std::vector<uint8_t> src(kPad + kMaxBytes + kPad);
  std::vector<uint8_t> dst(kPad + kMaxBytes + kPad, kGuard);
  for (size_t i = 0; i < src.size(); i++) {
    src[i] = static_cast<uint8_t>(i * 7 + 1);
  }
  const uint8_t *s = src.data() + src_off;
  uint8_t *d = dst.data() + kPad + dst_off;

  bess::utils::Copy(d, s, bytes, sloppy);

  for (size_t i = 0; i < kPad + dst_off; i++) {
    ASSERT_EQ(dst[i], kGuard) << "wrote before dst: bytes=" << bytes;
  }
  for (size_t i = 0; i < bytes; i++) {
    ASSERT_EQ(d[i], s[i]) << "byte " << i << " of " << bytes
                          << " dst_off=" << dst_off << " src_off=" << src_off
                          << " sloppy=" << sloppy;
  }
  const size_t limit = sloppy ? bytes + kSloppyOverrun : bytes;
  for (size_t i = bytes; i < limit; i++) {
    if (d[i] != kGuard) {
      ASSERT_EQ(d[i], s[i]) << "sloppy overrun wrote non-source byte " << i;
    }
  }
  for (uint8_t *p = d + limit; p < dst.data() + dst.size(); p++) {
    ASSERT_EQ(*p, kGuard) << "wrote " << (p - d) << " bytes into a "
                          << bytes << "-byte copy, sloppy=" << sloppy
                          << " dst_off=" << dst_off;
  }
}

TEST(CopyTest, StrictCopiesExactlyTheBytes) {
  for (size_t bytes = 0; bytes <= kMaxBytes; bytes++) {
    for (size_t dst_off : {0, 1, 7, 15, 16, 17, 31}) {
      CheckCopy(bytes, dst_off, (bytes + dst_off) % 5, false);
    }
  }
}

TEST(CopyTest, SloppyOverrunsAtMost31Bytes) {
  for (size_t bytes = 0; bytes <= kMaxBytes; bytes++) {
    for (size_t dst_off : {0, 1, 7, 15, 16, 17, 31}) {
      CheckCopy(bytes, dst_off, (bytes + dst_off) % 5, true);
    }
  }
}

// Constant sizes take the inlined path (Copy() dispatches on
// __builtin_constant_p), including CopySmall's per-size cases.
template <size_t kBytes>
void CheckConstant() {
  uint8_t src[kBytes + 1];
  uint8_t dst[kBytes + kPad];
  for (size_t i = 0; i <= kBytes; i++) {
    src[i] = static_cast<uint8_t>(i + 1);
  }
  for (uint8_t &b : dst) {
    b = kGuard;
  }
  bess::utils::Copy(dst, src, kBytes);
  for (size_t i = 0; i < kBytes; i++) {
    ASSERT_EQ(dst[i], src[i]) << kBytes;
  }
  for (size_t i = kBytes; i < sizeof(dst); i++) {
    ASSERT_EQ(dst[i], kGuard) << kBytes;
  }
}

template <size_t... kSizes>
void CheckConstants(std::index_sequence<kSizes...>) {
  (CheckConstant<kSizes + 1>(), ...);
}

TEST(CopyTest, ConstantSizesUpTo80) {
  CheckConstants(std::make_index_sequence<80>{});
}

}  // namespace
