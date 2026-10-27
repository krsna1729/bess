// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_MACSWAP_KERNELS_H_
#define BESS_MODULES_MACSWAP_KERNELS_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "arch/cpu.h"

namespace bess::macswap {

// Exchange two six-byte Ethernet addresses. Exactly 12 writable bytes are
// required; alignment is unrestricted and every byte after them is preserved.
inline void SwapScalar(void *data) noexcept {
  auto *p = static_cast<uint8_t *>(data);
  uint32_t dst32, src32;
  uint16_t dst16, src16;
  std::memcpy(&dst32, p, sizeof(dst32));
  std::memcpy(&dst16, p + 4, sizeof(dst16));
  std::memcpy(&src32, p + 6, sizeof(src32));
  std::memcpy(&src16, p + 10, sizeof(src16));
  std::memcpy(p, &src32, sizeof(src32));
  std::memcpy(p + 4, &src16, sizeof(src16));
  std::memcpy(p + 6, &dst32, sizeof(dst32));
  std::memcpy(p + 10, &dst16, sizeof(dst16));
}

// `head_len` is the number of contiguous writable bytes at `data` (>= 12).
// With >= 16 bytes, a portable byte shuffle exchanges the addresses and keeps
// bytes 12..15: one unaligned load, one shuffle, one unaligned store on x86-v3.
// A 12..15-byte head and BESS_ARCH_GENERIC use the exact-width scalar path.
// GCC 16.2, x86-64-v3 release: M.macswap used 2.91 fewer instructions per
// packet in eight paired rounds (all eight lower) than the former Address
// temporary. Timing did not give a stable win beyond the layout controls.
inline void Swap(void *data, size_t head_len) noexcept {
#if !defined(BESS_ARCH_GENERIC)
  if (head_len >= 16) {
    using Bytes16 = uint8_t __attribute__((vector_size(16)));
    Bytes16 v;
    std::memcpy(&v, data, sizeof(v));
    v = __builtin_shufflevector(v, v, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 12,
                                13, 14, 15);
    std::memcpy(data, &v, sizeof(v));
    return;
  }
#else
  (void)head_len;
#endif
  SwapScalar(data);
}

}  // namespace bess::macswap

#endif  // BESS_MODULES_MACSWAP_KERNELS_H_
