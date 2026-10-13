// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/simd_width.h"

#include <algorithm>

#include "arch/dpdk_simd.h"
#include "runtime/opts.h"
#include "utils/logging.h"

namespace bess::runtime {
namespace {

std::string WidthName(uint16_t width) {
  return width >= arch::kDpdkUnlimitedSimdWidth ? std::string("unlimited")
                                                : std::to_string(width);
}

}  // namespace

std::optional<SimdWidthRequest> ParseSimdWidthRequest(std::string_view value) {
  if (value == "auto") {
    return SimdWidthRequest::kAuto;
  }
  if (value == "256") {
    return SimdWidthRequest::k256;
  }
  if (value == "512") {
    return SimdWidthRequest::k512;
  }
  return std::nullopt;
}

std::string_view SimdWidthRequestName(SimdWidthRequest request) {
  switch (request) {
    case SimdWidthRequest::kAuto:
      return "auto";
    case SimdWidthRequest::k256:
      return "256";
    case SimdWidthRequest::k512:
      return "512";
  }
  return "auto";
}

SimdWidth ResolveSimdWidth(SimdWidthRequest request,
                           arch::CpuVectorFeatures cpu, uint16_t dpdk_default) {
  SimdWidth w;
  w.request = request;
  w.cpu = arch::MaxSimdWidth(cpu);
  w.dpdk_default = dpdk_default;
  if (request == SimdWidthRequest::kAuto) {
    w.effective = std::min(dpdk_default, w.cpu);
    return w;
  }
  const uint16_t requested =
      request == SimdWidthRequest::k512 ? arch::kSimd512 : arch::kSimd256;
  w.effective = std::min(requested, w.cpu);
  if (requested > w.cpu) {
    // D-123's rule: diagnose, and proceed with what the CPU has.
    w.warnings.push_back(
        "--simd_width=" + std::to_string(requested) +
        " requested, but this CPU lacks " +
        (requested == arch::kSimd512 ? "AVX-512 (F, BW, CD, DQ and VL)"
                                     : "256-bit vectors (x86 AVX2)") +
        ": using " + std::to_string(w.effective) +
        " bits, its widest, for DPDK and BESS kernels");
  }
  // DPDK still checks the CPU before taking a path; passing the clamped
  // width keeps its limit equal to the one reported.
  w.eal_args = {"--force-max-simd-bitwidth", std::to_string(w.effective)};
  return w;
}

std::string SimdWidth::Summary() const {
  return "simd: width " + std::to_string(effective) + " (requested " +
         std::string(SimdWidthRequestName(request)) + "; CPU " +
         std::to_string(cpu) + ", DPDK default " + WidthName(dpdk_default) +
         ")";
}

const SimdWidth &ProcessSimdWidth() {
  static const SimdWidth width = [] {
    // The flag's validator accepted it.
    const std::optional<SimdWidthRequest> request =
        ParseSimdWidthRequest(FLAGS_simd_width);
    SimdWidth w =
        ResolveSimdWidth(request.value_or(SimdWidthRequest::kAuto),
                         arch::DetectCpuVectorFeatures(),
                         arch::kDpdkDefaultSimdWidth);
    LOG(INFO) << w.Summary();
    for (const std::string &warning : w.warnings) {
      LOG(WARNING) << warning;
    }
    arch::SetSimdWidth(w.effective);
    return w;
  }();
  return width;
}

}  // namespace bess::runtime
