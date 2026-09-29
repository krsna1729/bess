// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_PACKET_CHECKSUM_H_
#define BESS_PACKET_CHECKSUM_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "packet.h"
#include "utils/endian.h"

namespace bess::packet {

enum class IpVersion : uint8_t {
  kIpv4,
  kIpv6,
};

enum class NetworkChecksum : uint8_t {
  kNone,
  kIpv4Header,
};
enum class TransportChecksum : uint8_t {
  kNone,
  kUdp,
  kTcp,
};

struct ChecksumPlan {
  size_t network_offset = 0;
  size_t transport_offset = 0;
  IpVersion ip_version = IpVersion::kIpv4;
  NetworkChecksum network = NetworkChecksum::kNone;
  TransportChecksum transport = TransportChecksum::kNone;
};

struct ChecksumLayout {
  size_t network_header_length = 0;
  size_t network_length = 0;
  size_t transport_length = 0;
  size_t transport_header_length = 0;
  size_t network_checksum_offset = 0;
  size_t transport_checksum_offset = 0;
  uint8_t transport_protocol = 0;
};


struct TxChecksumCapabilities {
  bool ipv4_header = false;
  bool udp = false;  // IPv4 and IPv6 transport checksums.
  bool tcp = false;  // IPv4 and IPv6 transport checksums.
  bool outer_ipv4_header = false;
  bool outer_udp = false;
};


struct TxTunnelEncodingCapabilities {
  bool generic_ip = false;
  bool generic_udp = false;
  bool gtp = false;
};

// Effective capabilities available to an output queue after intersecting
// advertised offloads with the configuration actually accepted by the PMD.
struct TxOffloadCapabilities {
  TxChecksumCapabilities checksums;
  TxTunnelEncodingCapabilities tunnel_encodings;
  bool multi_segment_tx = false;
};

struct ChecksumValues {
  std::optional<utils::be16_t> network;
  std::optional<utils::be16_t> transport;
};

enum class ChecksumError : uint8_t {
  kNullPacket,
  kMalformedChain,
  kInvalidPlan,
  kLengthOutOfRange,
  kInvalidIpv4Header,
  kInvalidIpv6Header,
  kInvalidTransportHeader,
  kProtocolMismatch,
  kFragmentedDatagram,
  kUnsupportedExtensionHeader,
  kUnsupportedJumbogram,
  kAllocationFailed,
  kInsufficientWritableCapacity,
  kUnsupportedOffloadLayout,
};

// Computes requested checksums from the packet's logical bytes. This is
// read-only, traverses chains without linearizing, and uses IP-declared lengths
// rather than consuming unrelated trailing packet bytes. UDP results of zero
// are encoded as 0xffff. The caller may share payload backing.
std::expected<ChecksumValues, ChecksumError> ComputeChecksums(
    PacketRef packet, const ChecksumPlan &plan) noexcept;

// Validates a checksum plan and reports the parsed fixed header lengths and
// checksum-field offsets without computing checksums or mutating the packet.
std::expected<ChecksumLayout, ChecksumError> InspectChecksumPlan(
    PacketRef packet, const ChecksumPlan &plan) noexcept;


// Applies all requested checksums transactionally. The caller must exclusively
// own the descriptor chain; shared backing is copied only when a target
// checksum field lies in it. Split fields are written across segments without
// flattening the chain. An error leaves packet bytes, topology, and metadata
// unchanged.
std::expected<void, ChecksumError> ApplySoftwareChecksums(
    PacketHandle &packet, const ChecksumPlan &plan) noexcept;

}  // namespace bess::packet

#endif  // BESS_PACKET_CHECKSUM_H_
