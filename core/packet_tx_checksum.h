// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
//   contributors may be used to endorse or promote products derived from this
//   software without specific prior written permission.
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

#ifndef BESS_PACKET_TX_CHECKSUM_H_
#define BESS_PACKET_TX_CHECKSUM_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "packet_checksum.h"

namespace bess::packet {

// DPDK requires a generic IP or UDP tunnel kind to describe offloads for
// tunneled inner headers. This describes encapsulation only; it does not imply
// which checksum operations the profile requests.
enum class TxEncapsulationKind : uint8_t {
  kNone,
  kIp,
  kUdp,
};

struct TxEncapsulationLayout {
  TxEncapsulationKind kind = TxEncapsulationKind::kNone;
  IpVersion outer_ip_version = IpVersion::kIpv4;
  size_t outer_network_offset = 0;
};

// Fixed per-output intent. `outer` is the top-level IP domain for ordinary
// packets and the encapsulating IP domain when `encapsulation` is present.
// `inner` describes at most one encapsulated IP domain.
struct TxFinalizationProfile {
  TxEncapsulationLayout encapsulation;
  std::optional<ChecksumPlan> outer;
  std::optional<ChecksumPlan> inner;
};

enum class TxChecksumBackend : uint8_t {
  kNone,
  kSoftware,
  kHardware,
};

struct BoundTxChecksumDomain {
  ChecksumPlan plan;
  TxChecksumBackend network_backend = TxChecksumBackend::kNone;
  TxChecksumBackend transport_backend = TxChecksumBackend::kNone;
};

// Bound once during output-module initialization. It contains no per-packet
// state and exposes no backend-specific offload flags or header lengths.
struct BoundTxFinalizationProfile {
  TxEncapsulationLayout encapsulation;
  std::optional<BoundTxChecksumDomain> outer;
  std::optional<BoundTxChecksumDomain> inner;
  bool multi_segment_tx = false;

  bool enabled() const noexcept { return outer.has_value() || inner.has_value(); }
};

struct TxPacketBatchFinalizeResult {
  uint32_t rejected = 0;
};

std::expected<BoundTxFinalizationProfile, ChecksumError>
BindTxFinalizationProfile(const TxFinalizationProfile &profile,
                          const TxChecksumCapabilities &capabilities) noexcept;

// Validates the fixed profile against this packet, computes software-bound
// fields, and prepares hardware-bound metadata as one final pre-send step.
std::expected<void, ChecksumError> FinalizeTxPacket(
    PacketHandle &packet, const BoundTxFinalizationProfile &profile) noexcept;

// Finalizes valid packets in place, frees rejected packets, and compacts the
// batch before output. An empty profile is a no-op.
TxPacketBatchFinalizeResult FinalizeTxPacketBatch(
    PacketBatch &batch, const BoundTxFinalizationProfile &profile) noexcept;

}  // namespace bess::packet

#endif  // BESS_PACKET_TX_CHECKSUM_H_
