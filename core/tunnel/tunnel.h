// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_TUNNEL_TUNNEL_H_
#define BESS_TUNNEL_TUNNEL_H_

#include <rte_config.h>
#include <rte_hash_crc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "conntrack/packet_parse.h"
#include "utils/checksum.h"
#include "utils/ip.h"

namespace bess::tunnel {

// Tunnel packet mechanics (M19, D-069): header codecs and checks for VXLAN,
// Geneve, GRE and the GTP-U header, with no control semantics (no VTEP
// learning, no PDU session, no QFI, no FAR). Encapsulation writes headers
// into bytes the caller has made room for (a prepend); decapsulation checks
// the outer headers and returns where the inner packet starts and the
// tunnel's identifier. Nothing here touches a Module or metadata.

// -- sizes and MTU -------------------------------------------------------------------

inline constexpr size_t kEthernetBytes = 14;
inline constexpr size_t kIpv4Bytes = 20;
inline constexpr size_t kIpv6Bytes = 40;
inline constexpr size_t kUdpBytes = 8;
inline constexpr size_t kVxlanBytes = 8;
inline constexpr size_t kGeneveBaseBytes = 8;
inline constexpr size_t kGreBaseBytes = 4;
inline constexpr size_t kGtpuBaseBytes = 8;

enum class Outer : uint8_t { kIpv4, kIpv6 };

// Bytes a tunnel adds in front of the inner Ethernet frame (VXLAN, Geneve) or
// IP packet (GRE, GTP-U): outer Ethernet + IP + UDP + the tunnel header.
inline constexpr size_t VxlanOverhead(Outer o) noexcept {
  return kEthernetBytes + (o == Outer::kIpv4 ? kIpv4Bytes : kIpv6Bytes) + kUdpBytes + kVxlanBytes;
}
inline constexpr size_t GeneveOverhead(Outer o, size_t option_bytes) noexcept {
  return kEthernetBytes + (o == Outer::kIpv4 ? kIpv4Bytes : kIpv6Bytes) + kUdpBytes +
         kGeneveBaseBytes + option_bytes;
}
// The largest inner frame (or packet) that fits an outer link MTU (the IP MTU
// of the underlay: Ethernet header not counted).
inline constexpr size_t InnerMtu(size_t outer_mtu, size_t overhead) noexcept {
  const size_t ip_and_up = overhead - kEthernetBytes;
  return outer_mtu > ip_and_up ? outer_mtu - ip_and_up : 0;
}

// -- shared ---------------------------------------------------------------------------

namespace detail {
inline void Put16(uint8_t *p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
inline void Put32(uint8_t *p, uint32_t v) noexcept {
  Put16(p, static_cast<uint16_t>(v >> 16));
  Put16(p + 2, static_cast<uint16_t>(v));
}
inline uint16_t Get16(const uint8_t *p) noexcept { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline uint32_t Get32(const uint8_t *p) noexcept {
  return uint32_t{Get16(p)} << 16 | Get16(p + 2);
}
}  // namespace detail

// The UDP source port for a tunnel packet: a hash of the inner flow, in the
// dynamic range 0xc000-0xffff (RFC 7348 section 5; RFC 6335), so the
// underlay's ECMP spreads tunnels by inner flow and keeps a flow on one path.
// The legacy VXLANEncap's function with bounds checks and one fix (D-069): an
// inner IPv4 TCP/UDP packet hashes its protocol, ports and addresses; other
// IPv4 its protocol and addresses; any other frame its two MAC addresses. (The
// legacy code added the header length in words, not bytes, so it hashed IP
// header bytes 5-8 -- identification, fragment, TTL -- instead of the ports:
// the port changed per packet and the underlay could reorder a flow.) A frame
// too short for the ports, or fragmented, hashes without them.
inline uint16_t FlowEntropyPort(std::span<const uint8_t> inner) noexcept {
  uint32_t h = 0;
  if (inner.size() < 12) {
    return 0xc000;
  }
  const bool ipv4 = inner.size() >= kEthernetBytes + kIpv4Bytes && detail::Get16(inner.data() + 12) == 0x0800;
  if (!ipv4) {
    h = rte_hash_crc(inner.data(), 12, UINT32_MAX);
  } else {
    const uint8_t *ip = inner.data() + kEthernetBytes;
    const size_t ihl = static_cast<size_t>(ip[0] & 0x0f) * 4;
    h = ip[9];
    const size_t l4 = kEthernetBytes + ihl;
    // Ports only for an unfragmented packet: a fragment's bytes there are
    // payload (or absent), and a datagram's fragments must share one path.
    const bool fragment = (detail::Get16(ip + 6) & 0x3fff) != 0;
    if ((ip[9] == 6 || ip[9] == 17) && !fragment && inner.size() >= l4 + 4) {
      h = rte_hash_crc(inner.data() + l4, 4, h);
    }
    h = rte_hash_crc(ip + 12, 8, h);
  }
  return static_cast<uint16_t>(h | 0xc000);
}

// IPv4 outer header (no options), checksum computed. `payload` is the bytes
// after it. `id` 0 and DF set: tunnels do not fragment the outer packet.
inline void WriteIpv4(uint8_t *ip, utils::be32_t src, utils::be32_t dst, uint8_t protocol,
                      uint16_t payload, uint8_t ttl = 64, uint8_t tos = 0) noexcept {
  std::memset(ip, 0, kIpv4Bytes);
  ip[0] = 0x45;
  ip[1] = tos;
  detail::Put16(ip + 2, static_cast<uint16_t>(kIpv4Bytes + payload));
  detail::Put16(ip + 6, 0x4000);
  ip[8] = ttl;
  ip[9] = protocol;
  std::memcpy(ip + 12, &src, 4);
  std::memcpy(ip + 16, &dst, 4);
  const uint16_t sum = utils::CalculateIpv4NoOptChecksum(*reinterpret_cast<const utils::Ipv4 *>(ip));
  std::memcpy(ip + 10, &sum, 2);
}

// UDP header; checksum 0 ("checksum intent": zero is allowed for VXLAN, Geneve
// and GTP-U over IPv4; over IPv6 the caller computes it, RFC 6935/6936).
inline void WriteUdp(uint8_t *udp, uint16_t src_port, uint16_t dst_port, uint16_t payload) noexcept {
  detail::Put16(udp, src_port);
  detail::Put16(udp + 2, dst_port);
  detail::Put16(udp + 4, static_cast<uint16_t>(kUdpBytes + payload));
  detail::Put16(udp + 6, 0);
}

// -- decapsulation result --------------------------------------------------------------

enum class DecapError : uint8_t {
  kOk,             // decapsulated; the out-parameter is filled
  kMalformed,      // the outer headers do not parse (conntrack::ParseFrame)
  kNotTunnel,      // not UDP to the tunnel's port / not the GRE protocol
  kBadHeader,      // the tunnel header is short, a reserved version, or a flag missing
  kNoInner,        // too short for the inner header the tunnel carries
};

// What a decapsulation found. Results are written through out-parameters
// (the outer parse into the caller's ParsedFlowPacket, this into the
// caller's Decapsulated) and the function returns a status: returning them
// by value, in a std::expected, made the caller copy fields just written
// with narrow stores, which defeated store forwarding (measured, unisolated:
// VXLAN 10.3 against 5.3 ns a decap, GRE 20.1 against 5.4, D-069).
struct Decapsulated {
  uint32_t inner_offset;  // where the inner frame/packet starts in the outer frame
  uint32_t id;            // VNI, GRE key (0 if absent), TEID
  uint16_t protocol;      // Geneve/GRE protocol type (EtherType); 0x6558 for VXLAN's Ethernet
};

// Every decapsulation takes the readable bytes (`frame`: for a chained
// packet, its first segment) and the packet's length (`total_len`, default
// the span's). The tunnel header must be readable; the lengths are checked
// against the whole packet; the inner packet must start in `frame`.

namespace detail {
// Parses the outer headers; `end` is the end of the outer L4 payload.
inline DecapError Outer(std::span<const uint8_t> frame, size_t total_len,
                        conntrack::ParsedFlowPacket &outer, size_t &end) noexcept {
  if (conntrack::ParseFrame(frame, outer, total_len) != conntrack::ParseStatus::kOk ||
      outer.first_fragment) {
    // A fragmented outer packet (even its first fragment, whose L4 header the
    // parser accepts) carries a cut inner packet: reassemble before decap.
    return DecapError::kMalformed;
  }
  end = size_t{outer.l4_offset} + outer.l4_length;
  return DecapError::kOk;
}
}  // namespace detail

// -- VXLAN (RFC 7348) -----------------------------------------------------------------

inline constexpr uint16_t kVxlanPort = 4789;

// UDP + VXLAN headers (16 bytes) in front of an inner Ethernet frame of
// `inner_len` bytes: the legacy VXLANEncap's headers (flags I, the VNI's 24
// bits, UDP checksum 0).
inline void WriteVxlanUdp(uint8_t *hdr, uint32_t vni, uint16_t src_port, uint16_t dst_port,
                          size_t inner_len) noexcept {
  WriteUdp(hdr, src_port, dst_port, static_cast<uint16_t>(kVxlanBytes + inner_len));
  detail::Put32(hdr + 8, 0x08000000);
  detail::Put32(hdr + 12, (vni & 0xffffff) << 8);
}

// Checks an outer Ethernet/IP/UDP/VXLAN frame to `port` (nullopt: any UDP
// port, for a caller that has already classified the packet as VXLAN) and
// finds the inner Ethernet frame: the I flag set, the inner frame at least an
// Ethernet header.
inline DecapError DecapVxlan(std::span<const uint8_t> frame, conntrack::ParsedFlowPacket &outer,
                             Decapsulated &out, std::optional<uint16_t> port = kVxlanPort,
                             size_t total_len = 0) noexcept {
  size_t end;
  if (const DecapError e = detail::Outer(frame, total_len, outer, end); e != DecapError::kOk) {
    return e;
  }
  if (outer.l4 != conntrack::L4Kind::kUdp || (port && outer.dst_port != *port)) {
    return DecapError::kNotTunnel;
  }
  const size_t vx = size_t{outer.l4_offset} + kUdpBytes;
  if (end < vx + kVxlanBytes || frame.size() < vx + kVxlanBytes || (frame[vx] & 0x08) == 0) {
    return DecapError::kBadHeader;
  }
  const size_t inner = vx + kVxlanBytes;
  if (end < inner + kEthernetBytes || frame.size() < inner) {
    return DecapError::kNoInner;
  }
  out.inner_offset = static_cast<uint32_t>(inner);
  out.id = detail::Get32(frame.data() + vx + 4) >> 8;
  out.protocol = 0x6558;
  return DecapError::kOk;
}

// -- Geneve (RFC 8926) ------------------------------------------------------------------

inline constexpr uint16_t kGenevePort = 6081;

// The Geneve base header (8 bytes) for `option_bytes` of options (a multiple
// of 4, at most 252) carrying `protocol` (0x6558 Ethernet, 0x0800 IPv4, ...).
inline bool WriteGeneve(uint8_t *g, uint32_t vni, uint16_t protocol, size_t option_bytes = 0,
                        bool oam = false, bool critical = false) noexcept {
  if (option_bytes % 4 != 0 || option_bytes > 252) {
    return false;
  }
  g[0] = static_cast<uint8_t>(option_bytes / 4);  // version 0
  g[1] = static_cast<uint8_t>((oam ? 0x80 : 0) | (critical ? 0x40 : 0));
  detail::Put16(g + 2, protocol);
  detail::Put32(g + 4, (vni & 0xffffff) << 8);
  return true;
}

struct GeneveInfo {
  Decapsulated tunnel;
  uint32_t options_offset;
  uint16_t options_bytes;
  bool oam;
  bool critical;  // a critical option is present: a decapsulator that does not
                  // understand the options must drop (RFC 8926 3.5)
};

inline DecapError DecapGeneve(std::span<const uint8_t> frame, conntrack::ParsedFlowPacket &outer,
                              GeneveInfo &info, uint16_t port = kGenevePort,
                              size_t total_len = 0) noexcept {
  size_t end;
  if (const DecapError e = detail::Outer(frame, total_len, outer, end); e != DecapError::kOk) {
    return e;
  }
  if (outer.l4 != conntrack::L4Kind::kUdp || outer.dst_port != port) {
    return DecapError::kNotTunnel;
  }
  const size_t g = size_t{outer.l4_offset} + kUdpBytes;
  if (end < g + kGeneveBaseBytes || frame.size() < g + kGeneveBaseBytes || (frame[g] >> 6) != 0) {
    return DecapError::kBadHeader;
  }
  info.options_offset = static_cast<uint32_t>(g + kGeneveBaseBytes);
  info.options_bytes = static_cast<uint16_t>((frame[g] & 0x3f) * 4);
  info.oam = (frame[g + 1] & 0x80) != 0;
  info.critical = (frame[g + 1] & 0x40) != 0;
  const size_t inner = size_t{info.options_offset} + info.options_bytes;
  if (end < inner || frame.size() < inner) {
    return DecapError::kBadHeader;
  }
  info.tunnel.inner_offset = static_cast<uint32_t>(inner);
  info.tunnel.id = detail::Get32(frame.data() + g + 4) >> 8;
  info.tunnel.protocol = detail::Get16(frame.data() + g + 2);
  if (info.tunnel.protocol == 0x6558 && end < inner + kEthernetBytes) {
    return DecapError::kNoInner;
  }
  return DecapError::kOk;
}

// -- GRE (RFC 2784, keys and sequence numbers RFC 2890) --------------------------------

struct GreOptions {
  std::optional<uint32_t> key;
  std::optional<uint32_t> sequence;
  bool checksum = false;
};

inline constexpr size_t GreBytes(const GreOptions &o) noexcept {
  return kGreBaseBytes + (o.checksum ? 4 : 0) + (o.key ? 4 : 0) + (o.sequence ? 4 : 0);
}

// A GRE header for `protocol` (0x0800 IPv4, 0x86dd IPv6, 0x6558 Ethernet). With
// `checksum`, `payload` is the encapsulated packet (the checksum covers the
// GRE header and payload).
inline size_t WriteGre(uint8_t *g, uint16_t protocol, const GreOptions &o,
                       std::span<const uint8_t> payload = {}) noexcept {
  const uint16_t flags = (o.checksum ? 0x8000 : 0) | (o.key ? 0x2000 : 0) | (o.sequence ? 0x1000 : 0);
  detail::Put16(g, flags);
  detail::Put16(g + 2, protocol);
  size_t off = kGreBaseBytes;
  const size_t sum_at = off;
  if (o.checksum) {
    detail::Put32(g + off, 0);
    off += 4;
  }
  if (o.key) {
    detail::Put32(g + off, *o.key);
    off += 4;
  }
  if (o.sequence) {
    detail::Put32(g + off, *o.sequence);
    off += 4;
  }
  if (o.checksum) {
    // The header is an even number of bytes, so the two sums concatenate.
    const uint32_t a = utils::CalculateSum(g, off);
    const uint32_t b = utils::CalculateSum(payload.data(), payload.size());
    uint64_t sum = uint64_t{a} + b;
    sum = (sum & 0xffffffff) + (sum >> 32);
    const uint16_t c = utils::FoldChecksum(static_cast<uint32_t>(sum));  // folded and inverted
    std::memcpy(g + sum_at, &c, 2);
  }
  return off;
}

struct GreInfo {
  size_t header_offset;
  size_t payload_offset;
  uint16_t protocol;
  std::optional<uint32_t> key;
  std::optional<uint32_t> sequence;
};

// A GRE header at `offset` of `bytes` (version 0; no routing). A present
// checksum is verified over the rest of `bytes`, which must then hold the
// whole GRE payload.
inline DecapError ParseGre(std::span<const uint8_t> bytes, size_t offset, GreInfo &info) noexcept {
  if (bytes.size() < offset + kGreBaseBytes) {
    return DecapError::kBadHeader;
  }
  const uint8_t *g = bytes.data() + offset;
  const uint16_t flags = detail::Get16(g);
  // Version 0; no routing, strict source route or recursion bits (RFC 2784
  // 2.3 discards them; RFC 2890 adds K and S).
  if ((flags & 0x4c07) != 0) {
    return DecapError::kBadHeader;
  }
  info.header_offset = offset;
  info.protocol = detail::Get16(g + 2);
  info.key.reset();
  info.sequence.reset();
  size_t off = kGreBaseBytes;
  const bool csum = flags & 0x8000;
  const size_t need = kGreBaseBytes + (csum ? 4 : 0) + ((flags & 0x2000) ? 4 : 0) + ((flags & 0x1000) ? 4 : 0);
  if (bytes.size() < offset + need) {
    return DecapError::kBadHeader;
  }
  if (csum) {
    if (!utils::VerifyGenericChecksum(g, bytes.size() - offset)) {
      return DecapError::kBadHeader;
    }
    off += 4;
  }
  if (flags & 0x2000) {
    info.key = detail::Get32(g + off);
    off += 4;
  }
  if (flags & 0x1000) {
    info.sequence = detail::Get32(g + off);
    off += 4;
  }
  info.payload_offset = offset + off;
  return DecapError::kOk;
}

// Checks an outer Ethernet/IP/GRE frame and finds the payload. A GRE
// checksum is verified only when the whole packet is in `frame`.
inline DecapError DecapGre(std::span<const uint8_t> frame, conntrack::ParsedFlowPacket &outer,
                           Decapsulated &out, size_t total_len = 0) noexcept {
  size_t end;
  if (const DecapError e = detail::Outer(frame, total_len, outer, end); e != DecapError::kOk) {
    return e;
  }
  if (outer.protocol != 47) {
    return DecapError::kNotTunnel;
  }
  if (end > frame.size() && frame.size() > outer.l4_offset &&
      (frame[outer.l4_offset] & 0x80) != 0) {
    return DecapError::kBadHeader;  // a checksum over bytes we cannot read
  }
  GreInfo gre;
  if (const DecapError e = ParseGre(frame.first(std::min(end, frame.size())), outer.l4_offset, gre);
      e != DecapError::kOk) {
    return e;
  }
  if (gre.payload_offset >= end) {
    return DecapError::kNoInner;
  }
  out.inner_offset = static_cast<uint32_t>(gre.payload_offset);
  out.id = gre.key.value_or(0);
  out.protocol = gre.protocol;
  return DecapError::kOk;
}

// -- GTP-U header (3GPP TS 29.281): the codec only -----------------------------------

inline constexpr uint16_t kGtpuPort = 2152;
inline constexpr uint8_t kGtpuGpdu = 255;

// A G-PDU header (8 bytes; no sequence number, N-PDU number or extension
// headers) for a payload of `payload` bytes.
inline void WriteGtpu(uint8_t *g, uint32_t teid, uint16_t payload) noexcept {
  g[0] = 0x30;  // version 1, PT 1, E/S/PN 0
  g[1] = kGtpuGpdu;
  detail::Put16(g + 2, payload);
  detail::Put32(g + 4, teid);
}

struct GtpuInfo {
  Decapsulated tunnel;
  uint8_t message_type;
  std::optional<uint16_t> sequence;
  // The first extension header's type, if the E flag is set (the chain is
  // walked and skipped; what the types mean, e.g. the PDU session container,
  // is the application's).
  std::optional<uint8_t> first_extension;
};

// Checks an outer Ethernet/IP/UDP/GTP-U frame to `port` and finds the T-PDU.
// Only G-PDUs (type 255) carry a payload; other messages return kNotTunnel
// (echo, error indication: the application's control path).
inline DecapError DecapGtpu(std::span<const uint8_t> frame, conntrack::ParsedFlowPacket &outer,
                            GtpuInfo &info, uint16_t port = kGtpuPort,
                            size_t total_len = 0) noexcept {
  size_t end;
  if (const DecapError e = detail::Outer(frame, total_len, outer, end); e != DecapError::kOk) {
    return e;
  }
  if (outer.l4 != conntrack::L4Kind::kUdp || outer.dst_port != port) {
    return DecapError::kNotTunnel;
  }
  info.sequence.reset();
  info.first_extension.reset();
  Decapsulated &d = info.tunnel;
  const size_t g = size_t{outer.l4_offset} + kUdpBytes;
  if (end < g + kGtpuBaseBytes || frame.size() < g + kGtpuBaseBytes || (frame[g] >> 5) != 1 ||
      (frame[g] & 0x10) == 0) {
    return DecapError::kBadHeader;  // version 1, PT 1 (GTP, not GTP')
  }
  info.message_type = frame[g + 1];
  const size_t length = detail::Get16(frame.data() + g + 2);  // after the 8 mandatory bytes
  if (g + kGtpuBaseBytes + length > end) {
    return DecapError::kBadHeader;
  }
  // The optional fields and extension headers must be readable.
  const size_t readable = std::min(frame.size(), g + kGtpuBaseBytes + length);
  d.id = detail::Get32(frame.data() + g + 4);
  size_t off = g + kGtpuBaseBytes;
  const uint8_t flags = frame[g] & 0x07;
  if (flags != 0) {
    if (length < 4 || readable < off + 4) {
      return DecapError::kBadHeader;
    }
    if (flags & 0x02) {
      info.sequence = detail::Get16(frame.data() + off);
    }
    uint8_t next = frame[off + 3];
    off += 4;
    if (flags & 0x04) {
      if (next != 0) {
        info.first_extension = next;
      }
      for (int n = 0; next != 0; n++) {
        // Each extension header: length in 4-byte units, content, next type.
        if (n > 16 || off + 1 > readable) {
          return DecapError::kBadHeader;
        }
        const size_t len = size_t{frame[off]} * 4;
        if (len == 0 || off + len > readable) {
          return DecapError::kBadHeader;
        }
        next = frame[off + len - 1];
        off += len;
      }
    }
  }
  if (info.message_type != kGtpuGpdu) {
    return DecapError::kNotTunnel;
  }
  d.inner_offset = static_cast<uint32_t>(off);
  d.protocol = 0;  // an IP packet (its version nibble says which)
  if (off >= g + kGtpuBaseBytes + length || off > frame.size()) {
    return DecapError::kNoInner;
  }
  return DecapError::kOk;
}

}  // namespace bess::tunnel

#endif  // BESS_TUNNEL_TUNNEL_H_
