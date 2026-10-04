// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_BYTE_MATCH_H_
#define BESS_ARCH_BYTE_MATCH_H_

// Byte-class scanning for the HTTP parser (utils/http_parser.cc, from
// picohttpparser) (M21, D-071).

#include <cstddef>

#include "arch/cpu.h"

#if defined(BESS_ARCH_X86) && defined(__SSE4_2__)
#define BESS_ARCH_BYTE_MATCH_SSE42 1
#include <nmmintrin.h>
#endif

namespace bess::arch {

// Skips whole 16-byte blocks of [buf, buf_end) that hold no byte inside any of
// the inclusive ranges ranges[0]..ranges[1], ranges[2]..ranges[3], ... (the
// first `ranges_size` bytes; even, 2 to 16). `ranges` must be readable for 16
// bytes. Returns the first matching byte with *found = true, or else a point
// with fewer than 16 bytes left (or `buf` itself) with *found = false; every
// byte before the returned point is outside the ranges. Callers finish with a
// scalar loop, so a target without a vector kernel returns `buf` unchanged.
inline const char *SkipBytesOutsideRanges(const char *buf, const char *buf_end,
                                          const char *ranges,
                                          size_t ranges_size,
                                          bool *found) noexcept {
  *found = false;
#if defined(BESS_ARCH_BYTE_MATCH_SSE42)
  // SSE4.2 PCMPESTRI in ranges mode, as upstream picohttpparser does.
  if (__builtin_expect(buf_end - buf >= 16, 1)) {
    __m128i ranges16 = _mm_loadu_si128(
        static_cast<const __m128i *>(static_cast<const void *>(ranges)));
    size_t left = static_cast<size_t>(buf_end - buf) & ~size_t{15};
    do {
      __m128i b16 = _mm_loadu_si128(
          static_cast<const __m128i *>(static_cast<const void *>(buf)));
      int r = _mm_cmpestri(ranges16, static_cast<int>(ranges_size), b16, 16,
                           _SIDD_LEAST_SIGNIFICANT | _SIDD_CMP_RANGES |
                               _SIDD_UBYTE_OPS);
      if (__builtin_expect(r != 16, 0)) {
        *found = true;
        return buf + r;
      }
      buf += 16;
      left -= 16;
    } while (__builtin_expect(left != 0, 1));
  }
#else
  (void)buf_end;
  (void)ranges;
  (void)ranges_size;
#endif
  return buf;
}

}  // namespace bess::arch

#endif  // BESS_ARCH_BYTE_MATCH_H_
