// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_CRC32C_H_
#define BESS_ARCH_CRC32C_H_

// CRC32C (Castagnoli) steps for hashing (M21, D-071). Every path computes the
// same value, so a table hashed on one architecture or build hashes alike on
// another: the instruction when the build's target guarantees it (x86 SSE4.2,
// arm64 CRC), else DPDK's rte_hash_crc_*byte, which picks the instruction or
// a table at run time. On x86 the fallback is not reached: DPDK's own headers
// need SSE4.2, so BESS's x86 floor has it; arm64 builds without +crc use it.
//
// Argument order follows DPDK's rte_hash_crc_*byte: (data, init).

#include <cstdint>

#include "arch/cpu.h"

#if defined(BESS_ARCH_X86) && defined(__SSE4_2__)
#define BESS_ARCH_CRC_X86 1
#include <nmmintrin.h>
#elif defined(BESS_ARCH_ARM64) && defined(__ARM_FEATURE_CRC32)
#define BESS_ARCH_CRC_ARM64 1
#include <arm_acle.h>
#else
#include <rte_hash_crc.h>
#endif

namespace bess::arch {

inline uint32_t Crc32c(uint64_t data, uint32_t init) noexcept {
#if defined(BESS_ARCH_CRC_X86) && defined(__x86_64__)
  return static_cast<uint32_t>(_mm_crc32_u64(init, data));
#elif defined(BESS_ARCH_CRC_X86)
  init = _mm_crc32_u32(init, static_cast<uint32_t>(data));
  return _mm_crc32_u32(init, static_cast<uint32_t>(data >> 32));
#elif defined(BESS_ARCH_CRC_ARM64)
  return __crc32cd(init, data);
#else
  return rte_hash_crc_8byte(data, init);
#endif
}

inline uint32_t Crc32c(uint32_t data, uint32_t init) noexcept {
#if defined(BESS_ARCH_CRC_X86)
  return _mm_crc32_u32(init, data);
#elif defined(BESS_ARCH_CRC_ARM64)
  return __crc32cw(init, data);
#else
  return rte_hash_crc_4byte(data, init);
#endif
}

inline uint32_t Crc32c(uint16_t data, uint32_t init) noexcept {
#if defined(BESS_ARCH_CRC_X86)
  return _mm_crc32_u16(init, data);
#elif defined(BESS_ARCH_CRC_ARM64)
  return __crc32ch(init, data);
#else
  return rte_hash_crc_2byte(data, init);
#endif
}

}  // namespace bess::arch

#endif  // BESS_ARCH_CRC32C_H_
