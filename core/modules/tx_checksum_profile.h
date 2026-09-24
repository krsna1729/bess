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

#ifndef BESS_MODULES_TX_CHECKSUM_PROFILE_H_
#define BESS_MODULES_TX_CHECKSUM_PROFILE_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>

#include "../packet_tx_checksum.h"
#include "../pb/module_msg.pb.h"

namespace bess::modules {
namespace detail {

inline std::expected<packet::IpVersion, packet::ChecksumError> ParseIpVersion(
    pb::ChecksumIpVersion version) noexcept {
  switch (version) {
    case pb::CHECKSUM_IP_VERSION_IPV4:
      return packet::IpVersion::kIpv4;
    case pb::CHECKSUM_IP_VERSION_IPV6:
      return packet::IpVersion::kIpv6;
    case pb::CHECKSUM_IP_VERSION_UNSPECIFIED:
      break;
    default:
      break;
  }
  return std::unexpected(packet::ChecksumError::kInvalidPlan);
}

inline std::expected<packet::ChecksumPlan, packet::ChecksumError> ParseDomain(
    const pb::TxChecksumDomain &domain) noexcept {
  const auto version = ParseIpVersion(domain.ip_version());
  if (!version ||
      domain.network_offset() > std::numeric_limits<size_t>::max() ||
      domain.transport_offset() > std::numeric_limits<size_t>::max()) {
    return std::unexpected(packet::ChecksumError::kInvalidPlan);
  }

  packet::NetworkChecksum network;
  switch (domain.network()) {
    case pb::TX_CHECKSUM_NETWORK_NONE:
      network = packet::NetworkChecksum::kNone;
      break;
    case pb::TX_CHECKSUM_NETWORK_IPV4_HEADER:
      network = packet::NetworkChecksum::kIpv4Header;
      break;
    default:
      return std::unexpected(packet::ChecksumError::kInvalidPlan);
  }

  packet::TransportChecksum transport;
  switch (domain.transport()) {
    case pb::TX_CHECKSUM_TRANSPORT_NONE:
      transport = packet::TransportChecksum::kNone;
      break;
    case pb::TX_CHECKSUM_TRANSPORT_UDP:
      transport = packet::TransportChecksum::kUdp;
      break;
    case pb::TX_CHECKSUM_TRANSPORT_TCP:
      transport = packet::TransportChecksum::kTcp;
      break;
    default:
      return std::unexpected(packet::ChecksumError::kInvalidPlan);
  }

  return packet::ChecksumPlan{
      .network_offset = static_cast<size_t>(domain.network_offset()),
      .transport_offset = static_cast<size_t>(domain.transport_offset()),
      .ip_version = *version,
      .network = network,
      .transport = transport,
  };
}

}  // namespace detail

inline std::expected<packet::TxFinalizationProfile, packet::ChecksumError>
ParseTxChecksumProfile(const pb::TxChecksumProfile &arg) noexcept {
  packet::TxFinalizationProfile profile;
  if (arg.has_encapsulation()) {
    const pb::TxChecksumEncapsulation &encapsulation = arg.encapsulation();
    const auto version = detail::ParseIpVersion(encapsulation.outer_ip_version());
    if (!version ||
        encapsulation.outer_network_offset() >
            std::numeric_limits<size_t>::max()) {
      return std::unexpected(packet::ChecksumError::kInvalidPlan);
    }
    switch (encapsulation.kind()) {
      case pb::TX_ENCAPSULATION_IP:
        profile.encapsulation.encoding = packet::TxTunnelEncoding::kGenericIp;
        break;
      case pb::TX_ENCAPSULATION_UDP:
        profile.encapsulation.encoding = packet::TxTunnelEncoding::kGenericUdp;
        break;
      case pb::TX_ENCAPSULATION_GTP:
        profile.encapsulation.encoding = packet::TxTunnelEncoding::kGtp;
        break;
      case pb::TX_ENCAPSULATION_NONE:
        return std::unexpected(packet::ChecksumError::kInvalidPlan);
      default:
        return std::unexpected(packet::ChecksumError::kInvalidPlan);
    }
    profile.encapsulation.outer_ip_version = *version;
    profile.encapsulation.outer_network_offset =
        static_cast<size_t>(encapsulation.outer_network_offset());
  }
  if (arg.has_outer()) {
    auto domain = detail::ParseDomain(arg.outer());
    if (!domain) {
      return std::unexpected(domain.error());
    }
    profile.outer = *domain;
  }
  if (arg.has_inner()) {
    auto domain = detail::ParseDomain(arg.inner());
    if (!domain) {
      return std::unexpected(domain.error());
    }
    profile.inner = *domain;
  }
  return profile;
}

}  // namespace bess::modules

#endif  // BESS_MODULES_TX_CHECKSUM_PROFILE_H_
