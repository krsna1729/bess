// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_L3_PACKET_H_
#define BESS_ROUTE_L3_PACKET_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>

#include "utils/arp.h"
#include "utils/checksum.h"
#include "utils/ether.h"
#include "utils/icmp.h"
#include "utils/ip.h"

namespace bess::route {

using utils::be16_t;
using utils::be32_t;

// L3 packet mechanics for forwarding (M15, D-065): TTL and hop limit, the MTU
// check, ICMPv4 error construction and ARP. Mechanism, not policy: each
// helper reports what it found, and the application chooses -- forward,
// drop, punt, fragment (later) or emit ICMP. They work on raw header bytes,
// so they need no Module, packet type or gate; the caller ensures the bytes
// are writable and contiguous.

// -- TTL / hop limit -------------------------------------------------------------

enum class TtlResult : uint8_t {
  kForward,  // decremented; the header checksum is updated
  kExpired,  // TTL 0 or 1: nothing changed; the caller drops, or emits ICMP
             // Time Exceeded (BuildIcmpv4Error) before dropping
};

// Decrements an IPv4 TTL and patches the header checksum incrementally
// (RFC 1624), as a router forwarding the packet must (RFC 1812 5.3.1).
inline TtlResult DecrementTtl(utils::Ipv4 &ip) noexcept {
  if (ip.ttl <= 1) {
    return TtlResult::kExpired;
  }
  // TTL and protocol share one 16-bit header word; the checksum covers it as
  // stored, so the update uses the word as stored too.
  const auto *bytes = reinterpret_cast<const uint8_t *>(&ip);
  uint16_t before;
  std::memcpy(&before, bytes + 8, 2);
  ip.ttl--;
  uint16_t after;
  std::memcpy(&after, bytes + 8, 2);
  ip.checksum = utils::UpdateChecksum16(ip.checksum, before, after);
  return TtlResult::kForward;
}

// Decrements an IPv6 hop limit (byte 7 of the 40-byte header; IPv6 has no
// header checksum). kExpired for 0 or 1, nothing changed.
inline TtlResult DecrementHopLimit(std::span<uint8_t> ipv6_header) noexcept {
  if (ipv6_header.size() < 40 || ipv6_header[7] <= 1) {
    return TtlResult::kExpired;
  }
  ipv6_header[7]--;
  return TtlResult::kForward;
}

// -- MTU -----------------------------------------------------------------------

enum class MtuResult : uint8_t {
  kFits,
  kFragmentationNeeded,  // too big and DF set: drop, and ICMP 3/4 carries mtu
  kTooBigFragmentable,   // too big, DF clear: fragment, or drop
};

// Whether an IPv4 packet of total length ip.length fits an egress MTU.
inline MtuResult CheckMtu(const utils::Ipv4 &ip, uint16_t mtu) noexcept {
  if (ip.length.value() <= mtu) {
    return MtuResult::kFits;
  }
  return (ip.fragment_offset.value() & utils::Ipv4::kDF) != 0
             ? MtuResult::kFragmentationNeeded
             : MtuResult::kTooBigFragmentable;
}

// -- ICMPv4 errors ---------------------------------------------------------------

enum class IcmpType : uint8_t {
  kDestinationUnreachable = 3,
  kTimeExceeded = 11,
};
// Codes used with the types above (RFC 792, RFC 1191).
inline constexpr uint8_t kIcmpCodeNetUnreachable = 0;
inline constexpr uint8_t kIcmpCodeHostUnreachable = 1;
inline constexpr uint8_t kIcmpCodeFragmentationNeeded = 4;
inline constexpr uint8_t kIcmpCodeTtlExceeded = 0;

struct Icmpv4ErrorSpec {
  IcmpType type;
  uint8_t code;
  be32_t source;              // the router's address on the arrival interface
  uint16_t next_hop_mtu = 0;  // fragmentation needed only (RFC 1191)
  uint8_t ttl = 64;
};

enum class IcmpRefusal : uint8_t {
  kMalformed,         // not a complete IPv4 header (length, version, IHL)
  kOutputTooSmall,
  kIcmpError,         // never answer an ICMP error with one (RFC 1122 3.2.2)
  kNotUnicast,        // destination broadcast or multicast, or source not a
                      // unicast host (0/8, 127/8, broadcast, multicast)
  kNonInitialFragment,
};

// The size of the error message for `original` (IPv4 header + ICMP header +
// the original's IP header and up to 8 bytes after it, RFC 792/1812 4.3.2.3
// minimum).
inline size_t Icmpv4ErrorSize(std::span<const uint8_t> original) noexcept {
  if (original.size() < sizeof(utils::Ipv4)) {
    return 0;
  }
  const size_t ihl = static_cast<size_t>(original[0] & 0x0f) * 4;
  const size_t quoted = std::min(original.size(), ihl + 8);
  return sizeof(utils::Ipv4) + sizeof(utils::Icmp) + quoted;
}

namespace internal {
inline bool IsUnicast(uint32_t addr) noexcept {
  const uint32_t top = addr >> 28;
  return addr != 0 && addr != 0xffffffffu && top != 0xe /* 224/4 */ &&
         top != 0xf /* 240/4 */;
}
// A source an error may be sent to (RFC 1122 3.2.2): a unicast host, not
// 0/8 ("this network") or 127/8 (loopback).
inline bool IsHostSource(uint32_t addr) noexcept {
  const uint32_t first = addr >> 24;
  return IsUnicast(addr) && first != 0 && first != 127;
}
}  // namespace internal

// Writes an ICMPv4 error about `original` (an IPv4 packet starting at its
// header) into `out`: a 20-byte IPv4 header from spec.source to the
// original's source, the ICMP header, and the quote. Returns the bytes
// written, or why no error may be sent. Both checksums are computed.
//
// It refuses what the IP header shows RFC 1122 3.2.2 forbids. What it cannot
// see stays the caller's duty: a datagram that arrived as a link-layer
// broadcast or multicast, and one to a directed broadcast (a subnet's
// broadcast address) of an attached network, must not elicit an error.
inline std::expected<size_t, IcmpRefusal> BuildIcmpv4Error(
    std::span<const uint8_t> original, const Icmpv4ErrorSpec &spec,
    std::span<uint8_t> out) noexcept {
  if (original.size() < sizeof(utils::Ipv4) || (original[0] >> 4) != 4 ||
      (original[0] & 0x0f) < 5 || original.size() < (original[0] & 0x0fu) * 4u) {
    return std::unexpected(IcmpRefusal::kMalformed);
  }
  utils::Ipv4 orig;
  std::memcpy(&orig, original.data(), sizeof(orig));
  const size_t ihl = static_cast<size_t>(orig.header_length) * 4;
  if (!internal::IsUnicast(orig.dst.value()) || !internal::IsHostSource(orig.src.value())) {
    return std::unexpected(IcmpRefusal::kNotUnicast);
  }
  if ((orig.fragment_offset.value() & 0x1fff) != 0) {
    return std::unexpected(IcmpRefusal::kNonInitialFragment);
  }
  if (orig.protocol == utils::Ipv4::kIcmp) {
    // Queries (echo, timestamp, ...) may be answered; errors may not.
    if (original.size() < ihl + 1) {
      return std::unexpected(IcmpRefusal::kMalformed);
    }
    const uint8_t t = original[ihl];
    if (t == 3 || t == 4 || t == 5 || t == 11 || t == 12) {
      return std::unexpected(IcmpRefusal::kIcmpError);
    }
  }
  const size_t total = Icmpv4ErrorSize(original);
  if (out.size() < total) {
    return std::unexpected(IcmpRefusal::kOutputTooSmall);
  }
  const size_t quoted = total - sizeof(utils::Ipv4) - sizeof(utils::Icmp);

  utils::Ipv4 ip{};
  ip.version = 4;
  ip.header_length = 5;
  ip.length = be16_t(static_cast<uint16_t>(total));
  ip.ttl = spec.ttl;
  ip.protocol = utils::Ipv4::kIcmp;
  ip.src = spec.source;
  ip.dst = orig.src;
  ip.checksum = utils::CalculateIpv4NoOptChecksum(ip);
  std::memcpy(out.data(), &ip, sizeof(ip));

  uint8_t *icmp = out.data() + sizeof(ip);
  icmp[0] = static_cast<uint8_t>(spec.type);
  icmp[1] = spec.code;
  icmp[2] = icmp[3] = 0;  // checksum, below
  icmp[4] = icmp[5] = 0;  // unused
  const be16_t mtu(spec.type == IcmpType::kDestinationUnreachable &&
                           spec.code == kIcmpCodeFragmentationNeeded
                       ? spec.next_hop_mtu
                       : 0);
  std::memcpy(icmp + 6, &mtu, 2);
  std::memcpy(icmp + 8, original.data(), quoted);
  const uint16_t sum = utils::CalculateGenericChecksum(icmp, sizeof(utils::Icmp) + quoted);
  std::memcpy(icmp + 2, &sum, 2);
  return total;
}

// -- ARP (IPv4 over Ethernet) ---------------------------------------------------

struct ArpMessage {
  uint16_t opcode;  // utils::Arp::kRequest or kReply
  utils::Ethernet::Address sender_mac;
  be32_t sender_ip;
  utils::Ethernet::Address target_mac;
  be32_t target_ip;
};

inline constexpr size_t kArpFrameSize = sizeof(utils::Ethernet) + sizeof(utils::Arp);

// The ARP message in an Ethernet frame, or nullopt if the frame is not
// IPv4-over-Ethernet ARP (EtherType, hardware/protocol type and lengths). Any
// opcode is returned; the caller acts on requests and replies.
inline std::optional<ArpMessage> ParseArp(std::span<const uint8_t> frame) noexcept {
  if (frame.size() < kArpFrameSize) {
    return std::nullopt;
  }
  utils::Ethernet eth;
  std::memcpy(&eth, frame.data(), sizeof(eth));
  utils::Arp arp;
  std::memcpy(&arp, frame.data() + sizeof(eth), sizeof(arp));
  if (eth.ether_type.value() != utils::Ethernet::kArp ||
      arp.hw_addr.value() != utils::Arp::kEthernet ||
      arp.proto_addr.value() != utils::Ethernet::kIpv4 || arp.hw_addr_length != 6 ||
      arp.proto_addr_length != 4) {
    return std::nullopt;
  }
  return ArpMessage{arp.opcode.value(), arp.sender_hw_addr, arp.sender_ip_addr,
                    arp.target_hw_addr, arp.target_ip_addr};
}

namespace internal {
inline size_t WriteArp(std::span<uint8_t> out, const utils::Ethernet::Address &eth_dst,
                       const ArpMessage &m) noexcept {
  if (out.size() < kArpFrameSize) {
    return 0;
  }
  utils::Ethernet eth;
  eth.dst_addr = eth_dst;
  eth.src_addr = m.sender_mac;
  eth.ether_type = be16_t(utils::Ethernet::kArp);
  utils::Arp arp;
  arp.hw_addr = be16_t(utils::Arp::kEthernet);
  arp.proto_addr = be16_t(utils::Ethernet::kIpv4);
  arp.hw_addr_length = 6;
  arp.proto_addr_length = 4;
  arp.opcode = be16_t(m.opcode);
  arp.sender_hw_addr = m.sender_mac;
  arp.sender_ip_addr = m.sender_ip;
  arp.target_hw_addr = m.target_mac;
  arp.target_ip_addr = m.target_ip;
  std::memcpy(out.data(), &eth, sizeof(eth));
  std::memcpy(out.data() + sizeof(eth), &arp, sizeof(arp));
  return kArpFrameSize;
}
}  // namespace internal

// A broadcast who-has `target_ip` from (my_mac, my_ip). Returns the frame
// size (kArpFrameSize; pad to 60 bytes if the port does not), 0 if `out` is
// too small.
inline size_t BuildArpRequest(const utils::Ethernet::Address &my_mac, be32_t my_ip,
                              be32_t target_ip, std::span<uint8_t> out) noexcept {
  utils::Ethernet::Address broadcast;
  std::memset(broadcast.bytes, 0xff, sizeof(broadcast.bytes));
  utils::Ethernet::Address unknown;
  std::memset(unknown.bytes, 0, sizeof(unknown.bytes));
  return internal::WriteArp(out, broadcast,
                            {utils::Arp::kRequest, my_mac, my_ip, unknown, target_ip});
}

// The reply to `request` (whose target is one of the caller's addresses),
// unicast to the requester. 0 if `request` is not a request or `out` is too
// small.
inline size_t BuildArpReply(const ArpMessage &request, const utils::Ethernet::Address &my_mac,
                            std::span<uint8_t> out) noexcept {
  if (request.opcode != utils::Arp::kRequest) {
    return 0;
  }
  return internal::WriteArp(out, request.sender_mac,
                            {utils::Arp::kReply, my_mac, request.target_ip,
                             request.sender_mac, request.sender_ip});
}

}  // namespace bess::route

#endif  // BESS_ROUTE_L3_PACKET_H_
