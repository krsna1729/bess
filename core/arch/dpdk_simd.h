// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_DPDK_SIMD_H_
#define BESS_ARCH_DPDK_SIMD_H_

// DPDK's SIMD-width limit (D-126), behind core/arch/ because <rte_vect.h> is
// an architecture header (tools/check_arch.py). Internal: not installed.

#include <cstdint>

#include <rte_vect.h>

namespace bess::arch {

// The limit DPDK applies when the EAL is given no --force-max-simd-bitwidth:
// 256 on x86 (no AVX-512 path whatever the CPU), unlimited
// (RTE_VECT_SIMD_MAX) on arm64.
inline constexpr uint16_t kDpdkDefaultSimdWidth = RTE_VECT_DEFAULT_SIMD_BITWIDTH;
inline constexpr uint16_t kDpdkUnlimitedSimdWidth = RTE_VECT_SIMD_MAX;

// The limit DPDK applies; meaningful once rte_eal_init() has run.
inline uint16_t DpdkMaxSimdWidth() noexcept {
  return rte_vect_get_max_simd_bitwidth();
}

}  // namespace bess::arch

#endif  // BESS_ARCH_DPDK_SIMD_H_
