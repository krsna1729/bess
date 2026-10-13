// SPDX-License-Identifier: BSD-3-Clause

// --simd_width (D-126): parsing and validation, the effective width as a pure
// function of the request, the CPU and DPDK's default, and the EAL taking it.

#include "runtime/simd_width.h"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <string>
#include <vector>

#include "arch/dpdk_simd.h"
#include "dpdk.h"
#include "runtime/opts.h"

namespace bess::runtime {
namespace {

using arch::CpuVectorFeatures;

constexpr CpuVectorFeatures kNoVectors{};
constexpr CpuVectorFeatures kNeon{.vec128 = true};  // arm64; also SSE4.2 only
constexpr CpuVectorFeatures kAvx2{.vec128 = true, .avx2 = true};
constexpr CpuVectorFeatures kAvx512{.vec128 = true, .avx2 = true, .avx512 = true};
constexpr uint16_t kX86Default = 256;
constexpr uint16_t kArmDefault = 32768;  // RTE_VECT_SIMD_MAX: unlimited

TEST(SimdWidthTest, OnlyAuto256And512Parse) {
  EXPECT_EQ(SimdWidthRequest::kAuto, ParseSimdWidthRequest("auto"));
  EXPECT_EQ(SimdWidthRequest::k256, ParseSimdWidthRequest("256"));
  EXPECT_EQ(SimdWidthRequest::k512, ParseSimdWidthRequest("512"));
  for (const char *bad : {"", "AUTO", "128", "64", "0", "1024", " 256", "512 ", "0x200"}) {
    EXPECT_FALSE(ParseSimdWidthRequest(bad).has_value()) << '"' << bad << '"';
  }
  for (SimdWidthRequest r : {SimdWidthRequest::kAuto, SimdWidthRequest::k256,
                             SimdWidthRequest::k512}) {
    EXPECT_EQ(r, ParseSimdWidthRequest(SimdWidthRequestName(r)));
  }
}

TEST(SimdWidthTest, TheFlagRefusesOtherValues) {
  const std::string saved = FLAGS_simd_width;
  EXPECT_EQ("", gflags::SetCommandLineOption("simd_width", "128"));
  EXPECT_EQ("", gflags::SetCommandLineOption("simd_width", "avx512"));
  EXPECT_EQ(saved, FLAGS_simd_width) << "a refused value leaves the flag";
  EXPECT_NE("", gflags::SetCommandLineOption("simd_width", "512"));
  EXPECT_EQ("512", FLAGS_simd_width);
  EXPECT_NE("", gflags::SetCommandLineOption("simd_width", saved.c_str()));
}

// x86 shows every width it has (cpuid); arm64 does not (SVE undetected).
constexpr bool kX86 = true;
constexpr bool kArm = false;

struct Row {
  SimdWidthRequest request;
  CpuVectorFeatures cpu;
  bool cpu_width_known;
  uint16_t dpdk_default;
  uint16_t effective;
  std::vector<std::string> eal_args;
  bool warned;
};

TEST(SimdWidthTest, EffectiveWidthIsTheRequestWithinTheCpu) {
  using enum SimdWidthRequest;
  const std::string force = "--force-max-simd-bitwidth";
  const Row rows[] = {
      // auto: DPDK's default within the CPU, nothing passed to the EAL.
      {kAuto, kAvx2, kX86, kX86Default, 256, {}, false},
      {kAuto, kAvx512, kX86, kX86Default, 256, {}, false},  // no AVX-512 unless asked
      {kAuto, kNeon, kX86, kX86Default, 128, {}, false},
      {kAuto, kNeon, kArm, kArmDefault, 128, {}, false},
      // x86: 256 and 512 are forced on the EAL, clamped to the CPU with a warning.
      {k256, kAvx2, kX86, kX86Default, 256, {force, "256"}, false},
      {k256, kAvx512, kX86, kX86Default, 256, {force, "256"}, false},
      {k256, kNeon, kX86, kX86Default, 128, {force, "128"}, true},
      {k512, kAvx512, kX86, kX86Default, 512, {force, "512"}, false},
      {k512, kAvx2, kX86, kX86Default, 256, {force, "256"}, true},  // this host
      {k512, kNoVectors, kX86, kX86Default, 64, {force, "64"}, true},
      // arm64: nothing passed (DPDK keeps its unlimited default, SVE paths
      // included), a warning; BESS kernels at the detected width.
      {k256, kNeon, kArm, kArmDefault, 128, {}, true},
      {k512, kNeon, kArm, kArmDefault, 128, {}, true},
  };
  for (const Row &row : rows) {
    const SimdWidth w =
        ResolveSimdWidth(row.request, row.cpu, row.cpu_width_known, row.dpdk_default);
    SCOPED_TRACE(w.Summary());
    EXPECT_EQ(row.request, w.request);
    EXPECT_EQ(arch::MaxSimdWidth(row.cpu), w.cpu);
    EXPECT_EQ(row.dpdk_default, w.dpdk_default);
    EXPECT_EQ(row.effective, w.effective);
    EXPECT_LE(w.effective, w.cpu);
    EXPECT_EQ(row.eal_args, w.eal_args);
    EXPECT_EQ(row.warned, !w.warnings.empty());
  }
}

TEST(SimdWidthTest, SummaryAndWarningSayWhatHappened) {
  const SimdWidth w =
      ResolveSimdWidth(SimdWidthRequest::k512, kAvx2, kX86, kX86Default);
  EXPECT_EQ("simd: width 256 (requested 512; CPU 256, DPDK default 256)",
            w.Summary());
  ASSERT_EQ(1u, w.warnings.size());
  EXPECT_EQ("--simd_width=512 requested, but this CPU lacks AVX-512 (F, BW, CD, DQ and "
            "VL): using 256 bits, its widest, for DPDK and BESS kernels",
            w.warnings[0]);
  EXPECT_EQ("--simd_width=256 requested, but this CPU lacks 256-bit vectors (x86 AVX2): "
            "using 128 bits, its widest, for DPDK and BESS kernels",
            ResolveSimdWidth(SimdWidthRequest::k256, kNeon, kX86, kX86Default).warnings.at(0));
  EXPECT_EQ("--simd_width=512 limits DPDK only where the CPU's vector widths are known (x86); "
            "SVE is not detected here: DPDK keeps its default limit (unlimited), BESS kernels "
            "use 128 bits",
            ResolveSimdWidth(SimdWidthRequest::k512, kNeon, kArm, kArmDefault).warnings.at(0));
  EXPECT_EQ("simd: width 128 (requested auto; CPU 128, DPDK default unlimited)",
            ResolveSimdWidth(SimdWidthRequest::kAuto, kNeon, kArm, kArmDefault).Summary());
}

int Wide(int) { return 512; }
int Middle(int) { return 256; }
int Portable(int) { return 64; }

// --simd_width=512 on a CPU with only part of the AVX-512 set (F alone, F+BW,
// F+BW+VL without CD/DQ) never selects a 512-bit kernel.
TEST(SimdWidthTest, A512RequestWithoutTheFullAvx512SetNeverSelectsA512Kernel) {
  const std::array<arch::SimdVariant<int(int)>, 3> variants{
      {{arch::kSimd512, &Wide}, {arch::kSimd256, &Middle}, {arch::kSimdScalar, &Portable}}};
  const arch::X86CpuFlags partial[] = {
      {.sse42 = true, .avx2 = true, .avx512f = true},
      {.sse42 = true, .avx2 = true, .avx512f = true, .avx512bw = true},
      {.sse42 = true, .avx2 = true, .avx512f = true, .avx512bw = true, .avx512vl = true},
      {.sse42 = true, .avx2 = true, .avx512bw = true, .avx512cd = true, .avx512dq = true,
       .avx512vl = true},
  };
  for (const arch::X86CpuFlags &flags : partial) {
    const SimdWidth w = ResolveSimdWidth(SimdWidthRequest::k512,
                                         arch::VectorFeaturesFromX86(flags), kX86, kX86Default);
    EXPECT_EQ(256, w.effective);
    EXPECT_EQ(&Middle, arch::SelectSimdVariant<int(int)>(variants, w.effective));
    EXPECT_FALSE(w.warnings.empty());
  }
  const arch::X86CpuFlags full{.sse42 = true, .avx2 = true, .avx512f = true, .avx512bw = true,
                               .avx512cd = true, .avx512dq = true, .avx512vl = true};
  const SimdWidth w = ResolveSimdWidth(SimdWidthRequest::k512,
                                       arch::VectorFeaturesFromX86(full), kX86, kX86Default);
  EXPECT_EQ(&Wide, arch::SelectSimdVariant<int(int)>(variants, w.effective));
  // Without AVX2 a 256 request does not select the 256-bit kernel either.
  const SimdWidth sse = ResolveSimdWidth(SimdWidthRequest::k256,
                                         arch::VectorFeaturesFromX86({.sse42 = true}), kX86,
                                         kX86Default);
  EXPECT_EQ(&Portable, arch::SelectSimdVariant<int(int)>(variants, sse.effective));
}

// A program that never resolves the width (a test, a benchmark) runs its
// kernels at what bessd's auto would choose on this CPU.
TEST(SimdWidthTest, TheKernelDefaultIsWhatAutoChooses) {
  const CpuVectorFeatures cpu = arch::DetectCpuVectorFeatures();
  EXPECT_EQ(arch::DefaultSimdWidth(cpu),
            ResolveSimdWidth(SimdWidthRequest::kAuto, cpu, arch::kCpuSimdWidthKnown,
                             arch::kDpdkDefaultSimdWidth)
                .effective);
  EXPECT_EQ(kArmDefault, arch::kDpdkUnlimitedSimdWidth);
}

// The one test that brings the EAL up (once per process): --simd_width=512,
// decided before rte_eal_init() and held by DPDK afterwards.
TEST(SimdWidthTest, TheEalIsForcedToTheEffectiveWidth) {
  ASSERT_FALSE(IsDpdkInitialized());
  FLAGS_simd_width = "512";
  InitDpdk();
  const SimdWidth &w = ProcessSimdWidth();
  EXPECT_EQ(SimdWidthRequest::k512, w.request);
  const uint16_t cpu = arch::MaxSimdWidth(arch::DetectCpuVectorFeatures());
  EXPECT_EQ(cpu < 512 ? cpu : 512, w.effective);
  EXPECT_EQ(w.effective, arch::SimdWidth()) << "BESS kernels follow the same width";
  if (!arch::kCpuSimdWidthKnown) {
    // arm64: nothing passed, DPDK keeps its default (SVE paths included).
    EXPECT_TRUE(w.eal_args.empty());
    EXPECT_EQ(arch::kDpdkDefaultSimdWidth, arch::DpdkMaxSimdWidth());
    return;
  }
  EXPECT_EQ(w.effective, arch::DpdkMaxSimdWidth());
  // Forced: nothing later in the process changes DPDK's limit.
  EXPECT_EQ(-EPERM, rte_vect_set_max_simd_bitwidth(128));
  EXPECT_EQ(w.effective, arch::DpdkMaxSimdWidth());
  EXPECT_EQ(&w, &ProcessSimdWidth()) << "resolved once";
}

}  // namespace
}  // namespace bess::runtime
