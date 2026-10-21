// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_L3_PACKET_H_
#define BESS_ROUTE_L3_PACKET_H_

#include <array>
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

// -- ECMP flow hash --------------------------------------------------------------

// The hash a route that names a next-hop group picks its member with: the
// addresses, the ports and the protocol, mixed. Which fields a deployment
// hashes is its own choice -- `FlowHashFields` below, the module's `flow_hash`
// arg (D-163); what matters here is that every path resolving the same group
// hashes the same way, so a flow keeps one member. The Router module and the
// router appliance call this one function, so a flow cannot land on different
// members in the two paths. `src`/`dst` are the header's address words as
// stored (one byte order for all callers), the ports are host order.
//
// `kL3L4` is the whole flow: a group spreads one flow's packets over one
// member. `kL3` is the address pair alone, which pins every flow to one
// destination to the same member -- what a graph wants when a subscriber's
// traffic must not be spread. The parse reads the L4 header either way (a
// later fragment has none, and is refused before any hash is taken); `kL3`
// only keeps its ports out of the hash.
enum class FlowHashFields : uint8_t {
  kL3L4,
  kL3,
};

inline uint32_t FlowHash(uint32_t src, uint32_t dst, uint16_t src_port,
                         uint16_t dst_port, uint8_t protocol,
                         FlowHashFields fields = FlowHashFields::kL3L4) noexcept {
  uint64_t x = uint64_t{src} << 32u | dst;
  if (fields == FlowHashFields::kL3L4) {
    x ^= (uint64_t{src_port} << 24u | uint64_t{dst_port} << 8u | uint64_t{protocol});
  }
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  return static_cast<uint32_t>(x ^ (x >> 31));
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
      (original[0] & 0x0f) < 5 ||
      original.size() < static_cast<size_t>(original[0] & 0x0fu) * 4) {
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

// Answers a local ARP request in place: rewrites the Ethernet frame into the
// reply (my_mac, my_ip) would send to the requester, exactly as BuildArpReply
// does, leaving any bytes beyond the ARP frame untouched. Returns false and
// changes nothing when `frame` is not an IPv4-over-Ethernet request for
// `my_ip`. The interface has already removed its VLAN tag, and the caller
// owns the buffer. This is the local-responder half of the resolution
// library; a VIF uses it instead of rewriting the ARP fields itself.
inline bool AnswerArpRequest(std::span<uint8_t> frame,
                             const utils::Ethernet::Address &my_mac,
                             be32_t my_ip) noexcept {
  const std::optional<ArpMessage> request = ParseArp(frame);
  if (!request || request->opcode != utils::Arp::kRequest ||
      request->target_ip != my_ip) {
    return false;
  }
  std::array<uint8_t, kArpFrameSize> reply{};
  if (BuildArpReply(*request, my_mac, reply) != kArpFrameSize) {
    return false;
  }
  std::memcpy(frame.data(), reply.data(), kArpFrameSize);
  return true;
}

// -- IPv6 neighbor discovery (RFC 4861) ---------------------------------------

using utils::Ipv6Address;

inline constexpr size_t kIpv6HeaderSize = 40;
inline constexpr size_t kIcmpv6HeaderSize = 4;
// One ICMPv6 ND message before options: type/code/checksum, reserved/flags,
// target address.
inline constexpr size_t kNdMessageSize = 8 + 16;
inline constexpr size_t kNdOptionSize = 8;  // one Ethernet link-layer option
inline constexpr size_t kNdFrameSize =
    sizeof(utils::Ethernet) + kIpv6HeaderSize + kNdMessageSize + kNdOptionSize;
// The largest neighbor solicitation either family builds (ARP is 42 bytes).
inline constexpr size_t kMaxNeighborFrameSize = kNdFrameSize;
inline constexpr uint8_t kIpv6NextHeaderIcmpv6 = 58;
inline constexpr uint8_t kNdHopLimit = 255;

enum class NdType : uint8_t {
  kRouterSolicitation = 133,
  kRouterAdvertisement = 134,
  kNeighborSolicitation = 135,
  kNeighborAdvertisement = 136,
};

inline constexpr uint8_t kNdOptionSourceLinkLayer = 1;
inline constexpr uint8_t kNdOptionTargetLinkLayer = 2;

struct NdMessage {
  NdType type;
  Ipv6Address source_ip;
  Ipv6Address destination_ip;
  Ipv6Address target_ip;
  utils::Ethernet::Address source_mac;  // Ethernet source
  std::optional<utils::Ethernet::Address> source_ll;  // option 1
  std::optional<utils::Ethernet::Address> target_ll;  // option 2
  bool router = false;    // NA R flag
  bool solicited = false; // NA S flag
  bool override = false;  // NA O flag
};

// The solicited-node multicast address of `target`: ff02::1:ffXX:XXXX.
inline Ipv6Address SolicitedNodeAddress(const Ipv6Address &target) noexcept {
  Ipv6Address address;
  address.bytes[0] = 0xff;
  address.bytes[1] = 0x02;
  address.bytes[11] = 0x01;
  address.bytes[12] = 0xff;
  address.bytes[13] = target.bytes[13];
  address.bytes[14] = target.bytes[14];
  address.bytes[15] = target.bytes[15];
  return address;
}

// The Ethernet multicast MAC for a solicited-node address: 33:33:ff:XX:XX:XX.
inline utils::Ethernet::Address SolicitedNodeMac(
    const Ipv6Address &target) noexcept {
  utils::Ethernet::Address mac;
  mac.bytes[0] = 0x33;
  mac.bytes[1] = 0x33;
  mac.bytes[2] = 0xff;
  mac.bytes[3] = target.bytes[13];
  mac.bytes[4] = target.bytes[14];
  mac.bytes[5] = target.bytes[15];
  return mac;
}

// The all-nodes multicast address (ff02::1) and its MAC.
inline Ipv6Address AllNodesAddress() noexcept {
  Ipv6Address address;
  address.bytes[0] = 0xff;
  address.bytes[1] = 0x02;
  address.bytes[15] = 0x01;
  return address;
}
inline utils::Ethernet::Address AllNodesMac() noexcept {
  utils::Ethernet::Address mac = {};
  mac.bytes[0] = 0x33;
  mac.bytes[1] = 0x33;
  mac.bytes[5] = 0x01;
  return mac;
}

namespace internal {
// Sum of the ICMPv6 pseudo-header and `payload`; FoldChecksum() of it is the
// checksum to write (zero when a stored checksum is valid).
inline uint16_t Icmpv6ChecksumSum(const Ipv6Address &source,
                                  const Ipv6Address &destination,
                                  std::span<const uint8_t> payload) noexcept {
  uint8_t pseudo[40] = {};
  std::memcpy(pseudo, source.bytes, 16);
  std::memcpy(pseudo + 16, destination.bytes, 16);
  const uint32_t length = static_cast<uint32_t>(payload.size());
  pseudo[32] = static_cast<uint8_t>(length >> 24u);
  pseudo[33] = static_cast<uint8_t>(length >> 16u);
  pseudo[34] = static_cast<uint8_t>(length >> 8u);
  pseudo[35] = static_cast<uint8_t>(length);
  pseudo[39] = kIpv6NextHeaderIcmpv6;
  uint64_t sum = utils::CalculateSum(pseudo, sizeof(pseudo));
  sum += utils::CalculateSum(payload.data(), payload.size());
  sum = (sum >> 32u) + (sum & 0xffffffffu);
  sum += sum >> 32u;
  return utils::FoldChecksum(static_cast<uint32_t>(sum));
}
}  // namespace internal

inline bool VerifyIcmpv6Checksum(const Ipv6Address &source,
                                 const Ipv6Address &destination,
                                 std::span<const uint8_t> payload) noexcept {
  return internal::Icmpv6ChecksumSum(source, destination, payload) == 0;
}

// Parses an Ethernet IPv6 Neighbor Solicitation or Advertisement. Refuses:
// a non-IPv6/non-ND frame, version other than 6, next header other than
// ICMPv6, hop limit other than 255, a bad ICMPv6 checksum, a multicast or
// unspecified target, a multicast source, a truncated or malformed option, and
// a link-layer option that disagrees with the Ethernet source. A source
// address of :: is allowed only for duplicate-address detection: a solicitation
// with no source link-layer option whose destination is the target's
// solicited-node multicast address (RFC 4861 7.1.1). An advertisement sent to a
// multicast destination must have the Solicited flag clear.
inline std::optional<NdMessage> ParseNeighborDiscovery(
    std::span<const uint8_t> frame) noexcept {
  constexpr size_t kEth = sizeof(utils::Ethernet);
  if (frame.size() < kEth + kIpv6HeaderSize + kNdMessageSize) {
    return std::nullopt;
  }
  utils::Ethernet eth;
  std::memcpy(&eth, frame.data(), sizeof(eth));
  if (eth.ether_type.value() != utils::Ethernet::kIpv6 ||
      (frame[kEth] >> 4u) != 6) {
    return std::nullopt;
  }
  const size_t payload_length = static_cast<size_t>(frame[kEth + 4]) << 8u |
                                frame[kEth + 5];
  if (payload_length < kNdMessageSize ||
      kEth + kIpv6HeaderSize + payload_length > frame.size() ||
      frame[kEth + 6] != kIpv6NextHeaderIcmpv6 ||
      frame[kEth + 7] != kNdHopLimit) {
    return std::nullopt;
  }
  NdMessage message;
  std::memcpy(message.source_ip.bytes, frame.data() + kEth + 8, 16);
  std::memcpy(message.destination_ip.bytes, frame.data() + kEth + 24, 16);
  message.source_mac = eth.src_addr;
  if (message.source_ip.IsMulticast()) {
    return std::nullopt;
  }
  const std::span<const uint8_t> payload(frame.data() + kEth + kIpv6HeaderSize,
                                         payload_length);
  const uint8_t type = payload[0];
  if ((type != static_cast<uint8_t>(NdType::kNeighborSolicitation) &&
       type != static_cast<uint8_t>(NdType::kNeighborAdvertisement)) ||
      payload[1] != 0 || !VerifyIcmpv6Checksum(message.source_ip,
                                              message.destination_ip, payload)) {
    return std::nullopt;
  }
  message.type = static_cast<NdType>(type);
  std::memcpy(message.target_ip.bytes, payload.data() + 8, 16);
  if (message.target_ip.IsMulticast() || message.target_ip.IsUnspecified()) {
    return std::nullopt;
  }
  if (message.type == NdType::kNeighborAdvertisement) {
    const uint8_t flags = payload[4];
    message.router = (flags & 0x80u) != 0;
    message.solicited = (flags & 0x40u) != 0;
    message.override = (flags & 0x20u) != 0;
    if (message.source_ip.IsUnspecified()) {
      return std::nullopt;
    }
    // RFC 4861 7.1.1: an advertisement sent to a multicast destination must
    // have the Solicited flag clear; otherwise it is silently discarded.
    if (message.destination_ip.IsMulticast() && message.solicited) {
      return std::nullopt;
    }
  }
  size_t offset = kNdMessageSize;
  while (offset < payload.size()) {
    if (payload.size() - offset < 2) {
      return std::nullopt;
    }
    const uint8_t option = payload[offset];
    const size_t length = static_cast<size_t>(payload[offset + 1]) * 8;
    if (length == 0 || length > payload.size() - offset) {
      return std::nullopt;
    }
    if (option == kNdOptionSourceLinkLayer ||
        option == kNdOptionTargetLinkLayer) {
      if (length != kNdOptionSize) {
        return std::nullopt;
      }
      utils::Ethernet::Address mac;
      std::memcpy(mac.bytes, payload.data() + offset + 2, mac.kSize);
      if (mac != eth.src_addr) {
        return std::nullopt;
      }
      if (option == kNdOptionSourceLinkLayer) {
        message.source_ll = mac;
      } else {
        message.target_ll = mac;
      }
    }
    offset += length;
  }
  if (message.type == NdType::kNeighborSolicitation &&
      message.source_ip.IsUnspecified()) {
    // RFC 4861 7.1.1: a solicitation from :: is duplicate-address detection.
    // Its destination must be the target's solicited-node multicast address
    // and it must not carry a source link-layer option.
    if (message.source_ll ||
        message.destination_ip != SolicitedNodeAddress(message.target_ip)) {
      return std::nullopt;
    }
  }
  return message;
}

namespace internal {
inline size_t WriteIpv6Nd(std::span<uint8_t> out,
                          const utils::Ethernet::Address &eth_dst,
                          const utils::Ethernet::Address &eth_src,
                          const Ipv6Address &source_ip,
                          const Ipv6Address &destination_ip,
                          uint16_t payload_length) noexcept {
  const size_t frame_size =
      sizeof(utils::Ethernet) + kIpv6HeaderSize + payload_length;
  if (out.size() < frame_size) {
    return 0;
  }
  utils::Ethernet eth;
  eth.dst_addr = eth_dst;
  eth.src_addr = eth_src;
  eth.ether_type = be16_t(utils::Ethernet::kIpv6);
  std::memcpy(out.data(), &eth, sizeof(eth));
  uint8_t *ip = out.data() + sizeof(eth);
  std::memset(ip, 0, kIpv6HeaderSize + payload_length);
  ip[0] = 0x60;  // version 6, traffic class 0, flow label 0
  ip[4] = static_cast<uint8_t>(static_cast<uint32_t>(payload_length) >> 8u);
  ip[5] = static_cast<uint8_t>(payload_length);
  ip[6] = kIpv6NextHeaderIcmpv6;
  ip[7] = kNdHopLimit;
  std::memcpy(ip + 8, source_ip.bytes, 16);
  std::memcpy(ip + 24, destination_ip.bytes, 16);
  return frame_size;
}
}  // namespace internal

// A Neighbor Solicitation for `target_ip` from (my_mac, my_ip), sent to the
// target's solicited-node multicast address with the source link-layer option.
// 0 if `out` is smaller than kNdFrameSize or `my_ip` is unspecified/multicast.
inline size_t BuildNeighborSolicitation(
    const utils::Ethernet::Address &my_mac, const Ipv6Address &my_ip,
    const Ipv6Address &target_ip, std::span<uint8_t> out) noexcept {
  if (my_ip.IsUnspecified() || my_ip.IsMulticast() ||
      target_ip.IsUnspecified() || target_ip.IsMulticast()) {
    return 0;
  }
  const Ipv6Address destination = SolicitedNodeAddress(target_ip);
  const size_t size = internal::WriteIpv6Nd(
      out, SolicitedNodeMac(target_ip), my_mac, my_ip, destination,
      kNdMessageSize + kNdOptionSize);
  if (size == 0) {
    return 0;
  }
  uint8_t *payload = out.data() + sizeof(utils::Ethernet) + kIpv6HeaderSize;
  payload[0] = static_cast<uint8_t>(NdType::kNeighborSolicitation);
  payload[1] = 0;
  std::memcpy(payload + 8, target_ip.bytes, 16);
  payload[24] = kNdOptionSourceLinkLayer;
  payload[25] = 1;  // 8 bytes
  std::memcpy(payload + 26, my_mac.bytes, my_mac.kSize);
  const uint16_t checksum = internal::Icmpv6ChecksumSum(
      my_ip, destination,
      std::span<const uint8_t>(payload, kNdMessageSize + kNdOptionSize));
  // FoldChecksum() returns the value in host byte order; the field itself
  // holds network order.
  payload[2] = static_cast<uint8_t>(checksum & 0xffu);
  payload[3] = static_cast<uint8_t>(checksum >> 8u);
  return size;
}

// A Neighbor Advertisement for `target_ip` from (my_mac, source_ip), sent to
// `destination_mac`/`destination_ip` with the target link-layer option. The
// flags are the caller's: a solicited answer sets S and normally O.
inline size_t BuildNeighborAdvertisement(
    const utils::Ethernet::Address &my_mac, const Ipv6Address &source_ip,
    const Ipv6Address &target_ip, const utils::Ethernet::Address &destination_mac,
    const Ipv6Address &destination_ip, bool solicited, bool override,
    std::span<uint8_t> out, bool include_target_ll = true) noexcept {
  if (source_ip.IsUnspecified() || source_ip.IsMulticast() ||
      target_ip.IsUnspecified() || target_ip.IsMulticast()) {
    return 0;
  }
  const size_t payload_length =
      kNdMessageSize + (include_target_ll ? kNdOptionSize : 0);
  const size_t size = internal::WriteIpv6Nd(
      out, destination_mac, my_mac, source_ip, destination_ip,
      static_cast<uint16_t>(payload_length));
  if (size == 0) {
    return 0;
  }
  uint8_t *payload = out.data() + sizeof(utils::Ethernet) + kIpv6HeaderSize;
  payload[0] = static_cast<uint8_t>(NdType::kNeighborAdvertisement);
  payload[1] = 0;
  payload[4] = (solicited ? 0x40u : 0u) | (override ? 0x20u : 0u);
  std::memcpy(payload + 8, target_ip.bytes, 16);
  if (include_target_ll) {
    payload[24] = kNdOptionTargetLinkLayer;
    payload[25] = 1;
    std::memcpy(payload + 26, my_mac.bytes, my_mac.kSize);
  }
  const uint16_t checksum = internal::Icmpv6ChecksumSum(
      source_ip, destination_ip,
      std::span<const uint8_t>(payload, payload_length));
  // FoldChecksum() returns the value in host byte order; the field itself
  // holds network order.
  payload[2] = static_cast<uint8_t>(checksum & 0xffu);
  payload[3] = static_cast<uint8_t>(checksum >> 8u);
  return size;
}

// Answers a Neighbor Solicitation for `my_ip` into `out`, like
// AnswerArpRequest: the solicitation becomes the solicited Neighbor
// Advertisement. Every answer carries the Target Link-Layer Address option: a
// solicitation is addressed to the target's solicited-node multicast address
// and RFC 4861 7.2.4 requires the option when the solicitation's destination
// is multicast, duplicate-address detection included. A DAD solicitation
// (source ::, which has no source link-layer option) is answered to the
// all-nodes multicast address with the Solicited flag clear; with the option
// present the Override flag is set. Returns the frame length written (always
// kNdFrameSize), or 0 and `out` unchanged for anything that is not a
// solicitation for `my_ip` or when `out` is too small.
inline size_t AnswerNeighborSolicitation(std::span<const uint8_t> frame,
                                         const utils::Ethernet::Address &my_mac,
                                         const Ipv6Address &my_ip,
                                         std::span<uint8_t> out) noexcept {
  const std::optional<NdMessage> solicitation =
      ParseNeighborDiscovery(frame);
  if (!solicitation || solicitation->type != NdType::kNeighborSolicitation ||
      solicitation->target_ip != my_ip) {
    return 0;
  }
  const bool unspecified = solicitation->source_ip.IsUnspecified();
  const Ipv6Address destination_ip =
      unspecified ? AllNodesAddress() : solicitation->source_ip;
  const utils::Ethernet::Address destination_mac =
      unspecified ? AllNodesMac() : solicitation->source_mac;
  return BuildNeighborAdvertisement(
      my_mac, my_ip, my_ip, destination_mac, destination_ip,
      /*solicited=*/!unspecified, /*override=*/true, out,
      /*include_target_ll=*/true);
}

}  // namespace bess::route

#endif  // BESS_ROUTE_L3_PACKET_H_
