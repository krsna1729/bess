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

#include "packet_tx_checksum.h"

#include <algorithm>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>

#include <rte_ip.h>
#include <rte_mbuf.h>

#include "packet.h"
#include "packet_reshape.h"
#include "utils/ip.h"
#include "utils/udp.h"

namespace bess::packet {
namespace {

constexpr uint64_t kTxChecksumFlags =
    RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_IP_CKSUM |
    RTE_MBUF_F_TX_L4_MASK | RTE_MBUF_F_TX_OUTER_IP_CKSUM |
    RTE_MBUF_F_TX_OUTER_UDP_CKSUM | RTE_MBUF_F_TX_OUTER_IPV4 |
    RTE_MBUF_F_TX_OUTER_IPV6 | RTE_MBUF_F_TX_TUNNEL_MASK;

struct OuterIpLayout {
  IpVersion version = IpVersion::kIpv4;
  size_t header_length = 0;
  size_t network_length = 0;
  uint8_t protocol = 0;
};

struct HardwareWrite {
  size_t offset = 0;
  std::array<std::byte, sizeof(uint16_t)> bytes{};
};

struct TxChecksumMetadata {
  uint64_t flags = 0;
  std::array<HardwareWrite, 4> writes{};
  size_t write_count = 0;
  uint16_t l2_len = 0;
  uint16_t l3_len = 0;
  uint16_t l4_len = 0;
  uint16_t outer_l2_len = 0;
  uint16_t outer_l3_len = 0;
  bool set_main_lengths = false;
  bool set_outer_lengths = false;
};

bool RangeWithinPacket(size_t packet_length, size_t offset,
                       size_t length) noexcept {
  return offset <= packet_length && length <= packet_length - offset;
}

bool ReadBytes(PacketRef packet, size_t offset,
               std::span<std::byte> output) noexcept {
  if (!RangeWithinPacket(packet.total_len(), offset, output.size())) {
    return false;
  }
  PacketHandle segment = packet.handle();
  for (uint16_t index = 0; index < packet.nb_segs() && !output.empty(); index++) {
    if (segment == nullptr) {
      return false;
    }
    if (offset >= segment->data_len) {
      offset -= segment->data_len;
      segment = segment->next;
      continue;
    }
    const size_t chunk =
        std::min(output.size(), static_cast<size_t>(segment->data_len) - offset);
    const auto *source = PacketRef(segment).head_data<const std::byte *>(
        static_cast<uint16_t>(offset));
    std::memcpy(output.data(), source, chunk);
    output = output.subspan(chunk);
    offset = 0;
    segment = segment->next;
  }
  return output.empty();
}

template <typename Header>
bool ReadHeaderAt(PacketRef packet, size_t offset, Header *header) noexcept {
  return ReadBytes(packet, offset,
                   std::span<std::byte>(reinterpret_cast<std::byte *>(header),
                                        sizeof(Header)));
}

bool IsValidIpVersion(IpVersion version) noexcept {
  return version == IpVersion::kIpv4 || version == IpVersion::kIpv6;
}

bool IsValidChecksumPlan(const ChecksumPlan &plan) noexcept {
  if (!IsValidIpVersion(plan.ip_version)) {
    return false;
  }
  switch (plan.network) {
    case NetworkChecksum::kNone:
      break;
    case NetworkChecksum::kIpv4Header:
      if (plan.ip_version != IpVersion::kIpv4) {
        return false;
      }
      break;
    default:
      return false;
  }
  switch (plan.transport) {
    case TransportChecksum::kNone:
      break;
    case TransportChecksum::kUdp:
    case TransportChecksum::kTcp:
      if (plan.transport_offset <= plan.network_offset) {
        return false;
      }
      break;
    default:
      return false;
  }
  return plan.network != NetworkChecksum::kNone ||
         plan.transport != TransportChecksum::kNone;
}

bool HasHardware(const BoundTxChecksumDomain &domain) noexcept {
  return domain.network_backend == TxChecksumBackend::kHardware ||
         domain.transport_backend == TxChecksumBackend::kHardware;
}

bool IsHardware(TxChecksumBackend backend) noexcept {
  return backend == TxChecksumBackend::kHardware;
}

bool IsSoftware(TxChecksumBackend backend) noexcept {
  return backend == TxChecksumBackend::kSoftware;
}

bool FitsBitfield(size_t value, unsigned bits) noexcept {
  return bits < std::numeric_limits<size_t>::digits &&
         value < (size_t{1} << bits);
}

std::expected<uint16_t, ChecksumError> CheckedLength(size_t value,
                                                     unsigned bits) noexcept {
  if (value > UINT16_MAX || !FitsBitfield(value, bits)) {
    return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
  }
  return static_cast<uint16_t>(value);
}

bool FitsHeadSegment(PacketHandle packet, size_t offset,
                     size_t length) noexcept {
  return offset <= packet->data_len &&
         length <= static_cast<size_t>(packet->data_len) - offset;
}

std::expected<OuterIpLayout, ChecksumError> ReadOuterIp(
    PacketRef packet, size_t offset, IpVersion version) noexcept {
  OuterIpLayout layout;
  layout.version = version;
  if (version == IpVersion::kIpv4) {
    rte_ipv4_hdr ip{};
    if (!ReadHeaderAt(packet, offset, &ip)) {
      return std::unexpected(ChecksumError::kLengthOutOfRange);
    }
    const uint8_t ip_version = ip.version_ihl >> 4;
    const uint8_t ihl = ip.version_ihl & 0x0f;
    if (ip_version != 4 || ihl < 5) {
      return std::unexpected(ChecksumError::kInvalidIpv4Header);
    }
    layout.header_length = static_cast<size_t>(ihl) * 4;
    layout.network_length = rte_be_to_cpu_16(ip.total_length);
    layout.protocol = ip.next_proto_id;
    if (layout.header_length > layout.network_length) {
      return std::unexpected(ChecksumError::kInvalidIpv4Header);
    }
    if (!RangeWithinPacket(packet.total_len(), offset,
                           layout.network_length)) {
      return std::unexpected(ChecksumError::kLengthOutOfRange);
    }
    return layout;
  }
  if (version != IpVersion::kIpv6) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }

  rte_ipv6_hdr ip{};
  if (!ReadHeaderAt(packet, offset, &ip)) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }
  if ((rte_be_to_cpu_32(ip.vtc_flow) >> 28) != 6) {
    return std::unexpected(ChecksumError::kInvalidIpv6Header);
  }
  const size_t payload_length = rte_be_to_cpu_16(ip.payload_len);
  if (payload_length == 0) {
    return std::unexpected(ChecksumError::kUnsupportedJumbogram);
  }
  layout.header_length = sizeof(rte_ipv6_hdr);
  layout.network_length = layout.header_length + payload_length;
  layout.protocol = ip.proto;
  if (!RangeWithinPacket(packet.total_len(), offset, layout.network_length)) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }
  return layout;
}

std::expected<OuterIpLayout, ChecksumError> ValidateEncapsulation(
    PacketRef packet, const BoundTxFinalizationProfile &profile,
    const std::optional<ChecksumLayout> &inner_layout) noexcept {
  if (profile.encapsulation.kind == TxEncapsulationKind::kNone) {
    if (profile.inner) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    return OuterIpLayout{};
  }
  if (profile.encapsulation.kind != TxEncapsulationKind::kIp &&
      profile.encapsulation.kind != TxEncapsulationKind::kUdp) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  if (!IsValidIpVersion(profile.encapsulation.outer_ip_version)) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }

  const size_t outer_offset = profile.encapsulation.outer_network_offset;
  auto outer = ReadOuterIp(packet, outer_offset,
                           profile.encapsulation.outer_ip_version);
  if (!outer) {
    return std::unexpected(outer.error());
  }

  if (profile.encapsulation.kind == TxEncapsulationKind::kIp) {
    if (outer->protocol != 4 && outer->protocol != 41) {
      return std::unexpected(ChecksumError::kProtocolMismatch);
    }
    if (profile.inner) {
      const uint8_t inner_protocol =
          profile.inner->plan.ip_version == IpVersion::kIpv4 ? 4 : 41;
      if (outer->protocol != inner_protocol) {
        return std::unexpected(ChecksumError::kProtocolMismatch);
      }
      const size_t inner_offset = profile.inner->plan.network_offset;
      const size_t outer_end = outer_offset + outer->network_length;
      if (inner_offset < outer_offset + outer->header_length || !inner_layout ||
          !RangeWithinPacket(outer_end, inner_offset,
                             inner_layout->network_length)) {
        return std::unexpected(ChecksumError::kInvalidPlan);
      }
    }
    return outer;
  }

  if (outer->protocol != utils::Ipv4::kUdp) {
    return std::unexpected(ChecksumError::kProtocolMismatch);
  }
  const size_t udp_offset = outer_offset + outer->header_length;
  utils::Udp udp{};
  if (!ReadHeaderAt(packet, udp_offset, &udp)) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }
  const size_t udp_length = udp.length.value();
  const size_t outer_payload_length =
      outer->network_length - outer->header_length;
  if (udp_length < sizeof(utils::Udp) || udp_length != outer_payload_length) {
    return std::unexpected(ChecksumError::kInvalidTransportHeader);
  }
  if (profile.inner) {
    const size_t inner_offset = profile.inner->plan.network_offset;
    const size_t encapsulation_end = udp_offset + udp_length;
    if (inner_offset < udp_offset + sizeof(utils::Udp) || !inner_layout ||
        !RangeWithinPacket(encapsulation_end, inner_offset,
                           inner_layout->network_length)) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
  }
  return outer;
}

std::expected<rte_be16_t, ChecksumError> PseudoHeaderSeed(
    PacketRef packet, const ChecksumPlan &plan) noexcept {
  if (plan.ip_version == IpVersion::kIpv4) {
    rte_ipv4_hdr ip{};
    if (!ReadHeaderAt(packet, plan.network_offset, &ip)) {
      return std::unexpected(ChecksumError::kLengthOutOfRange);
    }
    // DPDK's helper contract requires a zero IPv4 header checksum.
    ip.hdr_checksum = 0;
    return rte_ipv4_phdr_cksum(&ip, 0);
  }
  if (plan.ip_version == IpVersion::kIpv6) {
    rte_ipv6_hdr ip{};
    if (!ReadHeaderAt(packet, plan.network_offset, &ip)) {
      return std::unexpected(ChecksumError::kLengthOutOfRange);
    }
    return rte_ipv6_phdr_cksum(&ip, 0);
  }
  return std::unexpected(ChecksumError::kInvalidPlan);
}

void AddHardwareWrite(TxChecksumMetadata *metadata, size_t offset,
                      const void *value) noexcept {
  HardwareWrite &write = metadata->writes[metadata->write_count++];
  write.offset = offset;
  std::memcpy(write.bytes.data(), value, write.bytes.size());
}

void AddZeroHardwareWrite(TxChecksumMetadata *metadata,
                          size_t offset) noexcept {
  const uint16_t zero = 0;
  AddHardwareWrite(metadata, offset, &zero);
}

std::expected<void, ChecksumError> AddDomainHardwareWrites(
    PacketRef packet, const BoundTxChecksumDomain &domain,
    const ChecksumLayout &layout, bool outer_tunnel,
    TxChecksumMetadata *metadata) noexcept {
  if (IsHardware(domain.network_backend)) {
    if (domain.plan.network != NetworkChecksum::kIpv4Header) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    AddZeroHardwareWrite(metadata, layout.network_checksum_offset);
  }
  if (IsHardware(domain.transport_backend)) {
    if (domain.plan.transport == TransportChecksum::kUdp && outer_tunnel) {
      // DPDK's outer-UDP offload consumes the checksum field as zero.
      AddZeroHardwareWrite(metadata, layout.transport_checksum_offset);
    } else if (domain.plan.transport == TransportChecksum::kUdp ||
               domain.plan.transport == TransportChecksum::kTcp) {
      const auto seed = PseudoHeaderSeed(packet, domain.plan);
      if (!seed) {
        return std::unexpected(seed.error());
      }
      AddHardwareWrite(metadata, layout.transport_checksum_offset, &*seed);
    } else {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
  }
  return {};
}

std::expected<TxChecksumMetadata, ChecksumError> BuildMetadata(
    PacketRef packet, const BoundTxFinalizationProfile &profile,
    const std::optional<ChecksumLayout> &outer_layout,
    const std::optional<ChecksumLayout> &inner_layout,
    const std::optional<OuterIpLayout> &outer_ip) noexcept {
  TxChecksumMetadata metadata;
  const bool tunneled =
      profile.encapsulation.kind != TxEncapsulationKind::kNone;

  if (!tunneled && profile.outer && HasHardware(*profile.outer)) {
    if (!outer_layout) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    const BoundTxChecksumDomain &domain = *profile.outer;
    const ChecksumLayout &layout = *outer_layout;
    metadata.flags |= domain.plan.ip_version == IpVersion::kIpv4
                          ? RTE_MBUF_F_TX_IPV4
                          : RTE_MBUF_F_TX_IPV6;
    if (IsHardware(domain.network_backend)) {
      metadata.flags |= RTE_MBUF_F_TX_IP_CKSUM;
    }
    if (IsHardware(domain.transport_backend)) {
      metadata.flags |= domain.plan.transport == TransportChecksum::kUdp
                            ? RTE_MBUF_F_TX_UDP_CKSUM
                            : RTE_MBUF_F_TX_TCP_CKSUM;
    }
    const auto l2_len =
        CheckedLength(domain.plan.network_offset, RTE_MBUF_L2_LEN_BITS);
    const auto l3_len =
        CheckedLength(layout.network_header_length, RTE_MBUF_L3_LEN_BITS);
    const auto l4_len = CheckedLength(
        IsHardware(domain.transport_backend)
            ? layout.transport_header_length
            : 0,
        RTE_MBUF_L4_LEN_BITS);
    if (!l2_len || !l3_len || !l4_len) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
    metadata.l2_len = *l2_len;
    metadata.l3_len = *l3_len;
    metadata.l4_len = *l4_len;
    metadata.set_main_lengths = true;
    const auto writes =
        AddDomainHardwareWrites(packet, domain, layout, false, &metadata);
    if (!writes) {
      return std::unexpected(writes.error());
    }
    return metadata;
  }

  if (!tunneled) {
    return metadata;
  }

  const bool outer_hardware = profile.outer && HasHardware(*profile.outer);
  const bool inner_hardware = profile.inner && HasHardware(*profile.inner);
  if (!outer_ip) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }

  if (outer_hardware || inner_hardware) {
    const auto outer_l2_len = CheckedLength(
        profile.encapsulation.outer_network_offset, RTE_MBUF_OUTL2_LEN_BITS);
    const auto outer_l3_len =
        CheckedLength(outer_ip->header_length, RTE_MBUF_OUTL3_LEN_BITS);
    if (!outer_l2_len || !outer_l3_len) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
    metadata.flags |= outer_ip->version == IpVersion::kIpv4
                          ? RTE_MBUF_F_TX_OUTER_IPV4
                          : RTE_MBUF_F_TX_OUTER_IPV6;
    metadata.outer_l2_len = *outer_l2_len;
    metadata.outer_l3_len = *outer_l3_len;
    metadata.set_outer_lengths = true;
  }

  if (outer_hardware) {
    if (!outer_layout) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    const BoundTxChecksumDomain &domain = *profile.outer;
    if (IsHardware(domain.network_backend)) {
      metadata.flags |= RTE_MBUF_F_TX_OUTER_IP_CKSUM;
    }
    if (IsHardware(domain.transport_backend)) {
      if (domain.plan.transport != TransportChecksum::kUdp) {
        return std::unexpected(ChecksumError::kInvalidPlan);
      }
      metadata.flags |= RTE_MBUF_F_TX_OUTER_UDP_CKSUM;
    }
    const auto writes =
        AddDomainHardwareWrites(packet, domain, *outer_layout, true, &metadata);
    if (!writes) {
      return std::unexpected(writes.error());
    }
  }

  if (inner_hardware) {
    if (!inner_layout) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    const BoundTxChecksumDomain &domain = *profile.inner;
    metadata.flags |= domain.plan.ip_version == IpVersion::kIpv4
                          ? RTE_MBUF_F_TX_IPV4
                          : RTE_MBUF_F_TX_IPV6;
    if (IsHardware(domain.network_backend)) {
      metadata.flags |= RTE_MBUF_F_TX_IP_CKSUM;
    }
    if (IsHardware(domain.transport_backend)) {
      metadata.flags |= domain.plan.transport == TransportChecksum::kUdp
                            ? RTE_MBUF_F_TX_UDP_CKSUM
                            : RTE_MBUF_F_TX_TCP_CKSUM;
    }
    metadata.flags |= profile.encapsulation.kind == TxEncapsulationKind::kIp
                           ? RTE_MBUF_F_TX_TUNNEL_IP
                           : RTE_MBUF_F_TX_TUNNEL_UDP;

    const size_t inner_l2_start =
        profile.encapsulation.outer_network_offset + outer_ip->header_length;
    if (domain.plan.network_offset < inner_l2_start) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    const size_t inner_l2_length = domain.plan.network_offset - inner_l2_start;
    const auto l2_len =
        CheckedLength(inner_l2_length, RTE_MBUF_L2_LEN_BITS);
    const auto l3_len = CheckedLength(inner_layout->network_header_length,
                                      RTE_MBUF_L3_LEN_BITS);
    const auto l4_len = CheckedLength(
        IsHardware(domain.transport_backend)
            ? inner_layout->transport_header_length
            : 0,
        RTE_MBUF_L4_LEN_BITS);
    if (!l2_len || !l3_len || !l4_len) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
    metadata.l2_len = *l2_len;
    metadata.l3_len = *l3_len;
    metadata.l4_len = *l4_len;
    metadata.set_main_lengths = true;
    const auto writes =
        AddDomainHardwareWrites(packet, domain, *inner_layout, false, &metadata);
    if (!writes) {
      return std::unexpected(writes.error());
    }
  }
  return metadata;
}

std::expected<void, ChecksumError> ValidateHardwareLayout(
    PacketHandle packet, const BoundTxFinalizationProfile &profile,
    const std::optional<ChecksumLayout> &outer_layout,
    const std::optional<ChecksumLayout> &inner_layout,
    const std::optional<OuterIpLayout> &outer_ip) noexcept {
  const bool tunneled =
      profile.encapsulation.kind != TxEncapsulationKind::kNone;
  const bool outer_hardware = profile.outer && HasHardware(*profile.outer);
  const bool inner_hardware = profile.inner && HasHardware(*profile.inner);
  if (!outer_hardware && !inner_hardware) {
    return {};
  }

  if (packet->nb_segs > 1 && !profile.multi_segment_tx) {
    return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
  }

  if (tunneled && (outer_hardware || inner_hardware)) {
    if (!outer_ip ||
        !FitsHeadSegment(packet, profile.encapsulation.outer_network_offset,
                         outer_ip->header_length)) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
  }

  if (outer_hardware) {
    const BoundTxChecksumDomain &domain = *profile.outer;
    if (!outer_layout ||
        !FitsHeadSegment(packet, domain.plan.network_offset,
                         outer_layout->network_header_length)) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
    if (IsHardware(domain.transport_backend) &&
        !FitsHeadSegment(packet, domain.plan.transport_offset,
                         outer_layout->transport_header_length)) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
  }

  if (inner_hardware) {
    const BoundTxChecksumDomain &domain = *profile.inner;
    if (!inner_layout ||
        !FitsHeadSegment(packet, domain.plan.network_offset,
                         inner_layout->network_header_length)) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
    if (IsHardware(domain.transport_backend) &&
        !FitsHeadSegment(packet, domain.plan.transport_offset,
                         inner_layout->transport_header_length)) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
    const size_t inner_header_end =
        domain.plan.network_offset + inner_layout->network_header_length +
        (IsHardware(domain.transport_backend)
             ? inner_layout->transport_header_length
             : 0);
    if (!FitsHeadSegment(packet, 0, inner_header_end)) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
  }
  return {};
}

std::expected<void, ChecksumError> ApplyBoundSoftwareChecksums(
    PacketHandle &packet, const BoundTxChecksumDomain &domain,
    bool include_hardware_components) noexcept {
  ChecksumPlan plan = domain.plan;
  const bool apply_network =
      IsSoftware(domain.network_backend) ||
      (include_hardware_components &&
       IsHardware(domain.network_backend));
  const bool apply_transport =
      IsSoftware(domain.transport_backend) ||
      (include_hardware_components &&
       IsHardware(domain.transport_backend));
  if (!apply_network) {
    plan.network = NetworkChecksum::kNone;
  }
  if (!apply_transport) {
    plan.transport = TransportChecksum::kNone;
  }
  if (!apply_network && !apply_transport) {
    return {};
  }
  return ApplySoftwareChecksums(packet, plan);
}

std::expected<void, ChecksumError> MakeHardwareWritesWritable(
    PacketHandle &packet, const TxChecksumMetadata &metadata) noexcept {
  if (metadata.write_count == 0 ||
      PayloadWriteabilityOf(PacketRef(packet)) == PayloadWriteability::kWritable) {
    return {};
  }
  const auto writable = detail::EnsureWritablePreservingTopology(packet);
  if (!writable) {
    if (writable.error() == ReshapeError::kAllocationFailed) {
      return std::unexpected(ChecksumError::kAllocationFailed);
    }
    if (writable.error() == ReshapeError::kInsufficientContiguousCapacity) {
      return std::unexpected(ChecksumError::kInsufficientWritableCapacity);
    }
    return std::unexpected(ChecksumError::kMalformedChain);
  }
  return {};
}

void CommitMetadataAndSeeds(PacketHandle packet,
                            const TxChecksumMetadata &metadata) noexcept {
  for (size_t index = 0; index < metadata.write_count; index++) {
    const HardwareWrite &write = metadata.writes[index];
    auto *destination = PacketRef(packet).head_data<std::byte *>(
        static_cast<uint16_t>(write.offset));
    std::memcpy(destination, write.bytes.data(), write.bytes.size());
  }
  if (metadata.set_main_lengths) {
    packet->l2_len = metadata.l2_len;
    packet->l3_len = metadata.l3_len;
    packet->l4_len = metadata.l4_len;
  }
  if (metadata.set_outer_lengths) {
    packet->outer_l2_len = metadata.outer_l2_len;
    packet->outer_l3_len = metadata.outer_l3_len;
  }
  packet->ol_flags = (packet->ol_flags & ~kTxChecksumFlags) | metadata.flags;
}

}  // namespace

std::expected<BoundTxFinalizationProfile, ChecksumError>
BindTxFinalizationProfile(const TxFinalizationProfile &profile,
                          const TxChecksumCapabilities &capabilities) noexcept {
  const bool tunneled =
      profile.encapsulation.kind != TxEncapsulationKind::kNone;
  if (profile.encapsulation.kind != TxEncapsulationKind::kNone &&
      profile.encapsulation.kind != TxEncapsulationKind::kIp &&
      profile.encapsulation.kind != TxEncapsulationKind::kUdp) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  if (!tunneled &&
      (profile.encapsulation.outer_network_offset != 0 ||
       profile.encapsulation.outer_ip_version != IpVersion::kIpv4)) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  if (tunneled && !IsValidIpVersion(profile.encapsulation.outer_ip_version)) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  if (!profile.outer && !profile.inner) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  if (profile.inner && !tunneled) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }

  auto validate_domain = [](const ChecksumPlan &plan) {
    return IsValidChecksumPlan(plan);
  };
  if (profile.outer && !validate_domain(*profile.outer)) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  if (profile.inner && !validate_domain(*profile.inner)) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  if (tunneled) {
    if (profile.outer &&
        (profile.outer->network_offset !=
             profile.encapsulation.outer_network_offset ||
         profile.outer->ip_version != profile.encapsulation.outer_ip_version)) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    if (profile.outer && profile.encapsulation.kind == TxEncapsulationKind::kIp &&
        profile.outer->transport != TransportChecksum::kNone) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    if (profile.outer && profile.encapsulation.kind == TxEncapsulationKind::kUdp &&
        profile.outer->transport != TransportChecksum::kNone &&
        profile.outer->transport != TransportChecksum::kUdp) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
    if (profile.inner &&
        profile.inner->network_offset <=
            profile.encapsulation.outer_network_offset) {
      return std::unexpected(ChecksumError::kInvalidPlan);
    }
  }

  const bool generic_tunnel_supported =
      !tunneled ||
      (profile.encapsulation.kind == TxEncapsulationKind::kIp
           ? capabilities.ip_tunnel
           : capabilities.udp_tunnel);
  auto bind_domain = [&](const ChecksumPlan &plan, bool is_tunneled_outer,
                         bool is_inner) {
    BoundTxChecksumDomain bound;
    bound.plan = plan;
    if (plan.network == NetworkChecksum::kIpv4Header) {
      const bool supported =
          is_tunneled_outer
              ? capabilities.outer_ipv4_header
              : capabilities.ipv4_header &&
                    (!is_inner || generic_tunnel_supported);
      bound.network_backend = supported ? TxChecksumBackend::kHardware
                                         : TxChecksumBackend::kSoftware;
    }
    if (plan.transport != TransportChecksum::kNone) {
      bool supported = false;
      if (is_tunneled_outer) {
        supported = plan.transport == TransportChecksum::kUdp &&
                    capabilities.outer_udp;
      } else {
        supported = plan.transport == TransportChecksum::kUdp
                        ? capabilities.udp
                        : capabilities.tcp;
        if (is_inner) {
          supported = supported && generic_tunnel_supported;
        }
      }
      bound.transport_backend = supported ? TxChecksumBackend::kHardware
                                           : TxChecksumBackend::kSoftware;
    }
    return bound;
  };

  BoundTxFinalizationProfile bound;
  bound.encapsulation = profile.encapsulation;
  bound.multi_segment_tx = capabilities.multi_segment_tx;
  if (profile.outer) {
    bound.outer = bind_domain(*profile.outer, tunneled, false);
  }
  if (profile.inner) {
    bound.inner = bind_domain(*profile.inner, false, true);
  }
  return bound;
}

std::expected<void, ChecksumError> FinalizeTxPacket(
    PacketHandle &packet, const BoundTxFinalizationProfile &profile) noexcept {
  if (!profile.enabled()) {
    return {};
  }
  if (packet == nullptr) {
    return std::unexpected(ChecksumError::kNullPacket);
  }

  std::optional<ChecksumLayout> outer_layout;
  std::optional<ChecksumLayout> inner_layout;
  if (profile.outer) {
    const auto layout = InspectChecksumPlan(PacketRef(packet), profile.outer->plan);
    if (!layout) {
      return std::unexpected(layout.error());
    }
    outer_layout = *layout;
  }
  if (profile.inner) {
    const auto layout = InspectChecksumPlan(PacketRef(packet), profile.inner->plan);
    if (!layout) {
      return std::unexpected(layout.error());
    }
    inner_layout = *layout;
  }

  std::optional<OuterIpLayout> outer_ip;
  if (profile.encapsulation.kind != TxEncapsulationKind::kNone) {
    const auto layout =
        ValidateEncapsulation(PacketRef(packet), profile, inner_layout);
    if (!layout) {
      return std::unexpected(layout.error());
    }
    outer_ip = *layout;
  } else if (profile.inner) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }

  PacketHandle segment = packet;
  for (uint16_t index = 0; index < packet->nb_segs; index++) {
    if (segment == nullptr || rte_mbuf_refcnt_read(segment) != 1) {
      return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
    }
    segment = segment->next;
  }
  if (segment != nullptr) {
    return std::unexpected(ChecksumError::kMalformedChain);
  }
  const auto hardware_layout = ValidateHardwareLayout(
      packet, profile, outer_layout, inner_layout, outer_ip);
  if (!hardware_layout) {
    return std::unexpected(hardware_layout.error());
  }

  auto metadata = BuildMetadata(PacketRef(packet), profile, outer_layout,
                                inner_layout, outer_ip);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  if (metadata->write_count > 0) {
    for (size_t index = 0; index < metadata->write_count; index++) {
      const size_t offset = metadata->writes[index].offset;
      if (offset > UINT16_MAX || !FitsHeadSegment(packet, offset,
                                                  sizeof(uint16_t))) {
        return std::unexpected(ChecksumError::kUnsupportedOffloadLayout);
      }
    }
  }

  const bool software_outer_udp_depends_on_inner_hw =
      profile.encapsulation.kind != TxEncapsulationKind::kNone &&
      profile.outer && profile.inner &&
      profile.outer->plan.transport == TransportChecksum::kUdp &&
      IsSoftware(profile.outer->transport_backend) &&
      HasHardware(*profile.inner);
  if (profile.inner) {
    const auto software = ApplyBoundSoftwareChecksums(
        packet, *profile.inner, software_outer_udp_depends_on_inner_hw);
    if (!software) {
      return std::unexpected(software.error());
    }
  }
  if (profile.outer) {
    const auto software =
        ApplyBoundSoftwareChecksums(packet, *profile.outer, false);
    if (!software) {
      return std::unexpected(software.error());
    }
  }

  const auto writable = MakeHardwareWritesWritable(packet, *metadata);
  if (!writable) {
    return std::unexpected(writable.error());
  }
  CommitMetadataAndSeeds(packet, *metadata);
  return {};
}

TxPacketBatchFinalizeResult FinalizeTxPacketBatch(
    PacketBatch &batch, const BoundTxFinalizationProfile &profile) noexcept {
  TxPacketBatchFinalizeResult result;
  if (!profile.enabled()) {
    return result;
  }

  int prepared = 0;
  const int original_count = batch.cnt();
  for (int index = 0; index < original_count; index++) {
    PacketHandle packet = batch.handles()[index];
    const auto finalized = FinalizeTxPacket(packet, profile);
    if (!finalized) {
      PacketFree(packet);
      result.rejected++;
      continue;
    }
    batch.handles()[prepared++] = packet;
  }
  batch.set_cnt(prepared);
  return result;
}

}  // namespace bess::packet
