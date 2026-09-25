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

#ifndef BESS_METER_METER_H_
#define BESS_METER_METER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <variant>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_meter.h>
#include <rte_spinlock.h>

#include "dataplane/strong_id.h"

namespace bess::meter {

// Generic software metering (K5).
//
// The split of responsibility is the point of this layer:
//
//   DPDK `rte_meter` owns the algorithms -- srTCM (RFC 2697), trTCM
//   (RFC 2698) and trTCM (RFC 4115): token-bucket refill arithmetic, bucket
//   limits and the colour decision. None of that is reimplemented here.
//
//   BESS owns the resource around them -- typed profile specifications and
//   their validation, stable meter ids, where state lives and who may touch
//   it, generation-safe publication (MeterSet, meter_set.h), and a batch
//   invocation surface.
//
//   Applications own what a meter *means*: which bytes are counted (L2 frame,
//   IP packet, payload), what green/yellow/red do to a packet, and any
//   hierarchy of meters (session, APN, slice). This layer returns a colour and
//   nothing else.
//
// Time is TSC cycles, the unit `rte_meter` works in (`rte_get_tsc_cycles()`).
// Rates are bytes per second and bursts are bytes, as in `rte_meter`.

// The colour a meter assigns. The values are `rte_color`'s, so conversion in
// either direction is a cast.
enum class MeterColor : uint8_t {
  kGreen = RTE_COLOR_GREEN,
  kYellow = RTE_COLOR_YELLOW,
  kRed = RTE_COLOR_RED,
};

static_assert(static_cast<int>(MeterColor::kGreen) == RTE_COLOR_GREEN);
static_assert(static_cast<int>(MeterColor::kYellow) == RTE_COLOR_YELLOW);
static_assert(static_cast<int>(MeterColor::kRed) == RTE_COLOR_RED);

enum class MeterAlgorithm : uint8_t {
  kSrTcm,         // RFC 2697 single-rate three-colour marker
  kTrTcm,         // RFC 2698 two-rate three-colour marker
  kTrTcmRfc4115,  // RFC 4115 two-rate three-colour marker
};

// Who may run a meter's check.
//
// `rte_meter` state is plain data updated with non-atomic read-modify-write,
// so concurrent checks of one meter lose updates. BESS makes the choice
// explicit per meter instead of guessing:
//
//  - kWorkerExclusive: exactly one worker checks this meter (for example, the
//    flow is pinned to a worker by RSS or by the pipeline). No synchronization
//    is performed; running it from two workers is a caller bug.
//  - kShared: any worker may check it. Each check takes the meter's own
//    spinlock. This is correct but serializes contending workers on one cache
//    line; prefer exclusive meters where the traffic placement allows it.
enum class MeterSharing : uint8_t {
  kWorkerExclusive,
  kShared,
};

// A stable meter id: one-based like ActionId, with zero reserved as invalid.
struct MeterIdTag;
using MeterId = dataplane::StrongId<MeterIdTag, uint32_t>;
inline constexpr MeterId kInvalidMeterId{};

// -- profile specification ---------------------------------------------------

// RFC 2697: one rate, a committed and an excess bucket.
struct SrTcmSpec {
  uint64_t committed_rate = 0;   // CIR, bytes/s; must be non-zero
  uint64_t committed_burst = 0;  // CBS, bytes
  uint64_t excess_burst = 0;     // EBS, bytes; CBS and EBS not both zero

  friend bool operator==(const SrTcmSpec &, const SrTcmSpec &) = default;
};

// RFC 2698: committed and peak rates; PIR >= CIR.
struct TrTcmSpec {
  uint64_t committed_rate = 0;   // CIR, bytes/s; non-zero
  uint64_t committed_burst = 0;  // CBS, bytes; non-zero
  uint64_t peak_rate = 0;        // PIR, bytes/s; non-zero and >= CIR
  uint64_t peak_burst = 0;       // PBS, bytes; non-zero

  friend bool operator==(const TrTcmSpec &, const TrTcmSpec &) = default;
};

// RFC 4115: committed and excess rates, independent buckets.
struct TrTcmRfc4115Spec {
  uint64_t committed_rate = 0;   // CIR, bytes/s
  uint64_t committed_burst = 0;  // CBS, bytes; non-zero if CIR is
  uint64_t excess_rate = 0;      // EIR, bytes/s
  uint64_t excess_burst = 0;     // EBS, bytes; non-zero if EIR is

  friend bool operator==(const TrTcmRfc4115Spec &,
                         const TrTcmRfc4115Spec &) = default;
};

using MeterProfileSpec = std::variant<SrTcmSpec, TrTcmSpec, TrTcmRfc4115Spec>;

enum class MeterError : uint8_t {
  kZeroCommittedRate,
  kZeroCommittedBurst,
  kZeroBursts,  // srTCM: CBS and EBS both zero
  kZeroPeakRate,
  kZeroPeakBurst,
  kPeakBelowCommitted,
  kZeroExcessBurst,
  kNoRate,               // RFC 4115: CIR and EIR both zero
  kClockUnavailable,     // TSC frequency unknown (EAL not initialized)
  kRejectedByBackend,    // rte_meter refused a spec BESS accepted
  kOutOfMemory,
  kInvalidId,            // zero or beyond the set's capacity
  kDuplicateId,
  kUnknownId,
};

const char *MeterErrorName(MeterError error);

// BESS's own validation, with a reason per rule. It is at least as strict as
// `rte_meter`'s parameter check; the one addition is that an RFC 4115 profile
// with both rates zero is refused, since it can only ever colour red.
std::expected<void, MeterError> ValidateMeterProfileSpec(
    const MeterProfileSpec &spec);

MeterAlgorithm AlgorithmOf(const MeterProfileSpec &spec);

// -- profile -----------------------------------------------------------------

// An immutable, validated, compiled profile: the `rte_meter_*_profile` for one
// specification. Many meters share one profile (MeterSet deduplicates them);
// only per-meter state is written on the packet path.
//
// Compilation reads the TSC frequency, so the EAL must be up. Create()
// initializes it on demand the way the other DPDK-backed substrates do.
class MeterProfile {
 public:
  static std::expected<MeterProfile, MeterError> Create(
      const MeterProfileSpec &spec);

  MeterAlgorithm algorithm() const noexcept { return algorithm_; }
  const MeterProfileSpec &spec() const noexcept { return spec_; }

  // The TSC frequency the profile was compiled against.
  uint64_t tsc_hz() const noexcept { return tsc_hz_; }

 private:
  friend class MeterState;

  MeterProfile() = default;

  // `rte_meter`'s config and check functions take a non-const profile pointer
  // but only read it (DPDK 25.11 lib/meter), so handing out a mutable pointer
  // to an immutable profile is safe and keeps the constness honest here.
  rte_meter_srtcm_profile *srtcm() const noexcept {
    return const_cast<rte_meter_srtcm_profile *>(&rte_.srtcm);
  }
  rte_meter_trtcm_profile *trtcm() const noexcept {
    return const_cast<rte_meter_trtcm_profile *>(&rte_.trtcm);
  }
  rte_meter_trtcm_rfc4115_profile *rfc4115() const noexcept {
    return const_cast<rte_meter_trtcm_rfc4115_profile *>(&rte_.rfc4115);
  }

  union RteProfile {
    rte_meter_srtcm_profile srtcm;
    rte_meter_trtcm_profile trtcm;
    rte_meter_trtcm_rfc4115_profile rfc4115;
  };

  RteProfile rte_{};
  MeterAlgorithm algorithm_ = MeterAlgorithm::kSrTcm;
  uint64_t tsc_hz_ = 0;
  MeterProfileSpec spec_;
};

// -- state -------------------------------------------------------------------

class MeterState;

// Frees a MeterState allocated by MeterState::Create().
struct MeterStateDeleter {
  void operator()(MeterState *state) const noexcept;
};

using MeterStatePtr = std::unique_ptr<MeterState, MeterStateDeleter>;

// One meter: `rte_meter`'s token buckets, the profile they run under, and the
// BESS-owned fields around them. Exactly one cache line, so exclusive meters
// run by different workers never false-share.
//
// A state is bound to its profile for life -- the profile is part of the
// state, not an argument to Check() -- so a meter cannot be checked against
// the wrong parameters. The profile must outlive the state; MeterSet
// guarantees that by owning both.
class alignas(RTE_CACHE_LINE_SIZE) MeterState {
 public:
  // Allocates (on `socket`, a DPDK socket id or SOCKET_ID_ANY) and configures
  // a standalone state with full buckets, the `rte_meter` initial condition.
  // MeterSet allocates its states from slabs instead; this is for callers
  // that manage a meter on their own.
  static std::expected<MeterStatePtr, MeterError> Create(
      const MeterProfile &profile, MeterSharing sharing,
      int socket = SOCKET_ID_ANY);

  MeterState(const MeterState &) = delete;
  MeterState &operator=(const MeterState &) = delete;

  const MeterProfile &profile() const noexcept { return *profile_; }
  MeterSharing sharing() const noexcept { return sharing_; }

  // Colour-blind check of `bytes` at TSC time `now`.
  MeterColor Check(uint64_t now, uint32_t bytes) noexcept {
    return Run<false>(now, bytes, MeterColor::kGreen);
  }

  // Colour-aware check: `input` is the colour a previous stage assigned; the
  // result is never better than it.
  MeterColor CheckColorAware(uint64_t now, uint32_t bytes,
                             MeterColor input) noexcept {
    return Run<true>(now, bytes, input);
  }

 private:
  friend struct MeterStateDeleter;
  friend class MeterSetBuilder;
  friend class MeterStateTestAccess;

  MeterState(const MeterProfile &profile, MeterSharing sharing)
      : profile_(&profile), sharing_(sharing) {
    rte_spinlock_init(&lock_);
  }
  ~MeterState() = default;

  // Constructs and configures a state in `memory` (one suitably aligned cache
  // line). On failure nothing is left constructed.
  static std::expected<MeterState *, MeterError> ConstructAt(
      void *memory, const MeterProfile &profile, MeterSharing sharing);

  template <bool kColorAware>
  MeterColor Run(uint64_t now, uint32_t bytes, MeterColor input) noexcept {
    if (sharing_ == MeterSharing::kShared) {
      rte_spinlock_lock(&lock_);
      const MeterColor color = RunLocked<kColorAware>(now, bytes, input);
      rte_spinlock_unlock(&lock_);
      return color;
    }
    return RunLocked<kColorAware>(now, bytes, input);
  }

  template <bool kColorAware>
  MeterColor RunLocked(uint64_t now, uint32_t bytes, MeterColor input) noexcept;

  union RteState {
    rte_meter_srtcm srtcm;
    rte_meter_trtcm trtcm;
    rte_meter_trtcm_rfc4115 rfc4115;
  };

  RteState rte_{};
  const MeterProfile *const profile_;

  // The latest time this meter has been checked at. `rte_meter` subtracts its
  // last update time from `now` in unsigned arithmetic, so a `now` older than
  // the last update -- a stale batch timestamp, or two workers' timestamps
  // arriving out of order at a shared meter -- wraps into an enormous interval
  // and refills the buckets completely. Clamping `now` to this value makes the
  // algorithm's input monotonic; a late caller is metered as if it arrived at
  // `last_time_`, which can only be stricter, never more permissive.
  uint64_t last_time_ = 0;

  rte_spinlock_t lock_;
  const MeterSharing sharing_;
};

static_assert(sizeof(MeterState) == RTE_CACHE_LINE_SIZE,
              "a meter's state is expected to fill exactly one cache line");

template <bool kColorAware>
inline MeterColor MeterState::RunLocked(uint64_t now, uint32_t bytes,
                                        MeterColor input) noexcept {
  now = std::max(now, last_time_);
  last_time_ = now;

  const MeterProfile &p = *profile_;
  const rte_color in = static_cast<rte_color>(input);
  rte_color out;
  switch (p.algorithm()) {
    case MeterAlgorithm::kSrTcm:
      out = kColorAware ? rte_meter_srtcm_color_aware_check(
                              &rte_.srtcm, p.srtcm(), now, bytes, in)
                        : rte_meter_srtcm_color_blind_check(
                              &rte_.srtcm, p.srtcm(), now, bytes);
      break;
    case MeterAlgorithm::kTrTcm:
      out = kColorAware ? rte_meter_trtcm_color_aware_check(
                              &rte_.trtcm, p.trtcm(), now, bytes, in)
                        : rte_meter_trtcm_color_blind_check(
                              &rte_.trtcm, p.trtcm(), now, bytes);
      break;
    case MeterAlgorithm::kTrTcmRfc4115:
    default:
      out = kColorAware ? rte_meter_trtcm_rfc4115_color_aware_check(
                              &rte_.rfc4115, p.rfc4115(), now, bytes, in)
                        : rte_meter_trtcm_rfc4115_color_blind_check(
                              &rte_.rfc4115, p.rfc4115(), now, bytes);
      break;
  }
  return static_cast<MeterColor>(out);
}

// The clock meters run on. One read per batch is the intended use.
inline uint64_t MeterNow() noexcept { return rte_get_tsc_cycles(); }

}  // namespace bess::meter

#endif  // BESS_METER_METER_H_
