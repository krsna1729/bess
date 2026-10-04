// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_CHECKSUM_KERNELS_H_
#define BESS_ARCH_CHECKSUM_KERNELS_H_

// The bulk loop of the Internet checksum (utils/checksum.h) (M21, D-071).
// x86-64 keeps the AVX2 and add-with-carry kernels BESS has always used: the
// portable loop below, as GCC vectorizes it for x86-64-v3, is 11-25% slower
// from 1 KiB up and ~40% slower at 40-64 bytes (.scratch/m21/b1). Every path
// returns the same value.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "arch/cpu.h"

namespace bess::arch {

// Sums the 32-bit little-endian words of the `len` bytes at `p` (`len` a
// multiple of 16, any alignment). Returns a value below 2^34 that is congruent
// to that sum modulo 2^32 - 1 and is zero only if every word is zero.
inline uint64_t SumWords32(const unsigned char *p, size_t len) noexcept {
#if defined(BESS_ARCH_X86) && defined(__x86_64__)
  uint64_t sum64 = 0;

#if defined(__AVX2__)
  // Faster for >128B data
  if (len >= sizeof(__m256i) * 4) {
    const auto *buf256 = static_cast<const __m256i *>(static_cast<const void *>(p));
    __m256i zero256 = _mm256_setzero_si256();

    // We parallelize two ymm streams to minimize register dependency:
    //     a: buf256,             buf256 + 2,             ...
    //     b:         buf256 + 1,             buf256 + 3, ...
    __m256i a = _mm256_loadu_si256(buf256);
    __m256i b = _mm256_loadu_si256(buf256 + 1);

    // For each stream, accumulate unpackhi and unpacklo in parallel
    // (as 4x64bit vectors, so that each upper 0000 can hold carries)
    // -------------------------------------------------------------------
    // 32B data: aaaaAAAA bbbbBBBB ccccCCCC ddddDDDD  (1 letter = 1 byte)
    // unpackhi: bbbb0000 BBBB0000 dddd0000 DDDD0000
    // unpacklo: aaaa0000 AAAA0000 cccc0000 CCCC0000
    __m256i sum_a_hi = _mm256_unpackhi_epi32(a, zero256);
    __m256i sum_a_lo = _mm256_unpacklo_epi32(a, zero256);
    __m256i sum_b_hi = _mm256_unpackhi_epi32(b, zero256);
    __m256i sum_b_lo = _mm256_unpacklo_epi32(b, zero256);

    len -= sizeof(__m256i) * 2;
    buf256 += 2;

    while (len >= sizeof(__m256i) * 2) {
      a = _mm256_loadu_si256(buf256);
      b = _mm256_loadu_si256(buf256 + 1);

      sum_a_hi = _mm256_add_epi64(sum_a_hi, _mm256_unpackhi_epi32(a, zero256));
      sum_a_lo = _mm256_add_epi64(sum_a_lo, _mm256_unpacklo_epi32(a, zero256));
      sum_b_hi = _mm256_add_epi64(sum_b_hi, _mm256_unpackhi_epi32(b, zero256));
      sum_b_lo = _mm256_add_epi64(sum_b_lo, _mm256_unpacklo_epi32(b, zero256));

      len -= sizeof(__m256i) * 2;
      buf256 += 2;
    }

    // fold four 256bit sums into one 128bit sum
    __m256i sum256 = _mm256_add_epi64(_mm256_add_epi64(sum_a_hi, sum_a_lo),
                                      _mm256_add_epi64(sum_b_hi, sum_b_lo));
    __m128i sum128 = _mm_add_epi64(_mm256_extracti128_si256(sum256, 0),
                                   _mm256_extracti128_si256(sum256, 1));

    // fold 128bit sum into 64bit
    sum64 += static_cast<uint64_t>(_mm_cvtsi128_si64(sum128)) +
             static_cast<uint64_t>(_mm_extract_epi64(sum128, 1));
    p = static_cast<const unsigned char *>(static_cast<const void *>(buf256));
  }
#endif  // __AVX2__

  // 64-bit one's complement sum (end-around carry) of 8 then 2 words per
  // step. Each word is a memory operand of a 1-byte-aligned, may-alias type:
  // it tells the compiler exactly what the asm reads, at any address.
  typedef uint64_t __attribute__((may_alias, aligned(1))) UnalignedWord;
  while (len >= sizeof(uint64_t) * 8) {
    const auto *w = static_cast<const UnalignedWord *>(static_cast<const void *>(p));
    asm("addq %[u0], %[sum] \n\t"
        "adcq %[u1], %[sum] \n\t"
        "adcq %[u2], %[sum] \n\t"
        "adcq %[u3], %[sum] \n\t"
        "adcq %[u4], %[sum] \n\t"
        "adcq %[u5], %[sum] \n\t"
        "adcq %[u6], %[sum] \n\t"
        "adcq %[u7], %[sum] \n\t"
        "adcq $0, %[sum]"
        : [sum] "+&r"(sum64)
        : [u0] "m"(w[0]), [u1] "m"(w[1]), [u2] "m"(w[2]), [u3] "m"(w[3]),
          [u4] "m"(w[4]), [u5] "m"(w[5]), [u6] "m"(w[6]), [u7] "m"(w[7])
        : "cc");
    len -= sizeof(uint64_t) * 8;
    p += sizeof(uint64_t) * 8;
  }

  while (len >= sizeof(uint64_t) * 2) {
    const auto *w = static_cast<const UnalignedWord *>(static_cast<const void *>(p));
    asm("addq %[u0], %[sum] \n\t"
        "adcq %[u1], %[sum] \n\t"
        "adcq $0, %[sum]"
        : [sum] "+&r"(sum64)
        : [u0] "m"(w[0]), [u1] "m"(w[1])
        : "cc");
    len -= sizeof(uint64_t) * 2;
    p += sizeof(uint64_t) * 2;
  }

  // Reduce to 33 bits; carries need not be complete here.
  return (sum64 >> 32) + (sum64 & 0xFFFFFFFF);
#else
  // Each 64-bit word as its two 32-bit halves, summed exactly (no carries to
  // fold until the end); compilers vectorize this (AND, shift, 64-bit adds).
  uint64_t lo = 0, hi = 0;
  for (size_t i = 0; i < len; i += sizeof(uint64_t)) {
    uint64_t w;
    memcpy(&w, p + i, sizeof(w));
    lo += w & 0xFFFFFFFF;
    hi += w >> 32;
  }
  uint64_t sum = lo + hi;
  return (sum >> 32) + (sum & 0xFFFFFFFF);
#endif
}

}  // namespace bess::arch

#endif  // BESS_ARCH_CHECKSUM_KERNELS_H_
