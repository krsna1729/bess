// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_TAG_MATCH_H_
#define BESS_ARCH_TAG_MATCH_H_

// Bucket tag compares for hash-table probes (M21, D-071): which of a bucket's
// lanes equal a probe value, as a bit mask (bit i for lane i). Every path
// returns the same mask; the portable loops are the definition.

#include <cstdint>

#include "arch/cpu.h"

#if defined(BESS_ARCH_X86) && defined(__SSE2__)
#define BESS_ARCH_TAG_MATCH_SSE2 1
#include <emmintrin.h>
#endif

// CuckooMap's runtime-dispatched AVX2 compare (D-034): GCC/Clang with the
// <experimental/simd> TS on x86-64.
#if defined(BESS_ARCH_X86) && defined(__x86_64__) && defined(__GNUC__) && \
    __has_include(<experimental/simd>)
#define BESS_ARCH_TAG_MATCH_AVX2_DISPATCH 1
#include <experimental/simd>
#endif

namespace bess::arch {

// Eight 16-bit tags (FlowIndex buckets). `tags` is 16-byte aligned.
inline uint32_t MatchTags16x8(const uint16_t *tags, uint16_t tag) noexcept {
#if defined(BESS_ARCH_TAG_MATCH_SSE2)
  const __m128i have = _mm_load_si128(reinterpret_cast<const __m128i *>(tags));
  const __m128i eq =
      _mm_cmpeq_epi16(have, _mm_set1_epi16(static_cast<short>(tag)));
  // Narrow the sixteen byte lanes (two per tag) to one bit per tag.
  return static_cast<uint32_t>(_mm_movemask_epi8(_mm_packs_epi16(eq, eq))) &
         0xffu;
#else
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 8; i++) {
    mask |= static_cast<uint32_t>(tags[i] == tag) << i;
  }
  return mask;
#endif
}

// Four 32-bit hashes (CuckooMap buckets). Where a vector version exists
// (kMatchHashes32x4IsVector) it is compiled for AVX2 and chosen at run time:
// the caller is built for baseline x86-64 and checks the CPU on each call
// (one test of a flag the loader filled in), so the scalar compare remains
// for CPUs without AVX2. D-034 measured it paying off only for tables of
// 1024+ buckets; callers keep small tables on their own scalar loop.
#if defined(BESS_ARCH_TAG_MATCH_AVX2_DISPATCH)
inline constexpr bool kMatchHashes32x4IsVector = true;

namespace detail {

__attribute__((target("avx2"), noinline)) inline unsigned MatchHashes32x4Avx2(
    const uint32_t *hashes, uint32_t hash) {
  namespace stdx = std::experimental;
  using v4u32 = stdx::fixed_size_simd<uint32_t, 4>;
  const v4u32 values(hashes, stdx::element_aligned);
  const auto match = (values == v4u32(hash));

  unsigned mask = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (match[i]) {
      mask |= 1u << i;
    }
  }
  return mask;
}

}  // namespace detail

__attribute__((target("arch=x86-64,no-avx,no-avx2,no-bmi,no-bmi2"),
               noinline)) inline unsigned
MatchHashes32x4(const uint32_t *hashes, uint32_t hash) {
  if (__builtin_cpu_supports("avx2")) {
    return detail::MatchHashes32x4Avx2(hashes, hash);
  }

  unsigned mask = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (hashes[i] == hash) {
      mask |= 1u << i;
    }
  }
  return mask;
}
#else
inline constexpr bool kMatchHashes32x4IsVector = false;

inline unsigned MatchHashes32x4(const uint32_t *hashes,
                                uint32_t hash) noexcept {
  unsigned mask = 0;
  for (unsigned i = 0; i < 4; ++i) {
    mask |= static_cast<unsigned>(hashes[i] == hash) << i;
  }
  return mask;
}
#endif

}  // namespace bess::arch

#endif  // BESS_ARCH_TAG_MATCH_H_
