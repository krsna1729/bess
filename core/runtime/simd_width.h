// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_SIMD_WIDTH_H_
#define BESS_RUNTIME_SIMD_WIDTH_H_

// The process's SIMD width (D-126): --simd_width=auto|256|512 and this CPU,
// decided once before the EAL starts and used by DPDK
// (--force-max-simd-bitwidth) and BESS's own kernels (arch/simd_dispatch.h)
// alike.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "arch/simd_dispatch.h"

namespace bess::runtime {

enum class SimdWidthRequest { kAuto, k256, k512 };

// "auto", "256" or "512"; anything else is nullopt.
std::optional<SimdWidthRequest> ParseSimdWidthRequest(std::string_view value);
std::string_view SimdWidthRequestName(SimdWidthRequest request);

struct SimdWidth {
  SimdWidthRequest request = SimdWidthRequest::kAuto;
  uint16_t cpu = 0;           // the widest vectors the CPU runs
  uint16_t dpdk_default = 0;  // DPDK's own limit for this architecture
  // What BESS kernels use at most: under auto, DPDK's default limit; else the
  // request. Never more than `cpu`. Where the CPU's widths are known (x86)
  // DPDK is limited to the same width; elsewhere DPDK keeps its default.
  uint16_t effective = 0;
  // Appended to the EAL's arguments: nothing under auto (DPDK decides as it
  // always has) or where the CPU's widths are not known (arm64: SVE is not
  // detected), else --force-max-simd-bitwidth <effective>.
  std::vector<std::string> eal_args;
  // A request beyond the CPU (512 without AVX-512), or one that cannot limit
  // DPDK (arm64): diagnosed, and bessd proceeds (D-123's rule).
  std::vector<std::string> warnings;

  // "simd: width 256 (requested auto; CPU 256, DPDK default 256)", logged at
  // startup and reported by GetCapabilities.
  std::string Summary() const;
};

// Pure: the width for `request` on a CPU with `cpu`, given whether `cpu`
// shows every width the CPU has (arch::kCpuSimdWidthKnown in bessd) and
// DPDK's default limit (arch::kDpdkDefaultSimdWidth).
SimdWidth ResolveSimdWidth(SimdWidthRequest request, arch::CpuVectorFeatures cpu,
                           bool cpu_width_known, uint16_t dpdk_default);

// The process's width: --simd_width on this CPU, resolved on first use,
// logged once with its warnings, and handed to arch::SetSimdWidth(). bessd's
// main() calls it right after placement; InitDpdk() takes its EAL arguments.
const SimdWidth &ProcessSimdWidth();

}  // namespace bess::runtime

#endif  // BESS_RUNTIME_SIMD_WIDTH_H_
