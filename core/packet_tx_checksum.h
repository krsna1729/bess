// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_PACKET_TX_CHECKSUM_H_
#define BESS_PACKET_TX_CHECKSUM_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "packet_checksum.h"

namespace bess::packet {

// DPDK tunnel type metadata describes encapsulation, not checksum support.
enum class TxTunnelEncoding : uint8_t {
  kNone,
  kGenericIp,
  kGenericUdp,
  kGtp,
};

struct TxEncapsulationLayout {
  TxTunnelEncoding encoding = TxTunnelEncoding::kNone;
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
                          const TxOffloadCapabilities &capabilities) noexcept;
// Validates the fixed profile against this packet, computes software-bound
// fields, and prepares hardware-bound metadata as one final pre-send step.
// Caller retains ownership on success and failure. An error may follow
// software checksum writes, so discard a failed packet rather than retrying or
// transmitting it. TX offload flags and lengths commit only on success.
std::expected<void, ChecksumError> FinalizeTxPacket(
    PacketHandle &packet, const BoundTxFinalizationProfile &profile) noexcept;

// Finalizes valid packets in place, frees rejected packets, and compacts the
// batch before output. Failed packets may already contain software writes.
// An empty profile is a no-op.
TxPacketBatchFinalizeResult FinalizeTxPacketBatch(
    PacketBatch &batch, const BoundTxFinalizationProfile &profile) noexcept;

}  // namespace bess::packet

#endif  // BESS_PACKET_TX_CHECKSUM_H_
