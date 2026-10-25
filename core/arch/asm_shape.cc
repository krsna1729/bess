// SPDX-License-Identifier: BSD-3-Clause

// The assembly-shape probe for docs/assembly-shape.md (roadmap Appendix J:
// "assembly review complete for critical kernels").
//
// Every kernel, in arch/ or in an owner's `*_kernels.h` (D-190), is `inline`
// in a header, so no archive carries a symbol to disassemble. This unit calls
// each one behind a `noinline` wrapper, and Meson builds it as
// bess_arch_asm_shape (nothing links or installs it), so every lane compiles
// the probe with the real flags. It is a probe, not a library: including the
// owners' kernel headers adds no dependency of arch/ on them
// (tools/check_includes.py exempts this file). Read the assembly from the
// built archive:
//
//   ninja -C build/perf-release core/libbess_arch_asm_shape.a
//   objdump -d --no-show-raw-insn build/perf-release/core/libbess_arch_asm_shape.a
//
// or compile this unit by hand with a core/ translation unit's flags (a
// release's are -O3 -march=x86-64-v3 -DNDEBUG -D_FILE_OFFSET_BITS=64
// -std=c++23 -I core) and split the output by symbol:
//
//   g++ -O3 -march=x86-64-v3 -DNDEBUG -std=c++23 -I core -S -o /tmp/asm_shape.s core/arch/asm_shape.cc
//   awk '/^[_a-zA-Z][A-Za-z0-9_.]*:$/{f=$1} /^\t[a-z]/{print f, $1}' /tmp/asm_shape.s
//
// The wrappers take their arguments from the caller, so nothing folds on
// constants, and the includes are portable: the arm64 lanes compile this unit
// too (their kernels take the other side of each #if).

#include <cstddef>
#include <cstdint>

#include "arch/checksum_kernels.h"
#include "arch/crc32c.h"
#include "arch/tag_match.h"
#include "classifier/firewall_kernels.h"
#include "l2/mac_table_kernels.h"
#include "modules/vlan_kernels.h"
#include "packet_parse.h"
#include "utils/http_parser_kernels.h"

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

[[gnu::noinline]] void k_maskpair16x2(void *out, const void *src,
                                      const void *dst, const void *mask_src,
                                      const void *mask_dst) {
  MaskedAddrPair16x2(out, src, dst, mask_src, mask_dst);
}

// Not a kernel but the common frame's checks every IPv4 reader runs first
// (packet_parse.h, D-188): the accept test, then the checks one at a time.
// It returns what a reader branches on (the status, or -1 for a frame the
// general parse takes), so the body is the checks and no result packing.
[[gnu::noinline]] int k_fast_ipv4(const unsigned char *f, size_t n, size_t total_len) {
  const bess::packet::FastIpv4 v = bess::packet::CheckFastIpv4({f, n}, total_len);
  return v.taken ? static_cast<int>(v.status) : -1;
}

}  // extern "C"
