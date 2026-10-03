// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_WORD_PROBE_H_
#define BESS_ARCH_WORD_PROBE_H_

// Probing a bucket of four 64-bit words that a writer replaces concurrently,
// one atomic store per word (L2Forward's MAC table, D-017) (M21, D-071).

#include <cstdint>

#include "arch/cpu.h"

#if defined(BESS_ARCH_X86) && defined(__x86_64__) && defined(__AVX2__)
#define BESS_ARCH_WORD_PROBE_AVX2 1
#include <immintrin.h>
#endif

namespace bess::arch {

// Finds the first of the four 64-bit words at `words` (32-byte aligned) with
// (word & mask) == want and stores that word in *found. `load(i)` reads word
// i with one atomic load. Only words `load` returned are trusted, so the
// match and the rest of the word come from the same store even while the
// writer replaces words.
//
// Ordering: the reads behave as relaxed loads. A caller that needs this
// probe ordered before a later one (L2Forward: primary bucket before
// alternate) puts std::atomic_thread_fence(acquire) between them; with
// `load` atomic, a word written by a release store and read here makes the
// writer's earlier stores visible after the fence.
template <typename Load>
inline bool FindMaskedWord64x4(const void *words, uint64_t mask, uint64_t want,
                               const Load &load, uint64_t *found) {
#if defined(BESS_ARCH_WORD_PROBE_AVX2)
  // The four words are read with one 32-byte vector load, issued as inline
  // assembly: a C++ vector load of words the writer stores atomically would
  // be a data race in the language (undefined behaviour), while the asm is
  // opaque to the compiler and its meaning is the hardware's. On x86 a word
  // no one is writing reads back intact; a word being written may read torn,
  // which yields at most a false candidate (rejected by the atomic re-read
  // below) or a miss of a word mid-update. x86 does not reorder loads with
  // older loads, so this load is ordered after the caller's earlier probe
  // just as atomic loads with the caller's fence are (the fence keeps the
  // compiler from moving it). Four atomic loads assembled in registers are
  // the conforming alternative and cost +10% (P-core) to +56% (E-core) per
  // lookup; see D-017's amendment (docs/decisions.md).
  __m256i table;
  asm volatile("vmovdqa %1, %0"
               : "=x"(table)
               : "m"(*static_cast<const __m256i *>(words)));
  // Integer compare. (A former _mm256_cmp_pd compare treated the slots as
  // doubles: an empty slot (+0.0) equalled the key for MAC 0 (-0.0), and
  // with denormals-are-zero every masked slot equalled every key.)
  const __m256i masked = _mm256_and_si256(
      table, _mm256_set1_epi64x(static_cast<long long>(mask)));
  const int bits = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(
      masked, _mm256_set1_epi64x(static_cast<long long>(want)))));
  for (int m = bits; m != 0; m &= m - 1) {
    const uint64_t word = load(static_cast<unsigned>(__builtin_ctz(m)));
    if ((word & mask) == want) {
      *found = word;
      return true;
    }
  }
  return false;
#else
  (void)words;
  for (unsigned i = 0; i < 4; i++) {
    const uint64_t word = load(i);
    if ((word & mask) == want) {
      *found = word;
      return true;
    }
  }
  return false;
#endif
}

}  // namespace bess::arch

#endif  // BESS_ARCH_WORD_PROBE_H_
