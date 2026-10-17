// SPDX-License-Identifier: BSD-3-Clause

// The wrapper translation unit for docs/assembly-shape.md (roadmap Appendix J:
// "assembly review complete for critical kernels").
//
// Every kernel in core/arch is `inline` in a header, so no archive carries a
// symbol to disassemble. This unit calls each one behind a `noinline` wrapper
// and is compiled by hand with the release flags, which makes each kernel's own
// body appear under the wrapper's symbol:
//
//   g++ -O3 -march=x86-64-v3 -DNDEBUG -std=c++23 -I core -S -o /tmp/asm_shape.s \
//       tools/asm_shape.cc
//   awk '/^[_a-zA-Z][A-Za-z0-9_.]*:$/{f=$1} /^\t[a-z]/{print f, $1}' \
//       /tmp/asm_shape.s | sort | uniq -c
//
// It is not built by Meson: it exists to be read, and its arguments come from
// the caller so nothing folds on constants.

#include <cstddef>
#include <cstdint>

#include "arch/byte_match.h"
#include "arch/checksum_kernels.h"
#include "arch/crc32c.h"
#include "arch/tag_match.h"
#include "arch/vlan.h"
#include "arch/word_probe.h"

using namespace bess::arch;

extern "C" {

[[gnu::noinline]] uint32_t k_crc64(uint64_t d, uint32_t i) {
  return Crc32c(d, i);
}

[[gnu::noinline]] uint64_t k_sumwords(const unsigned char *p, size_t n) {
  return SumWords32(p, n);
}

[[gnu::noinline]] uint32_t k_addwords5(uint32_t s, const uint32_t *w) {
  return AddWords32<5>(s, *reinterpret_cast<const uint32_t(*)[5]>(w));
}

[[gnu::noinline]] const char *k_skipbytes(const char *b, const char *e,
                                          const char *r, size_t n, bool *f) {
  return SkipBytesOutsideRanges(b, e, r, n, f);
}

[[gnu::noinline]] unsigned k_probe4(const void *w, uint64_t m, uint64_t v) {
  return MaskedWordCandidates64x4(w, m, v);
}

[[gnu::noinline]] uint32_t k_match16x8(const uint16_t *t, uint16_t tag) {
  return MatchTags16x8(t, tag);
}

// The dispatched one: this wrapper is the dispatch (a `jmp` through
// RuntimeDispatch); the callee is one of the two bodies in tag_match.h.
[[gnu::noinline]] unsigned k_match32x4(const uint32_t *h, uint32_t v) {
  return MatchHashes32x4(h, v);
}

[[gnu::noinline]] void k_vlan_remove(void *p) {
  RemoveVlanTag(p);
}

[[gnu::noinline]] void k_vlan_insert(void *p, uint32_t tag) {
  InsertVlanTag(p, tag);
}

}  // extern "C"
