// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_MASKED_ADDR_H_
#define BESS_ARCH_MASKED_ADDR_H_

// Masking the source and destination addresses of a classification key
// against the prefix lengths of a rule (the firewall table, and any other
// classifier that keys on a masked address pair).
//
// One 16-byte load, one AND and one 16-byte store per address, written with
// GCC/Clang vector extensions rather than intrinsics, as arch/vlan.h is: the
// compiler emits SSE2 on x86-64 (the same `vmovdqu`/`vpand` intrinsics would)
// and NEON on arm64, and plain loads where there is no vector unit. One path
// for every target; nothing here depends on BESS_ARCH_*, so the
// -Darch_generic build and the arm64 lanes compile this source too.
//
// The emitted body is two mask loads, two `vpand` with the address as the
// memory operand and two stores (docs/assembly-shape.md, probe
// `k_maskpair16x2`). Measured against the portable loop it replaced, 0.54 ns
// per pair against 0.58 (release, x86-64-v3, pinned; 0.41 against 0.44 in an
// earlier run): the point of the kernel is the fixed shape, not the ~6% delta -
// the loop is already vectorized by the compiler when the lengths are constant,
// and they are not in the packet path.
//
// A 16-byte operation needs no width decision (D-126): it is one SSE2
// register on every x86-64 the releases support, so it is not dispatched.
//
// An IPv4 pair (two 4-byte addresses) is a 64-bit word's worth of work and
// needs no kernel: the caller loads the pair, ANDs the precomputed mask and
// compares one register.

#include <cstdint>
#include <cstring>

namespace bess::arch {

namespace detail {
using MaskedAddr16 = uint8_t __attribute__((vector_size(16)));
}  // namespace detail

// `out` (32 bytes) gets `src[0..15] & mask_src[0..15]` then
// `dst[0..15] & mask_dst[0..15]`: the masked pair as the caller's key wants
// it, source first. `out` may alias nothing; the masks are the rule's prefix
// masks, built once per rule, never per packet.
inline void MaskedAddrPair16x2(void *out, const void *src, const void *dst,
                               const void *mask_src,
                               const void *mask_dst) noexcept {
  detail::MaskedAddr16 s;
  detail::MaskedAddr16 d;
  detail::MaskedAddr16 ms;
  detail::MaskedAddr16 md;
  std::memcpy(&s, src, sizeof(s));
  std::memcpy(&d, dst, sizeof(d));
  std::memcpy(&ms, mask_src, sizeof(ms));
  std::memcpy(&md, mask_dst, sizeof(md));
  s = s & ms;
  d = d & md;
  std::memcpy(out, &s, sizeof(s));
  std::memcpy(static_cast<uint8_t *>(out) + 16, &d, sizeof(d));
}

}  // namespace bess::arch

#endif  // BESS_ARCH_MASKED_ADDR_H_
