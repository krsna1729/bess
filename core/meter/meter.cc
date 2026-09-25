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

#include "meter/meter.h"

#include <new>
#include <type_traits>

#include <rte_malloc.h>

#include "dpdk.h"

namespace bess::meter {

const char *MeterErrorName(MeterError error) {
  switch (error) {
    case MeterError::kZeroCommittedRate:
      return "committed rate must be non-zero";
    case MeterError::kZeroCommittedBurst:
      return "committed burst must be non-zero";
    case MeterError::kZeroBursts:
      return "committed and excess bursts must not both be zero";
    case MeterError::kZeroPeakRate:
      return "peak rate must be non-zero";
    case MeterError::kZeroPeakBurst:
      return "peak burst must be non-zero";
    case MeterError::kPeakBelowCommitted:
      return "peak rate must not be below the committed rate";
    case MeterError::kZeroExcessBurst:
      return "excess burst must be non-zero when the excess rate is";
    case MeterError::kNoRate:
      return "committed and excess rates must not both be zero";
    case MeterError::kClockUnavailable:
      return "TSC frequency is unavailable";
    case MeterError::kRejectedByBackend:
      return "rte_meter rejected the profile";
    case MeterError::kOutOfMemory:
      return "out of memory for meter state";
    case MeterError::kInvalidId:
      return "meter id is zero or beyond the configured capacity";
    case MeterError::kDuplicateId:
      return "meter id is already in use";
    case MeterError::kUnknownId:
      return "no meter has this id";
  }
  return "unknown meter error";
}

MeterAlgorithm AlgorithmOf(const MeterProfileSpec &spec) {
  return std::visit(
      [](const auto &s) {
        using S = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<S, SrTcmSpec>) {
          return MeterAlgorithm::kSrTcm;
        } else if constexpr (std::is_same_v<S, TrTcmSpec>) {
          return MeterAlgorithm::kTrTcm;
        } else {
          return MeterAlgorithm::kTrTcmRfc4115;
        }
      },
      spec);
}

namespace {

std::expected<void, MeterError> Validate(const SrTcmSpec &s) {
  if (s.committed_rate == 0) {
    return std::unexpected(MeterError::kZeroCommittedRate);
  }
  if (s.committed_burst == 0 && s.excess_burst == 0) {
    return std::unexpected(MeterError::kZeroBursts);
  }
  return {};
}

std::expected<void, MeterError> Validate(const TrTcmSpec &s) {
  if (s.committed_rate == 0) {
    return std::unexpected(MeterError::kZeroCommittedRate);
  }
  if (s.peak_rate == 0) {
    return std::unexpected(MeterError::kZeroPeakRate);
  }
  if (s.peak_rate < s.committed_rate) {
    return std::unexpected(MeterError::kPeakBelowCommitted);
  }
  if (s.committed_burst == 0) {
    return std::unexpected(MeterError::kZeroCommittedBurst);
  }
  if (s.peak_burst == 0) {
    return std::unexpected(MeterError::kZeroPeakBurst);
  }
  return {};
}

std::expected<void, MeterError> Validate(const TrTcmRfc4115Spec &s) {
  if (s.committed_rate == 0 && s.excess_rate == 0) {
    return std::unexpected(MeterError::kNoRate);
  }
  if (s.committed_rate != 0 && s.committed_burst == 0) {
    return std::unexpected(MeterError::kZeroCommittedBurst);
  }
  if (s.excess_rate != 0 && s.excess_burst == 0) {
    return std::unexpected(MeterError::kZeroExcessBurst);
  }
  return {};
}

}  // namespace

std::expected<void, MeterError> ValidateMeterProfileSpec(
    const MeterProfileSpec &spec) {
  return std::visit([](const auto &s) { return Validate(s); }, spec);
}

std::expected<MeterProfile, MeterError> MeterProfile::Create(
    const MeterProfileSpec &spec) {
  if (auto valid = ValidateMeterProfileSpec(spec); !valid) {
    return std::unexpected(valid.error());
  }

  // rte_meter derives its refill period from the TSC frequency. Before EAL
  // initialization that frequency reads as zero, which would compile a profile
  // with a zero period -- a division by zero on the first check.
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  const uint64_t hz = rte_get_tsc_hz();
  if (hz == 0) {
    return std::unexpected(MeterError::kClockUnavailable);
  }

  MeterProfile profile;
  profile.spec_ = spec;
  profile.algorithm_ = AlgorithmOf(spec);
  profile.tsc_hz_ = hz;

  int ret = 0;
  switch (profile.algorithm_) {
    case MeterAlgorithm::kSrTcm: {
      const auto &s = std::get<SrTcmSpec>(spec);
      rte_meter_srtcm_params params{s.committed_rate, s.committed_burst,
                                    s.excess_burst};
      ret = rte_meter_srtcm_profile_config(&profile.rte_.srtcm, &params);
      break;
    }
    case MeterAlgorithm::kTrTcm: {
      const auto &s = std::get<TrTcmSpec>(spec);
      rte_meter_trtcm_params params{s.committed_rate, s.peak_rate,
                                    s.committed_burst, s.peak_burst};
      ret = rte_meter_trtcm_profile_config(&profile.rte_.trtcm, &params);
      break;
    }
    case MeterAlgorithm::kTrTcmRfc4115: {
      const auto &s = std::get<TrTcmRfc4115Spec>(spec);
      rte_meter_trtcm_rfc4115_params params{s.committed_rate, s.excess_rate,
                                            s.committed_burst, s.excess_burst};
      ret = rte_meter_trtcm_rfc4115_profile_config(&profile.rte_.rfc4115,
                                                   &params);
      break;
    }
  }
  if (ret != 0) {
    return std::unexpected(MeterError::kRejectedByBackend);
  }
  return profile;
}

std::expected<MeterState *, MeterError> MeterState::ConstructAt(
    void *memory, const MeterProfile &profile, MeterSharing sharing) {
  auto *state = new (memory) MeterState(profile, sharing);

  int ret = 0;
  switch (profile.algorithm()) {
    case MeterAlgorithm::kSrTcm:
      ret = rte_meter_srtcm_config(&state->rte_.srtcm, profile.srtcm());
      break;
    case MeterAlgorithm::kTrTcm:
      ret = rte_meter_trtcm_config(&state->rte_.trtcm, profile.trtcm());
      break;
    case MeterAlgorithm::kTrTcmRfc4115:
      ret = rte_meter_trtcm_rfc4115_config(&state->rte_.rfc4115,
                                           profile.rfc4115());
      break;
  }
  if (ret != 0) {
    state->~MeterState();
    return std::unexpected(MeterError::kRejectedByBackend);
  }

  // rte_meter stamped its buckets with the TSC inside the config call above.
  // A fenced read afterwards on the same thread bounds that stamp from above,
  // so the monotonic clamp never hands rte_meter a time before it.
  state->last_time_ = rte_rdtsc_precise();
  return state;
}

std::expected<MeterStatePtr, MeterError> MeterState::Create(
    const MeterProfile &profile, MeterSharing sharing, int socket) {
  void *memory = rte_malloc_socket("bess_meter_state", sizeof(MeterState),
                                   alignof(MeterState), socket);
  if (memory == nullptr && socket != SOCKET_ID_ANY) {
    // Placement is a performance preference, not a requirement: a socket with
    // no memory left (or none at all) should not make a meter impossible.
    memory = rte_malloc_socket("bess_meter_state", sizeof(MeterState),
                               alignof(MeterState), SOCKET_ID_ANY);
  }
  if (memory == nullptr) {
    return std::unexpected(MeterError::kOutOfMemory);
  }
  auto state = ConstructAt(memory, profile, sharing);
  if (!state) {
    rte_free(memory);
    return std::unexpected(state.error());
  }
  return MeterStatePtr(*state);
}

void MeterStateDeleter::operator()(MeterState *state) const noexcept {
  if (state != nullptr) {
    state->~MeterState();
    rte_free(state);
  }
}

}  // namespace bess::meter
