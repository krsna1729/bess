// SPDX-License-Identifier: BSD-3-Clause

// Kernel selection by the process's SIMD width (D-126): widths from CPU
// features, the variant a width selects, dispatchers following the width, and
// the converted kernel (MatchHashes32x4) agreeing with itself at every width.

#include "arch/simd_dispatch.h"
#include "arch/tag_match.h"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <span>

namespace bess::arch {
namespace {

constexpr CpuVectorFeatures kNoVectors{};
constexpr CpuVectorFeatures kSse42{.vec128 = true};
constexpr CpuVectorFeatures kAvx2{.vec128 = true, .avx2 = true};
constexpr CpuVectorFeatures kAvx512{.vec128 = true, .avx2 = true, .avx512 = true};

// Restores the process's width when a test is done with it.
class SimdDispatchTest : public ::testing::Test {
 protected:
  void TearDown() override { SetSimdWidth(saved_); }

 private:
  uint16_t saved_ = SimdWidth();
};

int Wide(int x) { return x * 512; }
int Middle(int x) { return x * 256; }
int Portable(int x) { return x; }

constexpr std::array<SimdVariant<int(int)>, 3> kVariants{
    {{kSimd512, &Wide}, {kSimd256, &Middle}, {kSimdScalar, &Portable}}};

TEST(SimdWidthTest, CpuFeaturesNameTheirWidth) {
  static_assert(MaxSimdWidth(kNoVectors) == kSimdScalar);
  static_assert(MaxSimdWidth(kSse42) == kSimd128);
  static_assert(MaxSimdWidth(kAvx2) == kSimd256);
  static_assert(MaxSimdWidth(kAvx512) == kSimd512);
  // Unless told otherwise, at most 256: DPDK's x86 default.
  static_assert(DefaultSimdWidth(kSse42) == kSimd128);
  static_assert(DefaultSimdWidth(kAvx2) == kSimd256);
  static_assert(DefaultSimdWidth(kAvx512) == kSimd256);
}

TEST(SimdWidthTest, DetectionIsConsistentWithThisBuild) {
  const CpuVectorFeatures cpu = DetectCpuVectorFeatures();
  EXPECT_TRUE(!cpu.avx512 || cpu.avx2);
  EXPECT_TRUE(!cpu.avx2 || cpu.vec128);
#if defined(__x86_64__) || defined(__aarch64__)
  EXPECT_TRUE(cpu.vec128) << "SSE4.2 is BESS's x86 floor; arm64 has Advanced SIMD";
#endif
#if defined(__AVX2__)
  EXPECT_TRUE(cpu.avx2) << "this binary is built for AVX2 and runs";
#endif
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
  EXPECT_TRUE(cpu.avx512);
#endif
}

TEST(SimdWidthTest, TheWidestVariantTheWidthAllowsIsSelected) {
  const std::span<const SimdVariant<int(int)>> v(kVariants);
  EXPECT_EQ(&Wide, SelectSimdVariant(v, MaxSimdWidth(kAvx512)));
  EXPECT_EQ(&Middle, SelectSimdVariant(v, MaxSimdWidth(kAvx2)));
  EXPECT_EQ(&Middle, SelectSimdVariant(v, DefaultSimdWidth(kAvx512)));
  EXPECT_EQ(&Portable, SelectSimdVariant(v, MaxSimdWidth(kSse42)));
  EXPECT_EQ(&Portable, SelectSimdVariant(v, MaxSimdWidth(kNoVectors)));
  // A width below every variant still gets the last one.
  const std::array<SimdVariant<int(int)>, 1> only{{{kSimd256, &Middle}}};
  EXPECT_EQ(&Middle, SelectSimdVariant<int(int)>(only, kSimdScalar));
}

TEST_F(SimdDispatchTest, DispatchersFollowTheWidthSetOnce) {
  SetSimdWidth(MaxSimdWidth(kAvx2));
  RuntimeDispatch<int(int), 3> kernel(kVariants);
  EXPECT_EQ(&Middle, kernel.selected()) << "resolved when constructed";
  EXPECT_EQ(256, kernel(1));

  SetSimdWidth(MaxSimdWidth(kAvx512));
  EXPECT_EQ(&Wide, kernel.selected()) << "re-resolved when the width is set";
  EXPECT_EQ(512, kernel(1));

  SetSimdWidth(MaxSimdWidth(kSse42));
  EXPECT_EQ(&Portable, kernel.selected());
  EXPECT_EQ(kSimd128, SimdWidth());

  {
    RuntimeDispatch<int(int), 3> later(kVariants);
    EXPECT_EQ(&Portable, later.selected()) << "a later dispatcher sees the width";
  }
  // `later` unregistered itself; setting the width again touches only `kernel`.
  SetSimdWidth(MaxSimdWidth(kAvx2));
  EXPECT_EQ(&Middle, kernel.selected());
}

TEST_F(SimdDispatchTest, MatchHashes32x4AgreesAtEveryWidth) {
#if !defined(BESS_ARCH_TAG_MATCH_AVX2_DISPATCH)
  GTEST_SKIP() << "no vector variant in this build";
#else
  const auto &dispatch = detail::match_hashes_32x4;
  ASSERT_EQ(2u, dispatch.variants().size());
  const CpuVectorFeatures cpu = DetectCpuVectorFeatures();

  SetSimdWidth(MaxSimdWidth(kSse42));
  EXPECT_EQ(&detail::MatchHashes32x4Scalar, dispatch.selected());
  if (cpu.avx2) {
    SetSimdWidth(MaxSimdWidth(kAvx2));
    EXPECT_EQ(&detail::MatchHashes32x4Avx2, dispatch.selected());
  }

  std::mt19937 rng(126);
  alignas(16) uint32_t hashes[4];
  for (int trial = 0; trial < 20000; trial++) {
    for (auto &h : hashes) h = rng() % 3;
    const uint32_t probe = rng() % 3;
    const unsigned expected = detail::MatchHashes32x4Scalar(hashes, probe);
    for (const auto &v : dispatch.variants()) {
      if (v.width <= MaxSimdWidth(cpu)) {
        ASSERT_EQ(expected, v.fn(hashes, probe)) << "variant width " << v.width;
      }
    }
    ASSERT_EQ(expected, MatchHashes32x4(hashes, probe));
  }
#endif
}

}  // namespace
}  // namespace bess::arch
