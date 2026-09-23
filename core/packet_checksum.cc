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

#include "packet_checksum.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>

#include <rte_ip.h>

#include "packet_cursor.h"
#include "packet_mutation.h"
#include "packet_reshape.h"
#include "utils/ip.h"
#include "utils/tcp.h"
#include "utils/udp.h"

namespace bess::packet {
namespace {

constexpr size_t kChecksumFieldSize = sizeof(utils::be16_t);
constexpr size_t kIpv4ChecksumOffset = offsetof(utils::Ipv4, checksum);
constexpr size_t kUdpChecksumOffset = offsetof(utils::Udp, checksum);
constexpr size_t kTcpChecksumOffset = offsetof(utils::Tcp, checksum);

static_assert(kChecksumFieldSize == sizeof(uint16_t));

struct ChainInfo {
  uint16_t segment_count;
  uint32_t packet_length;
};

std::expected<ChainInfo, ChecksumError> ValidateChain(
    PacketHandle packet) noexcept {
  if (packet == nullptr) {
    return std::unexpected(ChecksumError::kNullPacket);
  }
  if (packet->nb_segs == 0) {
    return std::unexpected(ChecksumError::kMalformedChain);
  }

  uint64_t logical_length = 0;
  PacketHandle segment = packet;
  for (uint16_t index = 0; index < packet->nb_segs; index++) {
    if (segment == nullptr || segment->pool == nullptr ||
        segment->buf_addr == nullptr || segment->data_off > segment->buf_len ||
        segment->data_len > segment->buf_len - segment->data_off) {
      return std::unexpected(ChecksumError::kMalformedChain);
    }
    logical_length += segment->data_len;
    if (logical_length > UINT32_MAX) {
      return std::unexpected(ChecksumError::kMalformedChain);
    }
    segment = segment->next;
  }

  if (segment != nullptr || logical_length != packet->pkt_len) {
    return std::unexpected(ChecksumError::kMalformedChain);
  }
  return ChainInfo{packet->nb_segs, packet->pkt_len};
}

bool RangeWithinPacket(size_t packet_length, size_t offset,
                       size_t length) noexcept {
  return offset <= packet_length && length <= packet_length - offset;
}

template <typename T>
std::optional<T> ReadHeaderAt(PacketRef packet, size_t offset) noexcept {
  if (!RangeWithinPacket(packet.total_len(), offset, sizeof(T))) {
    return std::nullopt;
  }
  PacketCursor cursor(packet);
  if (!cursor.Skip(offset)) {
    return std::nullopt;
  }
  return cursor.Read<T>();
}

class ChecksumAccumulator {
 public:
  void Add(std::span<const std::byte> bytes) noexcept {
    size_t offset = 0;
    if (has_pending_byte_ && !bytes.empty()) {
      sum_ += (static_cast<uint16_t>(pending_byte_) << 8) |
              std::to_integer<uint8_t>(bytes[0]);
      has_pending_byte_ = false;
      offset = 1;
    }

    while (offset + 1 < bytes.size()) {
      sum_ += (static_cast<uint16_t>(std::to_integer<uint8_t>(bytes[offset]))
               << 8) |
              std::to_integer<uint8_t>(bytes[offset + 1]);
      offset += 2;
    }
    if (offset < bytes.size()) {
      pending_byte_ = std::to_integer<uint8_t>(bytes[offset]);
      has_pending_byte_ = true;
    }
  }

  void AddZeroBytes(size_t length) noexcept {
    constexpr std::array<std::byte, 2> zeros{};
    Add(std::span<const std::byte>(zeros).first(length));
  }

  uint16_t Finish() const noexcept {
    uint64_t sum = sum_;
    if (has_pending_byte_) {
      sum += static_cast<uint16_t>(pending_byte_) << 8;
    }
    while ((sum >> 16) != 0) {
      sum = (sum & 0xffff) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
  }

 private:
  uint64_t sum_ = 0;
  uint8_t pending_byte_ = 0;
  bool has_pending_byte_ = false;
};

bool AddPacketRange(PacketRef packet, size_t offset, size_t length,
                    ChecksumAccumulator &sum) noexcept {
  if (!RangeWithinPacket(packet.total_len(), offset, length)) {
    return false;
  }
  if (length == 0) {
    return true;
  }

  PacketHandle segment = packet.handle();
  size_t segment_offset = offset;
  while (segment != nullptr && segment_offset >= segment->data_len) {
    segment_offset -= segment->data_len;
    segment = segment->next;
  }

  size_t remaining = length;
  while (remaining != 0 && segment != nullptr) {
    if (segment_offset >= segment->data_len) {
      segment_offset -= segment->data_len;
      segment = segment->next;
      continue;
    }

    const size_t chunk = std::min(
        remaining, static_cast<size_t>(segment->data_len) - segment_offset);
    const auto *data = PacketRef(segment).head_data<const std::byte *>(
        static_cast<uint16_t>(segment_offset));
    sum.Add(std::span<const std::byte>(data, chunk));
    remaining -= chunk;
    segment_offset = 0;
    segment = segment->next;
  }
  return remaining == 0;
}

bool AddPacketRangeWithZeroField(PacketRef packet, size_t range_offset,
                                 size_t range_length, size_t zero_offset,
                                 ChecksumAccumulator &sum) noexcept {
  if (zero_offset < range_offset || zero_offset - range_offset > range_length ||
      kChecksumFieldSize > range_length - (zero_offset - range_offset)) {
    return false;
  }
  const size_t before = zero_offset - range_offset;
  if (!AddPacketRange(packet, range_offset, before, sum)) {
    return false;
  }
  sum.AddZeroBytes(kChecksumFieldSize);
  const size_t after_offset = zero_offset + kChecksumFieldSize;
  const size_t after_length = range_length - before - kChecksumFieldSize;
  return AddPacketRange(packet, after_offset, after_length, sum);
}

void AddIpv4PseudoHeader(ChecksumAccumulator &sum, const utils::Ipv4 &ip,
                         uint8_t protocol, uint16_t transport_length) noexcept {
  std::array<std::byte, 12> pseudo_header{};
  std::memcpy(pseudo_header.data(), &ip.src, sizeof(ip.src));
  std::memcpy(pseudo_header.data() + 4, &ip.dst, sizeof(ip.dst));
  pseudo_header[9] = static_cast<std::byte>(protocol);
  pseudo_header[10] = static_cast<std::byte>(transport_length >> 8);
  pseudo_header[11] = static_cast<std::byte>(transport_length & 0xff);
  sum.Add(pseudo_header);
}

void AddIpv6PseudoHeader(ChecksumAccumulator &sum, const rte_ipv6_hdr &ip,
                         uint8_t protocol, uint16_t transport_length) noexcept {
  std::array<std::byte, 40> pseudo_header{};
  std::memcpy(pseudo_header.data(), ip.src_addr.a, sizeof(ip.src_addr.a));
  std::memcpy(pseudo_header.data() + 16, ip.dst_addr.a, sizeof(ip.dst_addr.a));
  pseudo_header[32] = static_cast<std::byte>((transport_length >> 24) & 0xff);
  pseudo_header[33] = static_cast<std::byte>((transport_length >> 16) & 0xff);
  pseudo_header[34] = static_cast<std::byte>((transport_length >> 8) & 0xff);
  pseudo_header[35] = static_cast<std::byte>(transport_length & 0xff);
  pseudo_header[39] = static_cast<std::byte>(protocol);
  sum.Add(pseudo_header);
}

bool IsIpv6ExtensionHeader(uint8_t protocol) noexcept {
  switch (protocol) {
    case 0:    // Hop-by-Hop Options
    case 43:   // Routing
    case 44:   // Fragment
    case 50:   // Encapsulating Security Payload
    case 51:   // Authentication Header
    case 60:   // Destination Options
    case 135:  // Mobility
    case 139:  // HIP
    case 140:  // Shim6
      return true;
    default:
      return false;
  }
}

bool IsValidPlan(const ChecksumPlan &plan) noexcept {
  switch (plan.ip_version) {
    case IpVersion::kIpv4:
    case IpVersion::kIpv6:
      break;
    default:
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
    case TransportChecksum::kUdp:
    case TransportChecksum::kTcp:
      break;
    default:
      return false;
  }
  return true;
}

std::expected<ChecksumValues, ChecksumError> ComputeIpv4Checksums(
    PacketRef packet, const ChecksumPlan &plan) noexcept {
  ChecksumValues values;
  if (plan.network == NetworkChecksum::kNone &&
      plan.transport == TransportChecksum::kNone) {
    return values;
  }

  const size_t packet_length = packet.total_len();
  const auto ip_header = ReadHeaderAt<utils::Ipv4>(packet, plan.network_offset);
  if (!ip_header) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }
  const utils::Ipv4 &ip = *ip_header;
  if (ip.version != 4 || ip.header_length < 5) {
    return std::unexpected(ChecksumError::kInvalidIpv4Header);
  }

  const size_t ip_header_length = static_cast<size_t>(ip.header_length) * 4;
  const size_t ip_total_length = ip.length.value();
  if (ip_header_length > ip_total_length) {
    return std::unexpected(ChecksumError::kInvalidIpv4Header);
  }
  if (!RangeWithinPacket(packet_length, plan.network_offset, ip_total_length)) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }

  if (plan.network == NetworkChecksum::kIpv4Header) {
    ChecksumAccumulator sum;
    const size_t field_offset = plan.network_offset + kIpv4ChecksumOffset;
    if (!AddPacketRangeWithZeroField(packet, plan.network_offset,
                                     ip_header_length, field_offset, sum)) {
      return std::unexpected(ChecksumError::kMalformedChain);
    }
    values.network.emplace(sum.Finish());
  }

  if (plan.transport == TransportChecksum::kNone) {
    return values;
  }

  const uint8_t expected_protocol = plan.transport == TransportChecksum::kUdp
                                        ? utils::Ipv4::kUdp
                                        : utils::Ipv4::kTcp;
  if (ip.protocol != expected_protocol) {
    return std::unexpected(ChecksumError::kProtocolMismatch);
  }
  if ((ip.fragment_offset.value() &
       (utils::Ipv4::kMF | static_cast<uint16_t>(0x1fff))) != 0) {
    return std::unexpected(ChecksumError::kFragmentedDatagram);
  }

  if (ip_header_length > packet_length - plan.network_offset) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }
  const size_t expected_transport_offset =
      plan.network_offset + ip_header_length;
  if (plan.transport_offset != expected_transport_offset) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  const size_t transport_length = ip_total_length - ip_header_length;
  if (transport_length > UINT16_MAX) {
    return std::unexpected(ChecksumError::kInvalidIpv4Header);
  }

  size_t checksum_field_offset = 0;
  if (plan.transport == TransportChecksum::kUdp) {
    if (transport_length < sizeof(utils::Udp)) {
      return std::unexpected(ChecksumError::kInvalidTransportHeader);
    }
    const auto udp = ReadHeaderAt<utils::Udp>(packet, plan.transport_offset);
    if (!udp) {
      return std::unexpected(ChecksumError::kLengthOutOfRange);
    }
    if (udp->length.value() != transport_length) {
      return std::unexpected(ChecksumError::kInvalidTransportHeader);
    }
    checksum_field_offset = plan.transport_offset + kUdpChecksumOffset;
  } else {
    if (transport_length < sizeof(utils::Tcp)) {
      return std::unexpected(ChecksumError::kInvalidTransportHeader);
    }
    const auto tcp = ReadHeaderAt<utils::Tcp>(packet, plan.transport_offset);
    if (!tcp) {
      return std::unexpected(ChecksumError::kLengthOutOfRange);
    }
    const size_t tcp_header_length = static_cast<size_t>(tcp->offset) * 4;
    if (tcp_header_length < sizeof(utils::Tcp) ||
        tcp_header_length > transport_length) {
      return std::unexpected(ChecksumError::kInvalidTransportHeader);
    }
    checksum_field_offset = plan.transport_offset + kTcpChecksumOffset;
  }

  ChecksumAccumulator sum;
  AddIpv4PseudoHeader(sum, ip, expected_protocol,
                      static_cast<uint16_t>(transport_length));
  if (!AddPacketRangeWithZeroField(packet, plan.transport_offset,
                                   transport_length, checksum_field_offset,
                                   sum)) {
    return std::unexpected(ChecksumError::kMalformedChain);
  }
  uint16_t checksum = sum.Finish();
  if (plan.transport == TransportChecksum::kUdp && checksum == 0) {
    checksum = 0xffff;
  }
  values.transport.emplace(checksum);
  return values;
}

std::expected<ChecksumValues, ChecksumError> ComputeIpv6Checksums(
    PacketRef packet, const ChecksumPlan &plan) noexcept {
  ChecksumValues values;
  if (plan.network != NetworkChecksum::kNone) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  if (plan.transport == TransportChecksum::kNone) {
    return values;
  }

  const auto ip_header =
      ReadHeaderAt<rte_ipv6_hdr>(packet, plan.network_offset);
  if (!ip_header) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }
  const rte_ipv6_hdr &ip = *ip_header;
  if ((rte_be_to_cpu_32(ip.vtc_flow) >> 28) != 6) {
    return std::unexpected(ChecksumError::kInvalidIpv6Header);
  }
  const size_t payload_length = rte_be_to_cpu_16(ip.payload_len);
  if (payload_length == 0) {
    return std::unexpected(ChecksumError::kUnsupportedJumbogram);
  }
  if (!RangeWithinPacket(packet.total_len(), plan.network_offset,
                         sizeof(rte_ipv6_hdr) + payload_length)) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }

  if (ip.proto == 44) {
    return std::unexpected(ChecksumError::kFragmentedDatagram);
  }
  if (IsIpv6ExtensionHeader(ip.proto)) {
    return std::unexpected(ChecksumError::kUnsupportedExtensionHeader);
  }
  const uint8_t expected_protocol = plan.transport == TransportChecksum::kUdp
                                        ? utils::Ipv4::kUdp
                                        : utils::Ipv4::kTcp;
  if (ip.proto != expected_protocol) {
    return std::unexpected(ChecksumError::kProtocolMismatch);
  }

  const size_t expected_transport_offset =
      plan.network_offset + sizeof(rte_ipv6_hdr);
  if (plan.transport_offset != expected_transport_offset) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }
  const size_t transport_length = payload_length;
  size_t checksum_field_offset = 0;
  if (plan.transport == TransportChecksum::kUdp) {
    if (transport_length < sizeof(utils::Udp)) {
      return std::unexpected(ChecksumError::kInvalidTransportHeader);
    }
    const auto udp = ReadHeaderAt<utils::Udp>(packet, plan.transport_offset);
    if (!udp) {
      return std::unexpected(ChecksumError::kLengthOutOfRange);
    }
    if (udp->length.value() != transport_length) {
      return std::unexpected(ChecksumError::kInvalidTransportHeader);
    }
    checksum_field_offset = plan.transport_offset + kUdpChecksumOffset;
  } else {
    if (transport_length < sizeof(utils::Tcp)) {
      return std::unexpected(ChecksumError::kInvalidTransportHeader);
    }
    const auto tcp = ReadHeaderAt<utils::Tcp>(packet, plan.transport_offset);
    if (!tcp) {
      return std::unexpected(ChecksumError::kLengthOutOfRange);
    }
    const size_t tcp_header_length = static_cast<size_t>(tcp->offset) * 4;
    if (tcp_header_length < sizeof(utils::Tcp) ||
        tcp_header_length > transport_length) {
      return std::unexpected(ChecksumError::kInvalidTransportHeader);
    }
    checksum_field_offset = plan.transport_offset + kTcpChecksumOffset;
  }

  ChecksumAccumulator sum;
  AddIpv6PseudoHeader(sum, ip, expected_protocol,
                      static_cast<uint16_t>(transport_length));
  if (!AddPacketRangeWithZeroField(packet, plan.transport_offset,
                                   transport_length, checksum_field_offset,
                                   sum)) {
    return std::unexpected(ChecksumError::kMalformedChain);
  }
  uint16_t checksum = sum.Finish();
  if (plan.transport == TransportChecksum::kUdp && checksum == 0) {
    checksum = 0xffff;
  }
  values.transport.emplace(checksum);
  return values;
}

struct PendingWrite {
  size_t offset = 0;
  std::array<std::byte, kChecksumFieldSize> bytes{};
};

void AddPendingWrite(std::array<PendingWrite, 2> &writes, size_t &count,
                     size_t offset, const utils::be16_t &value) noexcept {
  PendingWrite &write = writes[count++];
  write.offset = offset;
  std::memcpy(write.bytes.data(), &value, write.bytes.size());
}

std::expected<bool, ChecksumError> IsRangeWritable(PacketHandle packet,
                                                   size_t offset,
                                                   size_t length) noexcept {
  if (packet == nullptr) {
    return std::unexpected(ChecksumError::kNullPacket);
  }
  if (!RangeWithinPacket(packet->pkt_len, offset, length)) {
    return std::unexpected(ChecksumError::kLengthOutOfRange);
  }

  bool writable = true;
  size_t remaining = length;
  PacketHandle segment = packet;
  for (uint16_t index = 0; index < packet->nb_segs && remaining != 0; index++) {
    if (segment == nullptr) {
      return std::unexpected(ChecksumError::kMalformedChain);
    }
    if (offset >= segment->data_len) {
      offset -= segment->data_len;
      segment = segment->next;
      continue;
    }

    const size_t chunk =
        std::min(remaining, static_cast<size_t>(segment->data_len) - offset);
    if (PayloadWriteabilityOf(PacketRef(segment)) !=
        PayloadWriteability::kWritable) {
      writable = false;
    }
    remaining -= chunk;
    offset = 0;
    segment = segment->next;
  }
  if (remaining != 0) {
    return std::unexpected(ChecksumError::kMalformedChain);
  }
  return writable;
}

void WriteBytesUnchecked(PacketHandle packet, size_t offset,
                         std::span<const std::byte> bytes) noexcept {
  PacketHandle segment = packet;
  while (!bytes.empty()) {
    while (segment != nullptr && offset >= segment->data_len) {
      offset -= segment->data_len;
      segment = segment->next;
    }

    const size_t chunk =
        std::min(bytes.size(), static_cast<size_t>(segment->data_len) - offset);
    auto *destination = PacketRef(segment).head_data<std::byte *>(
        static_cast<uint16_t>(offset));
    std::memcpy(destination, bytes.data(), chunk);
    bytes = bytes.subspan(chunk);
    offset = 0;
    segment = segment->next;
  }
}

}  // namespace

std::expected<ChecksumValues, ChecksumError> ComputeChecksums(
    PacketRef packet, const ChecksumPlan &plan) noexcept {
  const auto chain = ValidateChain(packet.handle());
  if (!chain) {
    return std::unexpected(chain.error());
  }
  if (!IsValidPlan(plan)) {
    return std::unexpected(ChecksumError::kInvalidPlan);
  }

  switch (plan.ip_version) {
    case IpVersion::kIpv4:
      return ComputeIpv4Checksums(packet, plan);
    case IpVersion::kIpv6:
      return ComputeIpv6Checksums(packet, plan);
  }
  return std::unexpected(ChecksumError::kInvalidPlan);
}

std::expected<void, ChecksumError> ApplySoftwareChecksums(
    PacketHandle &packet, const ChecksumPlan &plan) noexcept {
  const auto values = ComputeChecksums(PacketRef(packet), plan);
  if (!values) {
    return std::unexpected(values.error());
  }

  std::array<PendingWrite, 2> writes{};
  size_t write_count = 0;
  if (values->network) {
    AddPendingWrite(writes, write_count,
                    plan.network_offset + kIpv4ChecksumOffset,
                    *values->network);
  }
  if (values->transport) {
    const size_t checksum_offset = plan.transport == TransportChecksum::kUdp
                                       ? kUdpChecksumOffset
                                       : kTcpChecksumOffset;
    AddPendingWrite(writes, write_count,
                    plan.transport_offset + checksum_offset,
                    *values->transport);
  }
  if (write_count == 0) {
    return {};
  }

  bool needs_copy_on_write = false;
  for (size_t index = 0; index < write_count; index++) {
    const auto writable = IsRangeWritable(packet, writes[index].offset,
                                          writes[index].bytes.size());
    if (!writable) {
      return std::unexpected(writable.error());
    }
    needs_copy_on_write |= !*writable;
  }

  if (needs_copy_on_write) {
    const auto writable = EnsureWritable(packet);
    if (!writable) {
      if (writable.error() == ReshapeError::kAllocationFailed) {
        return std::unexpected(ChecksumError::kAllocationFailed);
      }
      if (writable.error() == ReshapeError::kInsufficientContiguousCapacity) {
        return std::unexpected(ChecksumError::kInsufficientWritableCapacity);
      }
      return std::unexpected(ChecksumError::kMalformedChain);
    }
  }

  for (size_t index = 0; index < write_count; index++) {
    WriteBytesUnchecked(packet, writes[index].offset, writes[index].bytes);
  }
  return {};
}

}  // namespace bess::packet
