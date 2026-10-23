// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_PACKET_PARSE_H_
#define BESS_PACKET_PARSE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace bess::packet {

// The checked packet parse (M17, D-067, D-188), one for every module that
// reads a frame's flow fields (ConnTrack, NAT, the tunnel decaps, Router,
// Firewall, HashLB's L3 and L4 modes): where the L3 and L4 headers are, the
// L4 ports or ICMP identity, and the TCP flags. Every offset is checked
// against the frame before it is recorded: a frame whose headers do not fit,
// or that lies about its lengths, is kMalformed, never read past. No fixed
// Ethernet header, no untagged-IPv4 assumption: up to two VLAN tags, IPv4
// with options, IPv6 with its extension-header chain (AH included). A reader
// that needs only the addresses, protocol and ports may test the common frame
// with CheckFastIpv4 first: the same checks, so the same decisions.

enum class L3Kind : uint8_t { kNone, kIpv4, kIpv6 };
enum class L4Kind : uint8_t { kNone, kTcp, kUdp, kIcmp, kIcmpv6, kOther };

enum class ParseStatus : uint8_t {
  kOk,
  kNotIp,           // a valid frame whose EtherType is not IPv4/IPv6
  kMalformed,       // a header does not fit or its lengths are inconsistent
  kFragment,        // a non-initial IP fragment: no L4 header to read
};

struct ParsedFrame {
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
  return static_cast<uint16_t>(unsigned{p[0]} << 8u | unsigned{p[1]});
}

inline constexpr size_t kMaxExtensionHeaders = 8;

// The IPv6 extension headers the walk steps over are the ones Linux's
// ipv6_skip_exthdr steps over: hop-by-hop (0), routing (43) and destination
// options (60), the fragment header (44), and AH (51). ESP (50) ends the walk:
// what follows it is encrypted, so it is the packet's last protocol. (IPv4 has
// no such walk: an AH there is the protocol, as in Linux.) The options headers
// are the common ones, so one bit test finds them.
inline constexpr uint64_t kOptionHeaders =
    uint64_t{1} << 0u | uint64_t{1} << 43u | uint64_t{1} << 60u;

// AH's length (RFC 4302: 4-byte units past the first 8) at `off`, or 0 when
// it does not fit the `have` bytes. Out of line: AH is rare, and inlined its
// code slowed the common walks (BM_Parse, D-188).
[[gnu::cold, gnu::noinline]] inline size_t AhLength(const uint8_t *ip, size_t have,
                                                    size_t off) noexcept {
  if (off + 8 > have) {
    return 0;
  }
  const size_t len = (size_t{ip[off + 1]} + 2) * 4;
  return off + len > have ? 0 : len;
}

// The IPv6 extension-header walk, from the fixed header of the packet at `ip`
// (`have` >= 40 bytes of it readable) to its transport: hop-by-hop, routing
// and destination options, fragment and AH, at most kMaxExtensionHeaders of
// them. The one walk ParseIp and conntrack's ICMP-error quote parse share.
// kOk: `out.protocol` is the transport protocol, at `off`. kFragment: a
// non-initial fragment, `out.protocol` the protocol it carries. kMalformed: a
// header cut short by `have`, or too many. An initial fragment header sets
// `out.first_fragment`. It writes `out` itself, as the loop it replaced in
// ParseIp did: returning those through out-parameters for ParseIp to copy and
// dispatch on changed GCC's layout of the whole inlined ParseFrame, the IPv4
// paths included (BM_Parse two-tag UDP +6.5%, D-188).
inline ParseStatus WalkIpv6Chain(const uint8_t *ip, size_t have, size_t &off,
                                 ParsedFrame &out) noexcept {
  uint8_t next = ip[6];
  off = 40;
  for (size_t n = 0;; n++) {
    if (n > kMaxExtensionHeaders) {
      return ParseStatus::kMalformed;
    }
    if (next < 64 && ((kOptionHeaders >> next) & 1u) != 0) {
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
      const uint16_t frag = Be16(ip + off + 2);
      const bool initial = (frag & 0xfff8u) == 0;
      next = ip[off];
      off += 8;
      if (!initial) {
        out.protocol = next;
        return ParseStatus::kFragment;
      }
      // An atomic fragment (offset 0, M clear) is a whole packet (RFC 6946).
      out.first_fragment = (frag & 0x0001u) != 0;
      continue;
    }
    if (next == 51) [[unlikely]] {
      const size_t len = AhLength(ip, have, off);
      if (len == 0) {
        return ParseStatus::kMalformed;
      }
      next = ip[off];
      off += len;
      continue;
    }
    break;
  }
  out.protocol = next;
  return ParseStatus::kOk;
}

// The L4 header checks, the one definition ParseL4 and CheckFastIpv4 share
// (kFill: also the fields, into `out`, in the same switch): `have` readable
// header bytes at `l4`, `room` the L4 length the IP header states. A first
// fragment needs only the fixed header (TCP options and the UDP datagram may
// continue in later fragments).
template <bool kFill>
inline ParseStatus CheckL4(uint8_t protocol, const uint8_t *l4, size_t have, size_t room,
                           bool first_fragment, ParsedFrame *out) noexcept {
  switch (protocol) {
    case 6:  // TCP
      if (have < 20 || (unsigned{l4[12]} >> 4u) < 5 ||
          (!first_fragment && static_cast<size_t>(unsigned{l4[12]} >> 4u) * 4 > room)) {
        return ParseStatus::kMalformed;
      }
      if constexpr (kFill) {
        out->l4 = L4Kind::kTcp;
        out->src_port = Be16(l4);
        out->dst_port = Be16(l4 + 2);
        out->tcp_flags = l4[13];
      }
      return ParseStatus::kOk;
    case 17:  // UDP: its length covers the whole datagram
      if (have < 8 || Be16(l4 + 4) < 8 || (!first_fragment && Be16(l4 + 4) > room)) {
        return ParseStatus::kMalformed;
      }
      if constexpr (kFill) {
        out->l4 = L4Kind::kUdp;
        out->src_port = Be16(l4);
        out->dst_port = Be16(l4 + 2);
      }
      return ParseStatus::kOk;
    case 1:   // ICMP
    case 58:  // ICMPv6
      if (have < 8) {
        return ParseStatus::kMalformed;
      }
      if constexpr (kFill) {
        out->l4 = protocol == 1 ? L4Kind::kIcmp : L4Kind::kIcmpv6;
        out->icmp_type = l4[0];
        out->icmp_code = l4[1];
        out->src_port = out->dst_port = Be16(l4 + 4);  // echo identifier
      }
      return ParseStatus::kOk;
    default:
      if constexpr (kFill) {
        out->l4 = L4Kind::kOther;
      }
      return ParseStatus::kOk;
  }
}

// L4 at `off` of the IP packet at `ip`: `present` bytes of it are readable
// (headers must lie there), `total` is its stated length (which the length
// checks use; for a chained packet the rest lies in later segments).
inline ParseStatus ParseL4(const uint8_t *ip, size_t present, size_t total, size_t off,
                           size_t frame_base, ParsedFrame &out) noexcept {
  out.l4_offset = static_cast<uint16_t>(frame_base + off);
  out.l4_length = static_cast<uint16_t>(total - off);
  const size_t have = present > off ? present - off : 0;  // readable header bytes
  return CheckL4<true>(out.protocol, ip + off, have, total - off, out.first_fragment, &out);
}

// An IP packet starting at `ip` (frame offset `base`): `present` bytes are
// readable, `limit` bytes are the packet's (present <= limit; they differ for
// a chained packet). Headers must be present; lengths are checked against
// `limit`.
inline ParseStatus ParseIp(const uint8_t *ip, size_t present, size_t limit, size_t base,
                           ParsedFrame &out) noexcept {
  if (present < 1) {
    return ParseStatus::kMalformed;
  }
  out.l3_offset = static_cast<uint16_t>(base);
  const unsigned version = unsigned{ip[0]} >> 4u;
  if (version == 4) {
    const size_t ihl = static_cast<size_t>(unsigned{ip[0]} & 0x0fu) * 4;
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
    if ((Be16(ip + 6) & 0x1fffu) != 0) {
      return ParseStatus::kFragment;
    }
    out.first_fragment = (Be16(ip + 6) & 0x2000u) != 0;  // MF set, offset 0
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
    size_t off = 0;
    const ParseStatus walk = WalkIpv6Chain(ip, have, off, out);
    if (walk != ParseStatus::kOk) {
      return walk;
    }
    return ParseL4(ip, have, total, off, base, out);
  }
  return ParseStatus::kMalformed;
}

// The general path: any of up to two VLAN tags, IPv4 with options, IPv6 with
// extension headers. `out` is cleared and `total_len` settled by the caller.
inline ParseStatus ParseFrameGeneral(std::span<const uint8_t> frame, ParsedFrame &out,
                                     size_t total_len) noexcept {
  size_t off = 12;
  if (frame.size() < 14) {
    return ParseStatus::kMalformed;
  }
  uint16_t type = Be16(frame.data() + off);
  for (int tags = 0; (type == 0x8100 || type == 0x88a8) && tags < 2; tags++) {
    off += 4;
    if (frame.size() < off + 2) {
      return ParseStatus::kMalformed;
    }
    type = Be16(frame.data() + off);
  }
  off += 2;
  if (type != 0x0800 && type != 0x86dd) {
    return ParseStatus::kNotIp;
  }
  const ParseStatus s = ParseIp(frame.data() + off, frame.size() - off, total_len - off, off, out);
  if (s == ParseStatus::kOk && ((type == 0x0800) != (out.l3 == L3Kind::kIpv4))) {
    return ParseStatus::kMalformed;  // EtherType and IP version disagree
  }
  return s;
}
}  // namespace parse_internal

// The common frame (D-085): untagged Ethernet carrying IPv4 without options,
// so every field sits at a fixed offset of the frame.
inline constexpr size_t kFastIpv4Src = 26;  // 4 bytes
inline constexpr size_t kFastIpv4Dst = 30;  // 4 bytes
inline constexpr size_t kFastIpv4L4 = 34;   // ports at +0 and +2

// CheckFastIpv4's verdict on a frame. `taken`: the frame is the common frame,
// and `status` is exactly what ParseFrame returns for it; otherwise ParseFrame
// takes its general path and the caller must too. `header_ok`: the IPv4
// header was accepted (then `protocol` and the addresses are valid, and the
// status is kOk, kFragment or a malformed L4 header).
struct FastIpv4 {
  bool taken = false;
  bool header_ok = false;
  bool first_fragment = false;
  uint8_t protocol = 0;
  uint16_t total = 0;  // the IPv4 total length
  ParseStatus status = ParseStatus::kMalformed;
};

namespace parse_internal {
// Untagged Ethernet, EtherType IPv4, version 4 with IHL 5, the fixed headers there.
inline bool IsCommonFrame(std::span<const uint8_t> frame) noexcept {
  return frame.size() >= 34 && frame[12] == 0x08 && frame[13] == 0x00 && frame[14] == 0x45;
}

// The common frame's checks, in the order that decides fastest; with kFill,
// ParseFrame's fields too (into `out`, which starts zeroed), as the checks
// reach them.
template <bool kFill>
inline FastIpv4 FastFrame(std::span<const uint8_t> frame, size_t total_len,
                          ParsedFrame *out) noexcept {
  FastIpv4 f;
  if (!IsCommonFrame(frame)) [[unlikely]] {
    return f;
  }
  f.taken = true;
  total_len = std::max(total_len, frame.size());
  const uint8_t *ip = frame.data() + 14;
  if constexpr (kFill) {
    out->l3_offset = 14;
  }
  const size_t total = Be16(ip + 2);
  if (total < 20 || total > total_len - 14) {
    return f;
  }
  f.header_ok = true;
  f.total = static_cast<uint16_t>(total);
  f.protocol = ip[9];
  if constexpr (kFill) {
    out->l3 = L3Kind::kIpv4;
    out->protocol = ip[9];
    std::memcpy(out->src.data(), ip + 12, 4);
    std::memcpy(out->dst.data(), ip + 16, 4);
  }
  const uint16_t frag = Be16(ip + 6);
  if ((frag & 0x1fffu) != 0) {
    f.status = ParseStatus::kFragment;
    return f;
  }
  f.first_fragment = (frag & 0x2000u) != 0;
  if constexpr (kFill) {
    out->first_fragment = f.first_fragment;
    out->l4_offset = 34;
    out->l4_length = static_cast<uint16_t>(total - 20);
  }
  // frame.size() >= 34 and total >= 20: at least the 20-byte header is here.
  f.status = CheckL4<kFill>(f.protocol, ip + 20, std::min(frame.size() - 14, total) - 20,
                            total - 20, f.first_fragment, out);
  return f;
}
}  // namespace parse_internal

// ParseFrame's checks for the common frame, the one definition ParseFrame
// itself runs (and conntrack_test checks against its general path), so a
// reader that needs only the addresses, protocol and ports can take them from
// the fixed offsets without building a ParsedFrame and decide exactly as the
// parse would. Arguments as ParseFrame's.
inline FastIpv4 CheckFastIpv4(std::span<const uint8_t> frame, size_t total_len = 0) noexcept {
  return parse_internal::FastFrame<false>(frame, total_len, nullptr);
}

// Parses an Ethernet frame (optionally with up to two 802.1Q/802.1ad tags).
// `frame` is the readable bytes (for a chained packet, its first segment) and
// `total_len` the packet's length (default: the span's): the headers must be
// in `frame`, and the IP and L4 lengths are checked against `total_len`.
inline ParseStatus ParseFrame(std::span<const uint8_t> frame, ParsedFrame &out,
                              size_t total_len = 0) noexcept {
  out = ParsedFrame{};
  // The common frame first, validated in one place: untagged Ethernet, IPv4
  // without options. The same checks as the general path below, in the order
  // that decides fastest (the tunnel review's "validate once, execute fast";
  // conntrack_test checks both paths agree on every frame it builds).
  if (parse_internal::IsCommonFrame(frame)) [[likely]] {
    return parse_internal::FastFrame<true>(frame, total_len, &out).status;
  }
  return parse_internal::ParseFrameGeneral(frame, out, std::max(total_len, frame.size()));
}

// Parses a packet that starts at its IP header.
inline ParseStatus ParseIpPacket(std::span<const uint8_t> packet, ParsedFrame &out) noexcept {
  out = ParsedFrame{};
  return parse_internal::ParseIp(packet.data(), packet.size(), packet.size(), 0, out);
}

}  // namespace bess::packet

#endif  // BESS_PACKET_PARSE_H_
