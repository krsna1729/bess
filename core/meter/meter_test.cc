// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// K5: meter profiles, validation and state against DPDK's rte_meter.
//
// The differential tests are the core claim of the layer: a BESS meter makes
// exactly the colour decisions rte_meter makes for the same inputs, because it
// *is* rte_meter underneath. What BESS adds -- validation reasons, the
// monotonic clock clamp and the shared-meter lock -- is tested on its own.
//
// Profile compilation reads the TSC frequency, so these tests bring up the EAL
// (MeterProfile::Create does it on demand); like the other EAL-backed tests
// they cannot run under ASan.

#include "meter/meter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>
#include <vector>

namespace bess::meter {

class MeterStateTestAccess {
 public:
  static const rte_meter_srtcm &srtcm(const MeterState &s) {
    return s.rte_.srtcm;
  }
  static const rte_meter_trtcm &trtcm(const MeterState &s) {
    return s.rte_.trtcm;
  }
  static const rte_meter_trtcm_rfc4115 &rfc4115(const MeterState &s) {
    return s.rte_.rfc4115;
  }
  static uint64_t last_time(const MeterState &s) { return s.last_time_; }
};

namespace {

using Access = MeterStateTestAccess;

MeterProfile MustProfile(const MeterProfileSpec &spec) {
  auto profile = MeterProfile::Create(spec);
  EXPECT_TRUE(profile.has_value()) << MeterErrorName(profile.error());
  return std::move(profile).value();
}

MeterStatePtr MustState(const MeterProfile &profile, MeterSharing sharing) {
  auto state = MeterState::Create(profile, sharing);
  EXPECT_TRUE(state.has_value()) << MeterErrorName(state.error());
  return std::move(state).value();
}

MeterError ErrorOf(const MeterProfileSpec &spec) {
  auto result = ValidateMeterProfileSpec(spec);
  EXPECT_FALSE(result.has_value());
  return result.error();
}

TEST(MeterValidationTest, SrTcm) {
  EXPECT_TRUE(ValidateMeterProfileSpec(SrTcmSpec{1000, 100, 0}).has_value());
  EXPECT_TRUE(ValidateMeterProfileSpec(SrTcmSpec{1000, 0, 100}).has_value());
  EXPECT_EQ(MeterError::kZeroCommittedRate, ErrorOf(SrTcmSpec{0, 100, 100}));
  EXPECT_EQ(MeterError::kZeroBursts, ErrorOf(SrTcmSpec{1000, 0, 0}));
}

TEST(MeterValidationTest, TrTcm) {
  EXPECT_TRUE(
      ValidateMeterProfileSpec(TrTcmSpec{1000, 100, 1000, 100}).has_value());
  EXPECT_EQ(MeterError::kZeroCommittedRate,
            ErrorOf(TrTcmSpec{0, 100, 1000, 100}));
  EXPECT_EQ(MeterError::kZeroPeakRate, ErrorOf(TrTcmSpec{1000, 100, 0, 100}));
  EXPECT_EQ(MeterError::kPeakBelowCommitted,
            ErrorOf(TrTcmSpec{1000, 100, 999, 100}));
  EXPECT_EQ(MeterError::kZeroCommittedBurst,
            ErrorOf(TrTcmSpec{1000, 0, 2000, 100}));
  EXPECT_EQ(MeterError::kZeroPeakBurst, ErrorOf(TrTcmSpec{1000, 100, 2000, 0}));
}

TEST(MeterValidationTest, TrTcmRfc4115) {
  EXPECT_TRUE(ValidateMeterProfileSpec(TrTcmRfc4115Spec{1000, 100, 0, 0})
                  .has_value());
  EXPECT_TRUE(ValidateMeterProfileSpec(TrTcmRfc4115Spec{0, 0, 1000, 100})
                  .has_value());
  EXPECT_EQ(MeterError::kNoRate, ErrorOf(TrTcmRfc4115Spec{0, 100, 0, 100}));
  EXPECT_EQ(MeterError::kZeroCommittedBurst,
            ErrorOf(TrTcmRfc4115Spec{1000, 0, 0, 0}));
  EXPECT_EQ(MeterError::kZeroExcessBurst,
            ErrorOf(TrTcmRfc4115Spec{0, 0, 1000, 0}));
}

TEST(MeterProfileTest, CreateRejectsInvalidSpecWithReason) {
  auto profile = MeterProfile::Create(TrTcmSpec{2000, 100, 1000, 100});
  ASSERT_FALSE(profile.has_value());
  EXPECT_EQ(MeterError::kPeakBelowCommitted, profile.error());
}

TEST(MeterProfileTest, CompilesEachAlgorithm) {
  const MeterProfile sr = MustProfile(SrTcmSpec{1000, 100, 100});
  const MeterProfile tr = MustProfile(TrTcmSpec{1000, 100, 2000, 100});
  const MeterProfile rfc = MustProfile(TrTcmRfc4115Spec{1000, 100, 500, 100});
  EXPECT_EQ(MeterAlgorithm::kSrTcm, sr.algorithm());
  EXPECT_EQ(MeterAlgorithm::kTrTcm, tr.algorithm());
  EXPECT_EQ(MeterAlgorithm::kTrTcmRfc4115, rfc.algorithm());
  EXPECT_EQ(rte_get_tsc_hz(), sr.tsc_hz());
  EXPECT_NE(0u, sr.tsc_hz());
  EXPECT_EQ(MeterProfileSpec(TrTcmSpec{1000, 100, 2000, 100}), tr.spec());
}

TEST(MeterStateTest, StateFillsOneCacheLine) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1000, 100, 2000, 100});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);
  EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(state.get()) % RTE_CACHE_LINE_SIZE);
  EXPECT_EQ(MeterSharing::kWorkerExclusive, state->sharing());
  EXPECT_EQ(&profile, &state->profile());
}

// srTCM with a one-byte-per-second rate: the refill period is one second of
// TSC cycles, so a test that finishes within that second sees exactly the
// refills it asks for, however slow the machine is.
TEST(MeterStateTest, SrTcmColoursAndRefill) {
  const MeterProfile profile = MustProfile(SrTcmSpec{1, 1000, 500});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);
  const uint64_t hz = profile.tsc_hz();
  const uint64_t t0 = Access::last_time(*state);

  EXPECT_EQ(MeterColor::kGreen, state->Check(t0, 1000));
  EXPECT_EQ(MeterColor::kYellow, state->Check(t0, 500));
  EXPECT_EQ(MeterColor::kRed, state->Check(t0, 1));

  // Ten periods later the committed bucket holds ten bytes.
  const uint64_t t1 = t0 + 10 * hz;
  EXPECT_EQ(MeterColor::kGreen, state->Check(t1, 10));
  EXPECT_EQ(MeterColor::kRed, state->Check(t1, 1));
}

TEST(MeterStateTest, ColorAwareNeverImprovesInputColour) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1, 1000, 1, 2000});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);
  const uint64_t t0 = Access::last_time(*state);

  EXPECT_EQ(MeterColor::kRed,
            state->CheckColorAware(t0, 1, MeterColor::kRed));
  EXPECT_EQ(MeterColor::kYellow,
            state->CheckColorAware(t0, 1, MeterColor::kYellow));
  EXPECT_EQ(MeterColor::kGreen,
            state->CheckColorAware(t0, 1, MeterColor::kGreen));
}

// A `now` older than the meter's last update must not refill it. Raw
// rte_meter wraps the unsigned interval and refills fully -- the reference copy
// below shows that hazard is real -- while the BESS state clamps.
TEST(MeterStateTest, StaleTimestampDoesNotRefill) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1, 1000, 1, 1000});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);
  const uint64_t t0 = Access::last_time(*state);

  // Two refill periods on, so rte_meter's own bucket timestamps move past t0
  // (buckets are already full; the refill is capped away).
  const uint64_t t2 = t0 + 2 * profile.tsc_hz();
  ASSERT_EQ(MeterColor::kGreen, state->Check(t2, 1000));
  ASSERT_EQ(MeterColor::kRed, state->Check(t2, 1000));

  rte_meter_trtcm_profile ref_profile;
  rte_meter_trtcm_params params{1, 1, 1000, 1000};
  ASSERT_EQ(0, rte_meter_trtcm_profile_config(&ref_profile, &params));
  rte_meter_trtcm raw = Access::trtcm(*state);
  EXPECT_EQ(RTE_COLOR_GREEN,
            rte_meter_trtcm_color_blind_check(&raw, &ref_profile, t0, 1000))
      << "raw rte_meter no longer refills on a stale timestamp; the clamp's "
         "rationale needs revisiting";

  EXPECT_EQ(MeterColor::kRed, state->Check(t0, 1000));
  EXPECT_EQ(MeterColor::kRed, state->Check(0, 1000));
}

// The differential proof: for every algorithm, colour mode and sharing mode,
// a BESS meter and a raw rte_meter context started from the same bucket state
// agree on every colour of a long randomized sequence.
struct DifferentialCase {
  MeterProfileSpec spec;
  bool color_aware;
  MeterSharing sharing;
};

class MeterDifferentialTest
    : public ::testing::TestWithParam<DifferentialCase> {};

TEST_P(MeterDifferentialTest, MatchesRteMeter) {
  const DifferentialCase &c = GetParam();
  const MeterProfile profile = MustProfile(c.spec);
  MeterStatePtr state = MustState(profile, c.sharing);
  const uint64_t hz = profile.tsc_hz();

  // The reference: its own profile compiled from the same parameters, and a
  // copy of the BESS state's buckets as they stand.
  rte_meter_srtcm_profile sr_profile{};
  rte_meter_trtcm_profile tr_profile{};
  rte_meter_trtcm_rfc4115_profile rfc_profile{};
  rte_meter_srtcm sr{};
  rte_meter_trtcm tr{};
  rte_meter_trtcm_rfc4115 rfc{};
  switch (profile.algorithm()) {
    case MeterAlgorithm::kSrTcm: {
      const auto &s = std::get<SrTcmSpec>(c.spec);
      rte_meter_srtcm_params p{s.committed_rate, s.committed_burst,
                               s.excess_burst};
      ASSERT_EQ(0, rte_meter_srtcm_profile_config(&sr_profile, &p));
      sr = Access::srtcm(*state);
      break;
    }
    case MeterAlgorithm::kTrTcm: {
      const auto &s = std::get<TrTcmSpec>(c.spec);
      rte_meter_trtcm_params p{s.committed_rate, s.peak_rate,
                               s.committed_burst, s.peak_burst};
      ASSERT_EQ(0, rte_meter_trtcm_profile_config(&tr_profile, &p));
      tr = Access::trtcm(*state);
      break;
    }
    case MeterAlgorithm::kTrTcmRfc4115: {
      const auto &s = std::get<TrTcmRfc4115Spec>(c.spec);
      rte_meter_trtcm_rfc4115_params p{s.committed_rate, s.excess_rate,
                                       s.committed_burst, s.excess_burst};
      ASSERT_EQ(0, rte_meter_trtcm_rfc4115_profile_config(&rfc_profile, &p));
      rfc = Access::rfc4115(*state);
      break;
    }
  }

  std::mt19937_64 rng(0x6b35);
  std::uniform_int_distribution<uint32_t> length(40, 1500);
  // Up to ~1.5 ms between packets: at the rates below that refills anywhere
  // from nothing to a couple of MTUs, so all three colours occur.
  std::uniform_int_distribution<uint64_t> gap(0, hz / 700);
  std::uniform_int_distribution<int> colour(0, 2);

  uint64_t now = Access::last_time(*state);
  int seen[3] = {0, 0, 0};
  for (int i = 0; i < 20000; i++) {
    now += gap(rng);
    const uint32_t bytes = length(rng);
    const auto input = static_cast<MeterColor>(colour(rng));
    const rte_color in = static_cast<rte_color>(input);

    rte_color expected = RTE_COLOR_RED;
    switch (profile.algorithm()) {
      case MeterAlgorithm::kSrTcm:
        expected = c.color_aware ? rte_meter_srtcm_color_aware_check(
                                       &sr, &sr_profile, now, bytes, in)
                                 : rte_meter_srtcm_color_blind_check(
                                       &sr, &sr_profile, now, bytes);
        break;
      case MeterAlgorithm::kTrTcm:
        expected = c.color_aware ? rte_meter_trtcm_color_aware_check(
                                       &tr, &tr_profile, now, bytes, in)
                                 : rte_meter_trtcm_color_blind_check(
                                       &tr, &tr_profile, now, bytes);
        break;
      case MeterAlgorithm::kTrTcmRfc4115:
        expected = c.color_aware
                       ? rte_meter_trtcm_rfc4115_color_aware_check(
                             &rfc, &rfc_profile, now, bytes, in)
                       : rte_meter_trtcm_rfc4115_color_blind_check(
                             &rfc, &rfc_profile, now, bytes);
        break;
    }
    const MeterColor actual =
        c.color_aware ? state->CheckColorAware(now, bytes, input)
                      : state->Check(now, bytes);
    ASSERT_EQ(static_cast<int>(expected), static_cast<int>(actual))
        << "packet " << i;
    seen[static_cast<int>(actual)]++;
  }
  EXPECT_GT(seen[0], 0);
  EXPECT_GT(seen[1], 0);
  EXPECT_GT(seen[2], 0);
}

// About 1 MB/s committed: a 1.5 ms gap refills up to ~1.5 KB.
constexpr SrTcmSpec kSr{1'000'000, 3000, 3000};
constexpr TrTcmSpec kTr{1'000'000, 3000, 2'000'000, 6000};
constexpr TrTcmRfc4115Spec kRfc{1'000'000, 3000, 500'000, 3000};

INSTANTIATE_TEST_SUITE_P(
    AllModes, MeterDifferentialTest,
    ::testing::Values(
        DifferentialCase{kSr, false, MeterSharing::kWorkerExclusive},
        DifferentialCase{kSr, true, MeterSharing::kWorkerExclusive},
        DifferentialCase{kTr, false, MeterSharing::kWorkerExclusive},
        DifferentialCase{kTr, true, MeterSharing::kWorkerExclusive},
        DifferentialCase{kRfc, false, MeterSharing::kWorkerExclusive},
        DifferentialCase{kRfc, true, MeterSharing::kWorkerExclusive},
        DifferentialCase{kSr, false, MeterSharing::kShared},
        DifferentialCase{kTr, true, MeterSharing::kShared},
        DifferentialCase{kRfc, false, MeterSharing::kShared}));

// A shared meter under contention loses no updates. With one-byte-per-second
// rates nothing refills during the test, so the colour counts are exact:
// 10,000 committed bytes admit 100 green 100-byte packets, and the remaining
// 10,000 peak bytes admit 100 yellow ones. A lost read-modify-write would admit
// more. The threads also read their own clocks, so the monotonic clamp is
// exercised with out-of-order timestamps.
TEST(MeterStateTest, SharedMeterLosesNoUpdatesUnderContention) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1, 10000, 1, 20000});
  MeterStatePtr state = MustState(profile, MeterSharing::kShared);

  constexpr int kThreads = 4;
  constexpr int kPerThread = 5000;
  std::atomic<int> counts[3] = {0, 0, 0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      int local[3] = {0, 0, 0};
      for (int i = 0; i < kPerThread; i++) {
        local[static_cast<int>(state->Check(MeterNow(), 100))]++;
      }
      for (int c = 0; c < 3; c++) {
        counts[c] += local[c];
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(100, counts[0].load());
  EXPECT_EQ(100, counts[1].load());
  EXPECT_EQ(kThreads * kPerThread - 200, counts[2].load());
}

}  // namespace
}  // namespace bess::meter
