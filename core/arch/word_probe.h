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

// Which of the four 64-bit words at `words` (32-byte aligned) may satisfy
// (word & mask) == want: bit i for word i. A superset of the matches, read
// without the language's atomicity: the caller re-reads each candidate with
// one atomic load, checks it again and trusts only that word. Without a
// vector unit every word is a candidate, so the caller's checks are the
// probe.
//
// Ordering: no stronger than the caller's atomic re-reads (relaxed loads);
// a caller that needs this probe ordered before a later one puts an acquire
// fence between them.
inline unsigned MaskedWordCandidates64x4([[maybe_unused]] const void *words,
                                         [[maybe_unused]] uint64_t mask,
                                         [[maybe_unused]] uint64_t want) noexcept {
#if defined(BESS_ARCH_WORD_PROBE_AVX2)
  // The four words are read with one 32-byte vector load, issued as inline
  // assembly: a C++ vector load of words the writer stores atomically would
  // be a data race in the language (undefined behaviour), while the asm is
  // opaque to the compiler and its meaning is the hardware's. On x86 a word
  // no one is writing reads back intact; a word being written may read torn,
  // which yields at most a false candidate (rejected by the caller's atomic
  // re-read) or a miss of a word mid-update. x86 does not reorder loads with
  // older loads, so this load is ordered after the caller's earlier probe
  // just as atomic loads behind the caller's fence are (the fence keeps the
  // compiler from moving it). Four atomic loads assembled in registers are
  // the conforming alternative and cost +10% (P-core) to +56% (E-core) per
  // lookup; see D-017's amendment (docs/decisions.md).
  __m256i table;
  asm volatile("vmovdqa %1, %0"
               : "=x"(table)
               : "m"(*static_cast<const __m256i *>(words)));
  // Integer compare. (A former packed-double compare treated the slots as
  // doubles: an empty slot (+0.0) equalled the key for MAC 0 (-0.0), and
  // with denormals-are-zero every masked slot equalled every key.)
  const __m256i masked = _mm256_and_si256(
      table, _mm256_set1_epi64x(static_cast<long long>(mask)));
  return static_cast<unsigned>(_mm256_movemask_pd(
      _mm256_castsi256_pd(_mm256_cmpeq_epi64(
          masked, _mm256_set1_epi64x(static_cast<long long>(want))))));
#else
  return 0xfu;
#endif
}

}  // namespace bess::arch

#endif  // BESS_ARCH_WORD_PROBE_H_
