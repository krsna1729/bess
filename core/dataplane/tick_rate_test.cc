// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/tick_rate.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace bess::dataplane {
namespace {

using namespace std::chrono_literals;

TEST(TickRate, ConvertsExactlyAtRoundRates) {
  const TickRate ghz(1'000'000'000);
  EXPECT_EQ(30'000'000'000u, ghz.Ticks(30s));
  EXPECT_EQ(1'000'000u, ghz.Ticks(1ms));
  const TickRate tsc(2'994'000'000);  // a 2.994 GHz TSC
  EXPECT_EQ(2'994'000'000u, tsc.Ticks(1s));
  EXPECT_EQ(2'994u, tsc.Ticks(1us));
  EXPECT_EQ(2u, tsc.Ticks(1ns)) << "rounds down";
  EXPECT_EQ(1s, tsc.ToDuration(2'994'000'000));
}

TEST(TickRate, NegativeAndZeroDurationsAreZeroTicks) {
  const TickRate rate(3'000'000'000);
  EXPECT_EQ(0u, rate.Ticks(0s));
  EXPECT_EQ(0u, rate.Ticks(-5s));
  EXPECT_EQ(0u, rate.Ticks(std::chrono::nanoseconds::min()));
}

TEST(TickRate, VeryLongDurationsSaturateInsteadOfWrapping) {
  const TickRate fast(std::numeric_limits<uint64_t>::max());
  const TickRate rate(3'000'000'000);
  const auto forever = std::chrono::nanoseconds::max();
  EXPECT_EQ(std::numeric_limits<uint64_t>::max(), fast.Ticks(forever));
  EXPECT_EQ(std::numeric_limits<uint64_t>::max(), fast.FromNanoseconds(UINT64_MAX));
  // 292 years of nanoseconds at 3 GHz is 2.7e19 ticks: past 2^64.
  EXPECT_EQ(std::numeric_limits<uint64_t>::max(), rate.Ticks(forever));
  EXPECT_EQ(std::chrono::nanoseconds::max(),
            TickRate(1'000'000'000).ToDuration(UINT64_MAX));
  EXPECT_EQ(std::chrono::nanoseconds(6'148'914'691'236'517'205),
            rate.ToDuration(UINT64_MAX));
  // A duration that just fits does not saturate.
  EXPECT_LT(rate.Ticks(std::chrono::hours(24 * 365 * 100)),
            std::numeric_limits<uint64_t>::max());
}

TEST(TickRate, ConversionIsMonotonicInBothDirections) {
  for (const uint64_t hz : {uint64_t{1}, uint64_t{1'000}, uint64_t{999'999'937},
                            uint64_t{2'994'000'000}, uint64_t{4'000'000'000},
                            uint64_t{1} << 40}) {
    const TickRate rate(hz);
    std::mt19937_64 rng(hz);
    std::vector<uint64_t> values(20'000);
    for (uint64_t &v : values) v = rng() >> (rng() % 64);  // every magnitude
    std::sort(values.begin(), values.end());
    for (size_t i = 1; i < values.size(); i++) {
      ASSERT_GE(rate.FromNanoseconds(values[i]), rate.FromNanoseconds(values[i - 1]))
          << "hz " << hz << " ns " << values[i];
      ASSERT_GE(rate.ToDuration(values[i]), rate.ToDuration(values[i - 1]))
          << "hz " << hz << " ticks " << values[i];
    }
  }
}

TEST(TickRate, ARoundTripLosesLessThanOneTick) {
  const TickRate rate(2'994'000'000);
  std::mt19937_64 rng(1);
  for (int i = 0; i < 10'000; i++) {
    const uint64_t ticks = rng() >> 12;
    const uint64_t back = rate.FromNanoseconds(
        static_cast<uint64_t>(rate.ToDuration(ticks).count()));
    ASSERT_LE(back, ticks);
    ASSERT_LE(ticks - back, 3u) << "ns -> ticks -> ns rounds each way by < 1 unit";
  }
}

TEST(TickRate, ShiftForIsTheSmallestPowerOfTwoAtLeastTheGranule) {
  const TickRate rate(1'000'000'000);  // 1 tick = 1 ns
  EXPECT_EQ(0u, rate.ShiftFor(0ns));
  EXPECT_EQ(0u, rate.ShiftFor(1ns));
  EXPECT_EQ(1u, rate.ShiftFor(2ns));
  EXPECT_EQ(2u, rate.ShiftFor(3ns));
  EXPECT_EQ(2u, rate.ShiftFor(4ns));
  EXPECT_EQ(3u, rate.ShiftFor(5ns));
  EXPECT_EQ(20u, rate.ShiftFor(1ms));  // 2^20 ns = 1.05 ms
  EXPECT_EQ(0u, TickRate(1).ShiftFor(1ms)) << "a tick longer than the granule";
}

TEST(TickRate, AZeroRateIsTreatedAsOne) {
  EXPECT_EQ(1u, TickRate(0).per_second());
  EXPECT_EQ(5u, TickRate(0).Ticks(5s));
}

}  // namespace
}  // namespace bess::dataplane
