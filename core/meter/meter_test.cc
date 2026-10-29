// SPDX-License-Identifier: BSD-3-Clause

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
#include <cstddef>
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

TEST(MeterStateTest, EmbeddedStorageKeepsTokenStateUntilReconstructed) {
  const auto profile = MustProfile(TrTcmSpec{1000, 100, 2000, 200});
  alignas(MeterState) std::byte storage[sizeof(MeterState)];
  auto made = MeterState::ConstructAt(storage, profile, MeterSharing::kShared);
  ASSERT_TRUE(made.has_value());
  MeterState *state = *made;
  const uint64_t now = MeterNow();
  EXPECT_EQ(state->Check(now, 100), MeterColor::kGreen);
  EXPECT_EQ(state->Check(now, 100), MeterColor::kYellow);
  EXPECT_EQ(state->Check(now, 1), MeterColor::kRed);
  state->~MeterState();
  made = MeterState::ConstructAt(storage, profile, MeterSharing::kShared);
  ASSERT_TRUE(made.has_value());
  state = *made;
  EXPECT_EQ(state->Check(now, 100), MeterColor::kGreen);
  state->~MeterState();
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

// -- RFC-derived expectations -------------------------------------------------
//
// The differential tests prove BESS matches rte_meter; these state what both
// must do in the RFCs' own terms, so a change in DPDK's arithmetic or a wrong
// profile translation shows up as a failed RFC rule, not just a diff.

// RFC 2698 section 2: over PIR is red, over CIR (within PIR) is yellow, else
// green. CIR 1000 B/s, PIR 3000 B/s, both bursts 1000 B; one second of TSC is
// one second of refill, so the token counts below are exact.
TEST(MeterRfc2698Test, ColoursFollowTheCommittedAndPeakBuckets) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1000, 1000, 3000, 1000});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);
  const uint64_t hz = profile.tsc_hz();
  const uint64_t t0 = Access::last_time(*state);

  // Both buckets hold 1000: exactly green, and both are emptied.
  EXPECT_EQ(MeterColor::kGreen, state->Check(t0, 1000));
  // Half a second: CIR has added 500, PIR 1500 (capped at PBS). 700 fits the
  // peak bucket but not the committed one: yellow, and only peak is debited.
  EXPECT_EQ(MeterColor::kYellow, state->Check(t0 + hz / 2, 700));
  // Another half second: committed 500 + 500 = 1000, peak 300 + 1500 capped
  // at 1000. 600 is green against both.
  EXPECT_EQ(MeterColor::kGreen, state->Check(t0 + hz, 600));
  // At the same instant the peak bucket holds 400, below 500: red.
  EXPECT_EQ(MeterColor::kRed, state->Check(t0 + hz, 500));
}

// RFC 2698 section 2.4 (colour-aware): a pre-coloured packet is never
// improved; a yellow packet is tested against PIR only and is debited to the
// peak bucket; a red packet charges nothing.
TEST(MeterRfc2698Test, ColourAwareKeepsAMarkedPacketAtOrBelowItsColour) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1000, 1000, 3000, 1000});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);
  const uint64_t hz = profile.tsc_hz();
  const uint64_t t0 = Access::last_time(*state);

  // Both buckets are full, yet a yellow packet stays yellow (it would be
  // green if the meter ignored its input colour) and pays from peak only.
  EXPECT_EQ(MeterColor::kYellow,
            state->CheckColorAware(t0, 1, MeterColor::kYellow));
  // A red packet stays red and is not charged.
  EXPECT_EQ(MeterColor::kRed,
            state->CheckColorAware(t0, 1, MeterColor::kRed));
  // Committed has 1000, peak 999: a green 999 is green (peak emptied).
  EXPECT_EQ(MeterColor::kGreen,
            state->CheckColorAware(t0, 999, MeterColor::kGreen));
  // Half a second later committed holds 501, peak is back to 1000. 600 fits
  // peak, not committed: the green input becomes yellow, never stays green.
  EXPECT_EQ(MeterColor::kYellow,
            state->CheckColorAware(t0 + hz / 2, 600, MeterColor::kGreen));
}

// RFC 2697: within CBS is green, beyond CBS but within EBS is yellow, else
// red. CIR 1000 B/s, CBS 1000 B, EBS 1000 B.
TEST(MeterRfc2697Test, SrTcmUsesTheCommittedAndExcessBuckets) {
  const MeterProfile profile = MustProfile(SrTcmSpec{1000, 1000, 1000});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);
  const uint64_t t0 = Access::last_time(*state);

  EXPECT_EQ(MeterColor::kGreen, state->Check(t0, 1000));
  EXPECT_EQ(MeterColor::kYellow, state->Check(t0, 500));
  EXPECT_EQ(MeterColor::kRed, state->Check(t0, 600));

  // Colour-aware, full buckets: yellow input stays yellow; a yellow packet
  // with no excess tokens left is red, never green.
  MeterStatePtr fresh = MustState(profile, MeterSharing::kWorkerExclusive);
  const uint64_t f0 = Access::last_time(*fresh);
  EXPECT_EQ(MeterColor::kYellow,
            fresh->CheckColorAware(f0, 1, MeterColor::kYellow));
  EXPECT_EQ(MeterColor::kYellow,
            fresh->CheckColorAware(f0, 999, MeterColor::kYellow));
  EXPECT_EQ(MeterColor::kRed,
            fresh->CheckColorAware(f0, 1, MeterColor::kYellow));
}

// RFC 4115: within CIR is green, beyond CIR but within EIR is yellow, else
// red; green debits committed only and yellow debits excess only.
TEST(MeterRfc4115Test, ColoursFollowTheCommittedAndExcessRates) {
  const MeterProfile profile =
      MustProfile(TrTcmRfc4115Spec{1000, 1000, 2000, 1000});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);
  const uint64_t t0 = Access::last_time(*state);

  EXPECT_EQ(MeterColor::kGreen, state->Check(t0, 1000));  // committed 0
  EXPECT_EQ(MeterColor::kYellow, state->Check(t0, 1000));  // excess 0
  EXPECT_EQ(MeterColor::kRed, state->Check(t0, 1));
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

// Inspect() reports the never-updated state (full buckets at the creation
// stamp), then the same meter after a check: the committed bucket is debited,
// the peak bucket is not, and the last update is the check's TSC.
TEST(MeterStateTest, InspectReportsBucketsProfileAndLastUpdate) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1, 1000, 1, 2000});
  MeterStatePtr state = MustState(profile, MeterSharing::kShared);
  const uint64_t t0 = Access::last_time(*state);

  MeterStateSnapshot fresh = state->Inspect();
  EXPECT_EQ(MeterAlgorithm::kTrTcm, fresh.algorithm);
  EXPECT_EQ(profile.spec(), fresh.spec);
  EXPECT_EQ(MeterSharing::kShared, fresh.sharing);
  EXPECT_EQ(t0, fresh.last_update_tsc);
  ASSERT_EQ(2u, fresh.buckets.size());
  EXPECT_EQ(MeterBucketKind::kCommitted, fresh.buckets[0].kind);
  EXPECT_EQ(1000u, fresh.buckets[0].tokens);
  EXPECT_EQ(1000u, fresh.buckets[0].limit);
  EXPECT_EQ(MeterBucketKind::kPeak, fresh.buckets[1].kind);
  EXPECT_EQ(2000u, fresh.buckets[1].tokens);
  EXPECT_EQ(2000u, fresh.buckets[1].limit);

  const uint64_t t1 = t0 + profile.tsc_hz();
  ASSERT_EQ(MeterColor::kGreen, state->Check(t1, 600));
  MeterStateSnapshot checked = state->Inspect();
  EXPECT_EQ(t1, checked.last_update_tsc);
  EXPECT_EQ(400u, checked.buckets[0].tokens);
  // A trTCM green packet debits the peak bucket too: it held 2000 (refilled
  // and capped) and is now 1400.
  EXPECT_EQ(1400u, checked.buckets[1].tokens);
}

// A drained meter and a check bigger than either bucket: the reading keeps
// reporting zero tokens rather than a negative/wrapped value, and the limits
// stay the configured capacities.
TEST(MeterStateTest, InspectReportsSaturatedAndOverLimitBuckets) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1, 1000, 1, 2000});
  MeterStatePtr state = MustState(profile, MeterSharing::kShared);
  const uint64_t t0 = Access::last_time(*state);

  // A trTCM green packet debits both buckets. 1000 green leaves committed 0
  // and peak 1000; two 500-byte yellow packets then drain the peak bucket.
  ASSERT_EQ(MeterColor::kGreen, state->Check(t0, 1000));
  ASSERT_EQ(MeterColor::kYellow, state->Check(t0, 500));
  ASSERT_EQ(MeterColor::kYellow, state->Check(t0, 500));
  MeterStateSnapshot drained = state->Inspect();
  EXPECT_EQ(0u, drained.buckets[0].tokens);
  EXPECT_EQ(0u, drained.buckets[1].tokens);
  EXPECT_EQ(1000u, drained.buckets[0].limit);
  EXPECT_EQ(2000u, drained.buckets[1].limit);

  EXPECT_EQ(MeterColor::kRed, state->Check(t0, 1500));  // larger than both
  MeterStateSnapshot over = state->Inspect();
  EXPECT_EQ(0u, over.buckets[0].tokens);
  EXPECT_EQ(0u, over.buckets[1].tokens);
}

// The unlocked branch, single-threaded: a worker-exclusive meter is inspected
// correctly when its owner is quiescent, and the snapshot says so, so a caller
// can tell it did not get the shared-meter lock guarantee.
TEST(MeterStateTest, InspectReadsAWorkerExclusiveMeterWhenQuiescent) {
  const MeterProfile profile = MustProfile(TrTcmSpec{1, 1000, 1, 2000});
  MeterStatePtr state = MustState(profile, MeterSharing::kWorkerExclusive);

  MeterStateSnapshot fresh = state->Inspect();
  EXPECT_EQ(MeterSharing::kWorkerExclusive, fresh.sharing);
  EXPECT_EQ(1000u, fresh.buckets[0].tokens);
  EXPECT_EQ(1000u, fresh.buckets[0].limit);
  EXPECT_EQ(2000u, fresh.buckets[1].tokens);
  EXPECT_EQ(2000u, fresh.buckets[1].limit);

  const uint64_t now = Access::last_time(*state);
  ASSERT_EQ(MeterColor::kGreen, state->Check(now, 100));
  MeterStateSnapshot checked = state->Inspect();
  EXPECT_EQ(MeterSharing::kWorkerExclusive, checked.sharing);
  EXPECT_EQ(now, checked.last_update_tsc);
  EXPECT_EQ(900u, checked.buckets[0].tokens);
  EXPECT_EQ(1900u, checked.buckets[1].tokens);
}

// The read path beside running checks: every snapshot is taken under the same
// spinlock a shared check takes, so a reader never observes a half-updated
// bucket pair. (Under TSan this test is the race detector's proof; without it,
// the invariants below are what a torn snapshot would break.) The profile has
// CIR == PIR and equal bursts, so every coherent snapshot keeps the committed
// bucket at or above the peak bucket; a pair read between a green check's two
// token stores can invert that.
TEST(MeterStateTest, InspectIsCoherentUnderConcurrentChecks) {
  const MeterProfile profile =
      MustProfile(TrTcmSpec{1'000'000, 1000, 1'000'000, 1000});
  MeterStatePtr state = MustState(profile, MeterSharing::kShared);

  constexpr int kWriters = 3;
  constexpr int kChecksPerWriter = 100000;
  std::atomic<int> done{0};
  std::atomic<uint64_t> reads{0};
  std::atomic<bool> torn{false};

  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int t = 0; t < kWriters; t++) {
    writers.emplace_back([&] {
      for (int i = 0; i < kChecksPerWriter; i++) {
        state->Check(MeterNow(), 64);
      }
      done.fetch_add(1, std::memory_order_release);
    });
  }
  std::thread reader([&] {
    do {
      const MeterStateSnapshot snapshot = state->Inspect();
      if (snapshot.buckets[0].tokens > snapshot.buckets[0].limit ||
          snapshot.buckets[1].tokens > snapshot.buckets[1].limit ||
          snapshot.buckets[1].tokens > snapshot.buckets[0].tokens ||
          snapshot.buckets[0].kind != MeterBucketKind::kCommitted ||
          snapshot.buckets[1].kind != MeterBucketKind::kPeak) {
        torn.store(true, std::memory_order_relaxed);
      }
      reads.fetch_add(1, std::memory_order_relaxed);
    } while (done.load(std::memory_order_acquire) < kWriters);
  });
  for (auto &writer : writers) {
    writer.join();
  }
  reader.join();

  EXPECT_FALSE(torn.load(std::memory_order_relaxed));
  EXPECT_GT(reads.load(std::memory_order_relaxed), 0u);
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
