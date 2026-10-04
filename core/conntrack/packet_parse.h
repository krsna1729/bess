// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONNTRACK_PACKET_PARSE_H_
#define BESS_CONNTRACK_PACKET_PARSE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace bess::conntrack {

// A narrow parse result for flow tracking (M17, D-067): where the L3 and L4
// headers are, the L4 ports or ICMP identity, and the TCP flags -- nothing a
// tracker does not use. Every offset is checked against the frame before it
// is recorded: a frame whose headers do not fit, or that lies about its
// lengths, is kMalformed, never read past. No fixed Ethernet header, no
// untagged-IPv4 assumption: up to two VLAN tags, IPv4 with options, IPv6 with
// its extension-header chain.

enum class L3Kind : uint8_t { kNone, kIpv4, kIpv6 };
enum class L4Kind : uint8_t { kNone, kTcp, kUdp, kIcmp, kIcmpv6, kOther };

enum class ParseStatus : uint8_t {
  kOk,
  kNotIp,           // a valid frame whose EtherType is not IPv4/IPv6
  kMalformed,       // a header does not fit or its lengths are inconsistent
  kFragment,        // a non-initial IP fragment: no L4 header to read
};

// TCP flag bits as on the wire (byte 13).
inline constexpr uint8_t kTcpFin = 0x01;
inline constexpr uint8_t kTcpSyn = 0x02;
inline constexpr uint8_t kTcpRst = 0x04;
inline constexpr uint8_t kTcpAck = 0x10;

struct ParsedFlowPacket {
  L3Kind l3 = L3Kind::kNone;
  L4Kind l4 = L4Kind::kNone;
  uint8_t protocol = 0;  // the IP protocol / final next header
  uint8_t tcp_flags = 0;
  uint16_t l3_offset = 0;
  uint16_t l4_offset = 0;
  uint16_t l4_length = 0;  // bytes of L4 header + payload inside the IP packet
  // Addresses in network byte order; IPv4 uses the first 4 bytes.
  std::array<uint8_t, 16> src{};
  std::array<uint8_t, 16> dst{};
  // Ports in host order (TCP/UDP), or for ICMP echo the identifier in both.
  uint16_t src_port = 0;
  uint16_t dst_port = 0;
  uint8_t icmp_type = 0;
  uint8_t icmp_code = 0;
  // The packet is the first fragment of a fragmented datagram: its L4 header
  // is here, but the UDP length or TCP options may extend past it.
  bool first_fragment = false;
};

namespace parse_internal {
inline uint16_t Be16(const uint8_t *p) noexcept {
  return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

inline constexpr size_t kMaxExtensionHeaders = 8;

// L4 at `off` of the IP packet at `ip`: `present` bytes of it are readable
// (headers must lie there), `total` is its stated length (which the length
// checks use; for a chained packet the rest lies in later segments).
inline ParseStatus ParseL4(const uint8_t *ip, size_t present, size_t total, size_t off,
                           size_t frame_base, ParsedFlowPacket &out) noexcept {
  out.l4_offset = static_cast<uint16_t>(frame_base + off);
  out.l4_length = static_cast<uint16_t>(total - off);
  const uint8_t *l4 = ip + off;
  const size_t have = present > off ? present - off : 0;  // readable header bytes
  const size_t room = total - off;                          // the L4 length
  switch (out.protocol) {
    case 6: {  // TCP
      // A first fragment holds at least the fixed header; its options may
      // continue in the next fragment.
      if (have < 20 || (l4[12] >> 4) < 5 ||
          (!out.first_fragment && static_cast<size_t>(l4[12] >> 4) * 4 > room)) {
        return ParseStatus::kMalformed;
      }
      out.l4 = L4Kind::kTcp;
      out.src_port = Be16(l4);
      out.dst_port = Be16(l4 + 2);
      out.tcp_flags = l4[13];
      return ParseStatus::kOk;
    }
    case 17: {  // UDP
      // The UDP length covers the whole datagram, so a first fragment holds
      // less than it says.
      if (have < 8 || Be16(l4 + 4) < 8 || (!out.first_fragment && Be16(l4 + 4) > room)) {
        return ParseStatus::kMalformed;
      }
      out.l4 = L4Kind::kUdp;
      out.src_port = Be16(l4);
      out.dst_port = Be16(l4 + 2);
      return ParseStatus::kOk;
    }
    case 1:     // ICMP
    case 58: {  // ICMPv6
      if (have < 8) {
        return ParseStatus::kMalformed;
      }
      out.l4 = out.protocol == 1 ? L4Kind::kIcmp : L4Kind::kIcmpv6;
      out.icmp_type = l4[0];
      out.icmp_code = l4[1];
      out.src_port = out.dst_port = Be16(l4 + 4);  // echo identifier
      return ParseStatus::kOk;
    }
    default:
      out.l4 = L4Kind::kOther;
      return ParseStatus::kOk;
  }
}

// An IP packet starting at `ip` (frame offset `base`): `present` bytes are
// readable, `limit` bytes are the packet's (present <= limit; they differ for
// a chained packet). Headers must be present; lengths are checked against
// `limit`.
inline ParseStatus ParseIp(const uint8_t *ip, size_t present, size_t limit, size_t base,
                           ParsedFlowPacket &out) noexcept {
  if (present < 1) {
    return ParseStatus::kMalformed;
  }
  out.l3_offset = static_cast<uint16_t>(base);
  const unsigned version = ip[0] >> 4;
  if (version == 4) {
    const size_t ihl = static_cast<size_t>(ip[0] & 0x0f) * 4;
    if (present < 20 || ihl < 20 || ihl > present) {
      return ParseStatus::kMalformed;
    }
    const size_t total = Be16(ip + 2);
    if (total < ihl || total > limit) {
      return ParseStatus::kMalformed;
    }
    out.l3 = L3Kind::kIpv4;
    out.protocol = ip[9];
    std::memcpy(out.src.data(), ip + 12, 4);
    std::memcpy(out.dst.data(), ip + 16, 4);
    if ((Be16(ip + 6) & 0x1fff) != 0) {
      return ParseStatus::kFragment;
    }
    out.first_fragment = (Be16(ip + 6) & 0x2000) != 0;  // MF set, offset 0
    return ParseL4(ip, std::min(present, total), total, ihl, base, out);
  }
  if (version == 6) {
    if (present < 40) {
      return ParseStatus::kMalformed;
    }
    const size_t total = 40 + size_t{Be16(ip + 4)};
    if (total > limit) {
      return ParseStatus::kMalformed;
    }
    const size_t have = std::min(present, total);
    out.l3 = L3Kind::kIpv6;
    std::memcpy(out.src.data(), ip + 8, 16);
    std::memcpy(out.dst.data(), ip + 24, 16);
    uint8_t next = ip[6];
    size_t off = 40;
    for (size_t n = 0;; n++) {
      if (n > kMaxExtensionHeaders) {
        return ParseStatus::kMalformed;
      }
      if (next == 0 || next == 43 || next == 60) {  // hop-by-hop, routing, dest opts
        if (off + 8 > have) {
          return ParseStatus::kMalformed;
        }
        const size_t len = (size_t{ip[off + 1]} + 1) * 8;
        if (off + len > have) {
          return ParseStatus::kMalformed;
        }
        next = ip[off];
        off += len;
        continue;
      }
      if (next == 44) {  // fragment
        if (off + 8 > have) {
          return ParseStatus::kMalformed;
        }
        const bool initial = (Be16(ip + off + 2) & 0xfff8) == 0;
        next = ip[off];
        off += 8;
        if (!initial) {
          out.protocol = next;
          return ParseStatus::kFragment;
        }
        out.first_fragment = true;
        continue;
      }
      break;
    }
    out.protocol = next;
    return ParseL4(ip, have, total, off, base, out);
  }
  return ParseStatus::kMalformed;
}
}  // namespace parse_internal

// Parses an Ethernet frame (optionally with up to two 802.1Q/802.1ad tags).
// `frame` is the readable bytes (for a chained packet, its first segment) and
// `total_len` the packet's length (default: the span's): the headers must be
// in `frame`, and the IP and L4 lengths are checked against `total_len`.
inline ParseStatus ParseFrame(std::span<const uint8_t> frame, ParsedFlowPacket &out,
                              size_t total_len = 0) noexcept {
  if (total_len < frame.size()) {
    total_len = frame.size();
  }
  out = ParsedFlowPacket{};
  size_t off = 12;
  if (frame.size() < 14) {
    return ParseStatus::kMalformed;
  }
  uint16_t type = parse_internal::Be16(frame.data() + off);
  for (int tags = 0; (type == 0x8100 || type == 0x88a8) && tags < 2; tags++) {
    off += 4;
    if (frame.size() < off + 2) {
      return ParseStatus::kMalformed;
    }
    type = parse_internal::Be16(frame.data() + off);
  }
  off += 2;
  if (type != 0x0800 && type != 0x86dd) {
    return ParseStatus::kNotIp;
  }
  const ParseStatus s = parse_internal::ParseIp(frame.data() + off, frame.size() - off,
                                                total_len - off, off, out);
  if (s == ParseStatus::kOk &&
      ((type == 0x0800) != (out.l3 == L3Kind::kIpv4))) {
    return ParseStatus::kMalformed;  // EtherType and IP version disagree
  }
  return s;
}

// Parses a packet that starts at its IP header.
inline ParseStatus ParseIpPacket(std::span<const uint8_t> packet, ParsedFlowPacket &out) noexcept {
  out = ParsedFlowPacket{};
  return parse_internal::ParseIp(packet.data(), packet.size(), packet.size(), 0, out);
}

}  // namespace bess::conntrack

#endif  // BESS_CONNTRACK_PACKET_PARSE_H_
