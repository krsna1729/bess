// SPDX-License-Identifier: BSD-3-Clause
//
// The copy primitives both classifier plans share: exact-width, masked and
// unmasked. A constant `size` compiles to single loads/stores (no libc memcpy
// call for the common 1/2/4/8-byte fields) and is unaligned-safe via memcpy
// semantics. The gather (ExtractPlan) writes a key; the scatter (ResultPlan)
// writes metadata or packet bytes. Both use these.

#ifndef BESS_CLASSIFIER_PLAN_COPY_H_
#define BESS_CLASSIFIER_PLAN_COPY_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bess::classifier {

// Never over-reads: exactly `size` source bytes are touched.
inline void CopyBytes(std::byte *dst, const std::byte *src,
                      size_t size) noexcept {
  switch (size) {
    case 1:
      dst[0] = src[0];
      break;
    case 2:
      std::memcpy(dst, src, 2);
      break;
    case 4:
      std::memcpy(dst, src, 4);
      break;
    case 8:
      std::memcpy(dst, src, 8);
      break;
    case 16:
      std::memcpy(dst, src, 16);
      break;
    case 32:
      std::memcpy(dst, src, 32);
      break;
    default:
      std::memcpy(dst, src, size);
      break;
  }
}

// Exact-width copy + mask. Width-specific ANDs keep masked 1/2/4/8-byte
// fields on the same single-instruction path as unmasked ones.
inline void CopyMasked(std::byte *dst, const std::byte *src, size_t size,
                       const std::byte *mask) noexcept {
  switch (size) {
    case 1:
      dst[0] = static_cast<std::byte>(src[0] & mask[0]);
      break;
    case 2: {
      uint16_t v;
      uint16_t m;
      std::memcpy(&v, src, 2);
      std::memcpy(&m, mask, 2);
      v &= m;
      std::memcpy(dst, &v, 2);
      break;
    }
    case 4: {
      uint32_t v;
      uint32_t m;
      std::memcpy(&v, src, 4);
      std::memcpy(&m, mask, 4);
      v &= m;
      std::memcpy(dst, &v, 4);
      break;
    }
    case 8: {
      uint64_t v;
      uint64_t m;
      std::memcpy(&v, src, 8);
      std::memcpy(&m, mask, 8);
      v &= m;
      std::memcpy(dst, &v, 8);
      break;
    }
    default:
      std::memcpy(dst, src, size);
      for (size_t k = 0; k < size; k++) {
        dst[k] &= mask[k];
      }
      break;
  }
}

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_PLAN_COPY_H_
