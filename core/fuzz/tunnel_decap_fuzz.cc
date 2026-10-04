// SPDX-License-Identifier: BSD-3-Clause

// Fuzz target: tunnel decapsulation and header codecs (tunnel/tunnel.h).
//
// The first input byte picks one of two stages (bit 0).
//
// Stage A, raw frames (bit 0 clear): [ports mode u8][port u16][total_len
// mode u8][k u16][GRE offset u8][frame: rest]. DecapVxlan (port: the default,
// the fuzzed one or none), DecapGeneve and DecapGtpu (the default or the
// fuzzed port), DecapGre and ParseGre (at the fuzzed offset) run on the frame
// with total_len 0, the frame's size, size - k or size + k. Oracle: each is
// deterministic (called twice), kMalformed exactly when ParseFrame refuses the
// outer packet or it is a first fragment, kNotTunnel (VXLAN, Geneve, GRE)
// exactly when the outer L4 or port is not the tunnel's; on kOk every offset
// is re-derived here from the header bytes -- the inner offset lies in the
// frame and before the outer L4 end, which lies in total_len; identifiers,
// protocol, flags, options, the GRE key/sequence/checksum (checked with an
// independent one's-complement sum) and the GTP-U optional fields and
// extension chain are the bytes at those offsets -- and the frame cut at the
// inner offset decapsulates the same (the inner bytes are never needed).
//
// Stage B, round trip (bit 0 set): [cfg u8][dst port u16][cut u16][protocol
// params][inner: rest, at most 2000 bytes]. cfg bits 0-1 pick VXLAN, Geneve,
// GRE or GTP-U, bit 2 an IPv6 outer, bit 3 a VLAN tag, bits 4-5 the port
// mode (written port standard or fuzzed; decap asked for it, for the standard
// one, or for any). The frame is built with WriteIpv4/WriteUdp/
// WriteVxlanUdp/WriteGeneve/WriteGre/WriteGtpu (+ GTP-U optional fields and
// an extension chain written here) around the inner bytes. Oracle: Decap*
// returns exactly the status the construction implies (kOk, kNoInner for an
// inner too short, kNotTunnel for another port or a GTP-U non-G-PDU), and on
// kOk recovers the VNI (24 bits), Geneve protocol/options/O/C bits, GRE
// protocol/key/sequence, GTP-U TEID/type/sequence/first extension, and the
// inner bytes at the inner offset up to the outer end; the raw-frame oracle
// holds too; the frame cut to `cut` bytes (total_len the whole) decapsulates
// the same iff the cut keeps the inner offset (GRE with a checksum: only
// uncut). WriteGeneve refuses exactly invalid option lengths. FlowEntropyPort
// is in 0xc000-0xffff and ignores every inner byte it does not hash.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "conntrack/packet_parse.h"
#include "fuzz/fuzz_support.h"
#include "tunnel/tunnel.h"

namespace bess::tunnel {
namespace {

using conntrack::L3Kind;
using conntrack::L4Kind;
using conntrack::ParsedFlowPacket;
using conntrack::ParseStatus;
using fuzz::FuzzInput;

uint16_t Get16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t *p) { return uint32_t{Get16(p)} << 16 | Get16(p + 2); }

bool SameOuter(const ParsedFlowPacket &a, const ParsedFlowPacket &b) {
  return a.l3 == b.l3 && a.l4 == b.l4 && a.protocol == b.protocol &&
         a.l3_offset == b.l3_offset && a.l4_offset == b.l4_offset &&
         a.l4_length == b.l4_length && a.src == b.src && a.dst == b.dst &&
         a.src_port == b.src_port && a.dst_port == b.dst_port &&
         a.first_fragment == b.first_fragment;
}
bool SameD(const Decapsulated &a, const Decapsulated &b) {
  return a.inner_offset == b.inner_offset && a.id == b.id && a.protocol == b.protocol;
}
bool SameG(const GeneveInfo &a, const GeneveInfo &b) {
  return SameD(a.tunnel, b.tunnel) && a.options_offset == b.options_offset &&
         a.options_bytes == b.options_bytes && a.oam == b.oam && a.critical == b.critical;
}
bool SameT(const GtpuInfo &a, const GtpuInfo &b) {
  return SameD(a.tunnel, b.tunnel) && a.message_type == b.message_type &&
         a.sequence == b.sequence && a.first_extension == b.first_extension;
}
bool SameGre(const GreInfo &a, const GreInfo &b) {
  return a.header_offset == b.header_offset && a.payload_offset == b.payload_offset &&
         a.protocol == b.protocol && a.key == b.key && a.sequence == b.sequence;
}

// RFC 1071 over big-endian words (odd tail padded): the bytes, checksum
// field included, sum to 0xffff.
bool ChecksumOk(std::span<const uint8_t> b) {
  uint64_t s = 0;
  size_t i = 0;
  for (; i + 1 < b.size(); i += 2) s += Get16(b.data() + i);
  if (i < b.size()) s += uint32_t{b[i]} << 8;
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  return s == 0xffff;
}

// What every decapsulation does first: the outer parse. kMalformed exactly
// when it is refused or a first fragment.
struct OuterView {
  bool ok;
  ParsedFlowPacket p;
  size_t end;  // the end of the outer L4 payload
};
OuterView ParseOuter(std::span<const uint8_t> frame, size_t total_len) {
  OuterView v{};
  v.ok = conntrack::ParseFrame(frame, v.p, total_len) == ParseStatus::kOk && !v.p.first_fragment;
  v.end = size_t{v.p.l4_offset} + v.p.l4_length;
  return v;
}

// -- stage A: the raw-frame oracle -------------------------------------------------

void CheckVxlan(std::span<const uint8_t> frame, size_t total_len, size_t total,
                const OuterView &ov, std::optional<uint16_t> port) {
  ParsedFlowPacket o1, o2;
  Decapsulated d1{}, d2{};
  const DecapError s = DecapVxlan(frame, o1, d1, port, total_len);
  FUZZ_CHECK(DecapVxlan(frame, o2, d2, port, total_len) == s);
  FUZZ_CHECK((s == DecapError::kMalformed) == !ov.ok);
  if (!ov.ok) {
    return;
  }
  FUZZ_CHECK(SameOuter(o1, ov.p) && SameOuter(o2, ov.p));
  FUZZ_CHECK((s == DecapError::kNotTunnel) ==
             (ov.p.l4 != L4Kind::kUdp || (port && ov.p.dst_port != *port)));
  if (s != DecapError::kOk) {
    return;
  }
  FUZZ_CHECK(SameD(d1, d2));
  const size_t l4 = ov.p.l4_offset;
  FUZZ_CHECK(ov.end <= total && d1.inner_offset == l4 + 16 && d1.inner_offset <= frame.size());
  FUZZ_CHECK(d1.inner_offset + kEthernetBytes <= ov.end);
  FUZZ_CHECK((frame[l4 + 8] & 0x08) != 0);
  FUZZ_CHECK(d1.id == Get32(frame.data() + l4 + 12) >> 8 && d1.id < (1u << 24));
  FUZZ_CHECK(d1.protocol == 0x6558);
  ParsedFlowPacket o3;
  Decapsulated d3{};
  FUZZ_CHECK(DecapVxlan(frame.first(d1.inner_offset), o3, d3, port, total) == DecapError::kOk &&
             SameD(d1, d3) && SameOuter(o3, ov.p));
  FUZZ_CHECK(DecapVxlan(frame, o3, d3, std::nullopt, total_len) == DecapError::kOk &&
             SameD(d1, d3));
}

void CheckGeneve(std::span<const uint8_t> frame, size_t total_len, size_t total,
                 const OuterView &ov, uint16_t port) {
  ParsedFlowPacket o1, o2;
  GeneveInfo g1{}, g2{};
  const DecapError s = DecapGeneve(frame, o1, g1, port, total_len);
  FUZZ_CHECK(DecapGeneve(frame, o2, g2, port, total_len) == s);
  FUZZ_CHECK((s == DecapError::kMalformed) == !ov.ok);
  if (!ov.ok) {
    return;
  }
  FUZZ_CHECK(SameOuter(o1, ov.p));
  FUZZ_CHECK((s == DecapError::kNotTunnel) ==
             (ov.p.l4 != L4Kind::kUdp || ov.p.dst_port != port));
  if (s != DecapError::kOk) {
    return;
  }
  FUZZ_CHECK(SameG(g1, g2));
  const size_t g = size_t{ov.p.l4_offset} + 8;
  FUZZ_CHECK(ov.end <= total && g + 8 <= frame.size() && (frame[g] >> 6) == 0);
  FUZZ_CHECK(g1.options_offset == g + 8 && g1.options_bytes == (frame[g] & 0x3f) * 4);
  FUZZ_CHECK(g1.oam == ((frame[g + 1] & 0x80) != 0) && g1.critical == ((frame[g + 1] & 0x40) != 0));
  const size_t inner = g + 8 + g1.options_bytes;
  FUZZ_CHECK(g1.tunnel.inner_offset == inner && inner <= frame.size() && inner <= ov.end);
  FUZZ_CHECK(g1.tunnel.id == Get32(frame.data() + g + 4) >> 8 &&
             g1.tunnel.protocol == Get16(frame.data() + g + 2));
  FUZZ_CHECK(g1.tunnel.protocol != 0x6558 || inner + kEthernetBytes <= ov.end);
  ParsedFlowPacket o3;
  GeneveInfo g3{};
  FUZZ_CHECK(DecapGeneve(frame.first(inner), o3, g3, port, total) == DecapError::kOk &&
             SameG(g1, g3));
}

// The GRE header at `off`: flags, then the optional fields in C, K, S order.
size_t GreHeaderBytes(uint16_t flags) {
  return 4 + ((flags & 0x8000) ? 4 : 0) + ((flags & 0x2000) ? 4 : 0) + ((flags & 0x1000) ? 4 : 0);
}

void CheckGreFields(std::span<const uint8_t> bytes, size_t off, const GreInfo &gi) {
  const uint16_t flags = Get16(bytes.data() + off);
  FUZZ_CHECK((flags & 0x4c07) == 0 && gi.header_offset == off);
  FUZZ_CHECK(gi.payload_offset == off + GreHeaderBytes(flags) && gi.payload_offset <= bytes.size());
  FUZZ_CHECK(gi.protocol == Get16(bytes.data() + off + 2));
  size_t at = off + 4 + ((flags & 0x8000) ? 4 : 0);
  if (flags & 0x2000) {
    FUZZ_CHECK(gi.key == Get32(bytes.data() + at));
    at += 4;
  } else {
    FUZZ_CHECK(!gi.key);
  }
  if (flags & 0x1000) {
    FUZZ_CHECK(gi.sequence == Get32(bytes.data() + at));
  } else {
    FUZZ_CHECK(!gi.sequence);
  }
  if (flags & 0x8000) {
    FUZZ_CHECK(ChecksumOk(bytes.subspan(off)));
  }
}

void CheckParseGre(std::span<const uint8_t> frame, size_t off) {
  GreInfo a{}, b{};
  const DecapError s = ParseGre(frame, off, a);
  FUZZ_CHECK(ParseGre(frame, off, b) == s);
  FUZZ_CHECK(s == DecapError::kOk || s == DecapError::kBadHeader);
  if (s == DecapError::kOk) {
    FUZZ_CHECK(SameGre(a, b));
    CheckGreFields(frame, off, a);
  }
}

void CheckGre(std::span<const uint8_t> frame, size_t total_len, size_t total,
              const OuterView &ov) {
  ParsedFlowPacket o1, o2;
  Decapsulated d1{}, d2{};
  const DecapError s = DecapGre(frame, o1, d1, total_len);
  FUZZ_CHECK(DecapGre(frame, o2, d2, total_len) == s);
  FUZZ_CHECK((s == DecapError::kMalformed) == !ov.ok);
  if (!ov.ok) {
    return;
  }
  FUZZ_CHECK(SameOuter(o1, ov.p));
  FUZZ_CHECK((s == DecapError::kNotTunnel) == (ov.p.protocol != 47));
  if (s != DecapError::kOk) {
    return;
  }
  FUZZ_CHECK(SameD(d1, d2));
  const size_t l4 = ov.p.l4_offset;
  const std::span<const uint8_t> gre = frame.first(std::min(ov.end, frame.size()));
  FUZZ_CHECK(ov.end <= total && l4 + 4 <= gre.size());
  const uint16_t flags = Get16(frame.data() + l4);
  const size_t inner = l4 + GreHeaderBytes(flags);
  FUZZ_CHECK(d1.inner_offset == inner && inner < ov.end && inner <= frame.size());
  FUZZ_CHECK(d1.protocol == Get16(frame.data() + l4 + 2));
  FUZZ_CHECK(d1.id == ((flags & 0x2000) ? Get32(frame.data() + l4 + 4 + ((flags & 0x8000) ? 4 : 0))
                                        : 0u));
  // A checksum covers the whole GRE packet, so it must all be readable.
  FUZZ_CHECK(!(flags & 0x8000) || ov.end <= frame.size());
  GreInfo gi{};
  FUZZ_CHECK(ParseGre(gre, l4, gi) == DecapError::kOk);
  CheckGreFields(gre, l4, gi);
  if (!(flags & 0x8000)) {
    ParsedFlowPacket o3;
    Decapsulated d3{};
    FUZZ_CHECK(DecapGre(frame.first(inner), o3, d3, total) == DecapError::kOk && SameD(d1, d3));
  }
}

void CheckGtpu(std::span<const uint8_t> frame, size_t total_len, size_t total,
               const OuterView &ov, uint16_t port) {
  ParsedFlowPacket o1, o2;
  GtpuInfo t1{}, t2{};
  const DecapError s = DecapGtpu(frame, o1, t1, port, total_len);
  FUZZ_CHECK(DecapGtpu(frame, o2, t2, port, total_len) == s);
  FUZZ_CHECK((s == DecapError::kMalformed) == !ov.ok);
  if (!ov.ok) {
    return;
  }
  FUZZ_CHECK(SameOuter(o1, ov.p));
  if (ov.p.l4 != L4Kind::kUdp || ov.p.dst_port != port) {
    FUZZ_CHECK(s == DecapError::kNotTunnel);
    return;
  }
  if (s != DecapError::kOk) {
    return;
  }
  FUZZ_CHECK(SameT(t1, t2));
  const size_t g = size_t{ov.p.l4_offset} + 8;
  FUZZ_CHECK(g + 8 <= frame.size() && (frame[g] >> 5) == 1 && (frame[g] & 0x10) != 0);
  FUZZ_CHECK(t1.message_type == frame[g + 1] && t1.message_type == kGtpuGpdu);
  const size_t stated_end = g + 8 + Get16(frame.data() + g + 2);
  FUZZ_CHECK(stated_end <= ov.end && ov.end <= total);
  FUZZ_CHECK(t1.tunnel.id == Get32(frame.data() + g + 4) && t1.tunnel.protocol == 0);
  // The optional fields and the extension chain, walked here.
  const uint8_t flags = frame[g] & 0x07;
  size_t off = g + 8;
  std::optional<uint16_t> seq;
  std::optional<uint8_t> first;
  if (flags != 0) {
    FUZZ_CHECK(off + 4 <= frame.size() && off + 4 <= stated_end);
    if (flags & 0x02) seq = Get16(frame.data() + off);
    uint8_t next = frame[off + 3];
    off += 4;
    if ((flags & 0x04) && next != 0) {
      first = next;
      while (next != 0) {
        FUZZ_CHECK(off < frame.size() && frame[off] != 0);
        const size_t len = size_t{frame[off]} * 4;
        FUZZ_CHECK(off + len <= frame.size() && off + len <= stated_end);
        next = frame[off + len - 1];
        off += len;
      }
    }
  }
  FUZZ_CHECK(t1.sequence == seq && t1.first_extension == first);
  FUZZ_CHECK(t1.tunnel.inner_offset == off && off < stated_end && off <= frame.size());
  ParsedFlowPacket o3;
  GtpuInfo t3{};
  FUZZ_CHECK(DecapGtpu(frame.first(off), o3, t3, port, total) == DecapError::kOk &&
             SameT(t1, t3));
}

struct RawPorts {
  std::optional<uint16_t> vxlan;
  uint16_t geneve;
  uint16_t gtpu;
};

void CheckRaw(std::span<const uint8_t> frame, size_t total_len, const RawPorts &ports,
              size_t gre_offset) {
  const size_t total = std::max(total_len, frame.size());
  const OuterView ov = ParseOuter(frame, total_len);
  CheckVxlan(frame, total_len, total, ov, ports.vxlan);
  CheckGeneve(frame, total_len, total, ov, ports.geneve);
  CheckGre(frame, total_len, total, ov);
  CheckGtpu(frame, total_len, total, ov, ports.gtpu);
  CheckParseGre(frame, gre_offset);
}

// TODO(M21 checksum.h): remove 4-byte alignment workaround
// utils/checksum.h loads header words through misaligned pointers, which
// UBSan reports (known M22 finding; reproducers in .scratch/m22/b/tunnel_decap/):
// CalculateIpv4NoOptChecksum, reached from WriteIpv4, on an IPv4 header at
// Ethernet offset 14/18 (ubsan_misaligned_ipv4_checksum.bin), and
// CalculateSum's 16-bit loop, reached from the GRE checksum, on a GRE header
// at an odd address (ubsan_misaligned_calculate_sum_odd_base.bin). While true:
// a raw frame is copied so its base is 2 mod 4 (its IPv4 header 4-byte
// aligned, its GRE header even), ParseGre's fuzzed offset is even, and the
// round trip writes the outer IPv4 header 4-byte aligned and copies it in.
// False: frames in place, Ethernet-framed, any offset.
constexpr bool kAlignForChecksum = true;

void RunRaw(FuzzInput &in) {
  const uint8_t mode = in.U8();
  const uint16_t port = in.U16();
  const uint8_t tl_mode = in.U8();
  const uint16_t k = in.U16();
  size_t gre_offset = in.U8();
  std::span<const uint8_t> frame = in.Rest();
  std::vector<uint8_t> storage;
  if (kAlignForChecksum) {
    storage.resize(frame.size() + 2);
    std::copy(frame.begin(), frame.end(), storage.begin() + 2);
    frame = std::span<const uint8_t>(storage).subspan(2);
    gre_offset &= ~size_t{1};
  }
  RawPorts ports;
  switch (mode & 3) {
    case 0:
      ports.vxlan = kVxlanPort;
      break;
    case 1:
      ports.vxlan = port;
      break;
    default:
      ports.vxlan = std::nullopt;
      break;
  }
  ports.geneve = (mode & 4) ? port : kGenevePort;
  ports.gtpu = (mode & 8) ? port : kGtpuPort;
  size_t total_len = 0;
  switch (tl_mode % 4) {
    case 0:
      break;
    case 1:
      total_len = frame.size();
      break;
    case 2:
      total_len = frame.size() - std::min<size_t>(k, frame.size());
      break;
    default:
      total_len = frame.size() + k;
      break;
  }
  CheckRaw(frame, total_len, ports, gre_offset);
}

// -- stage B: round trip -------------------------------------------------------------

// FlowEntropyPort is in the dynamic range and a function of the bytes it
// hashes only: the MACs of a non-IPv4 frame; else the protocol, addresses and
// (TCP/UDP, unfragmented) ports. Every other byte is scrambled here.
void CheckEntropyPort(std::span<const uint8_t> inner) {
  const uint16_t port = FlowEntropyPort(inner);
  FUZZ_CHECK(port >= 0xc000);
  if (inner.size() < 12) {
    FUZZ_CHECK(port == 0xc000);
    return;
  }
  std::vector<uint8_t> keep(inner.size(), 0);
  const bool ipv4 = inner.size() >= 34 && Get16(inner.data() + 12) == 0x0800;
  for (size_t i = 12; i < 14 && i < inner.size(); i++) keep[i] = 1;
  if (!ipv4) {
    std::fill(keep.begin(), keep.begin() + 12, 1);
  } else {
    for (size_t i : {14u, 20u, 21u, 23u}) keep[i] = 1;
    std::fill(keep.begin() + 26, keep.begin() + 34, 1);
    const size_t l4 = 14 + size_t{inner[14] & 0x0fu} * 4;
    for (size_t i = l4; i < l4 + 4 && i < inner.size(); i++) keep[i] = 1;
  }
  std::vector<uint8_t> scrambled(inner.begin(), inner.end());
  for (size_t i = 0; i < scrambled.size(); i++) {
    if (!keep[i]) scrambled[i] ^= static_cast<uint8_t>(0xa5 + i);
  }
  FUZZ_CHECK(FlowEntropyPort(scrambled) == port);
}

enum Proto : unsigned { kVxlan, kGeneve, kGre, kGtpu };

struct Frame {
  std::vector<uint8_t> bytes;
  size_t l4 = 0;  // the outer L4 offset
};

// Outer Ethernet (+ VLAN) + IPv4/IPv6 around the L4 bytes `l4`.
Frame Outer(bool v6, bool vlan, uint8_t protocol, const std::vector<uint8_t> &l4) {
  Frame f;
  f.bytes = {0x02, 0, 0, 0, 0, 0x02, 0x02, 0, 0, 0, 0, 0x01};
  if (vlan) {
    f.bytes.insert(f.bytes.end(), {0x81, 0x00, 0x00, 0x07});
  }
  f.bytes.push_back(v6 ? 0x86 : 0x08);
  f.bytes.push_back(v6 ? 0xdd : 0x00);
  const size_t ip = f.bytes.size();
  if (!v6) {
    if (kAlignForChecksum) {
      alignas(4) std::array<uint8_t, kIpv4Bytes> h;
      WriteIpv4(h.data(), utils::be32_t(0xc0000201), utils::be32_t(0xc0000202), protocol,
                static_cast<uint16_t>(l4.size()));
      f.bytes.insert(f.bytes.end(), h.begin(), h.end());
    } else {
      f.bytes.resize(ip + kIpv4Bytes);
      WriteIpv4(f.bytes.data() + ip, utils::be32_t(0xc0000201), utils::be32_t(0xc0000202),
                protocol, static_cast<uint16_t>(l4.size()));
    }
  } else {
    f.bytes.resize(ip + kIpv6Bytes, 0);
    uint8_t *h = f.bytes.data() + ip;
    h[0] = 0x60;
    detail::Put16(h + 4, static_cast<uint16_t>(l4.size()));
    h[6] = protocol;
    h[7] = 64;
    h[8] = 0x20;
    h[9] = 0x01;
    h[23] = 1;
    h[24] = 0x20;
    h[25] = 0x01;
    h[39] = 2;
  }
  f.l4 = f.bytes.size();
  f.bytes.insert(f.bytes.end(), l4.begin(), l4.end());
  return f;
}

// Decapsulates `frame` cut to `cut` bytes with total_len the whole frame
// (`decap` checks a success gives the same result): it succeeds iff the cut
// keeps the inner offset (with `needs_all`, only uncut).
template <typename Decap>
void CheckCut(std::span<const uint8_t> frame, size_t cut, size_t inner, bool needs_all,
              Decap decap) {
  const bool keeps = needs_all ? cut == frame.size() : cut >= inner;
  FUZZ_CHECK(decap(frame.first(cut), frame.size()) == keeps);
}

// The inner bytes are at the decapsulated offset and run to the outer end.
void CheckInner(std::span<const uint8_t> frame, const ParsedFlowPacket &outer, size_t offset,
                std::span<const uint8_t> inner) {
  FUZZ_CHECK(size_t{outer.l4_offset} + outer.l4_length == offset + inner.size());
  FUZZ_CHECK(offset + inner.size() <= frame.size() &&
             std::equal(inner.begin(), inner.end(), frame.begin() + offset));
}

void RunRoundTrip(FuzzInput &in) {
  const uint8_t cfg = in.U8();
  const unsigned proto = cfg & 3;
  const bool v6 = (cfg & 4) != 0;
  const bool vlan = (cfg & 8) != 0;
  const unsigned port_mode = (cfg >> 4) & 3;
  const uint16_t fuzzed_port = in.U16();
  const uint16_t cut_in = in.U16();

  // Protocol parameters (read before the inner bytes, which take the rest).
  const uint32_t id = in.U32();
  const uint8_t p1 = in.U8();
  const uint16_t p2 = in.U16();
  const uint32_t p3 = in.U32();
  std::vector<uint8_t> extra;  // Geneve options / GTP-U extension contents
  if (proto == kGeneve || proto == kGtpu) {
    const auto e = in.Bytes(in.U8());
    extra.assign(e.begin(), e.end());
  }
  const auto rest = in.Rest();
  const std::span<const uint8_t> inner = rest.first(std::min<size_t>(rest.size(), 2000));
  CheckEntropyPort(inner);

  const uint16_t standard = proto == kVxlan ? kVxlanPort : proto == kGeneve ? kGenevePort
                                                                           : kGtpuPort;
  // Written port and the port decap is asked for (VXLAN may ask for none).
  const uint16_t written = (port_mode & 1) ? fuzzed_port : standard;
  const bool ask_written = (port_mode & 2) == 0;
  const uint16_t asked = ask_written ? written : standard;
  const bool port_matches = written == asked;

  std::vector<uint8_t> l4;
  Frame f;
  size_t inner_offset = 0;
  DecapError expect = DecapError::kOk;
  bool needs_all = false;
  RawPorts ports{kVxlanPort, kGenevePort, kGtpuPort};
  switch (proto) {
    case kVxlan: {
      l4.resize(kUdpBytes + kVxlanBytes);
      WriteVxlanUdp(l4.data(), id, FlowEntropyPort(inner), written, inner.size());
      l4.insert(l4.end(), inner.begin(), inner.end());
      f = Outer(v6, vlan, 17, l4);
      inner_offset = f.l4 + 16;
      const std::optional<uint16_t> ask =
          port_mode == 3 ? std::nullopt : std::optional<uint16_t>(asked);
      ports.vxlan = ask;
      if (ask && !port_matches) {
        expect = DecapError::kNotTunnel;
      } else if (inner.size() < kEthernetBytes) {
        expect = DecapError::kNoInner;
      }
      ParsedFlowPacket o;
      Decapsulated d{};
      const DecapError s = DecapVxlan(f.bytes, o, d, ask);
      FUZZ_CHECK(s == expect);
      if (s == DecapError::kOk) {
        FUZZ_CHECK(d.id == (id & 0xffffff) && d.protocol == 0x6558 && d.inner_offset == inner_offset);
        FUZZ_CHECK(o.dst_port == written && o.src_port == FlowEntropyPort(inner));
        CheckInner(f.bytes, o, d.inner_offset, inner);
        CheckCut(f.bytes, cut_in % (f.bytes.size() + 1), inner_offset, false,
                 [&](std::span<const uint8_t> b, size_t total) {
                   Decapsulated d2{};
                   ParsedFlowPacket o2;
                   const bool ok = DecapVxlan(b, o2, d2, ask, total) == DecapError::kOk;
                   FUZZ_CHECK(!ok || SameD(d, d2));
                   return ok;
                 });
      }
      break;
    }
    case kGeneve: {
      const size_t option_bytes = p1;
      const bool oam = (p2 & 1) != 0;
      const bool critical = (p2 & 2) != 0;
      static constexpr uint16_t kProtocols[4] = {0, 0x6558, 0x0800, 0x86dd};
      const uint16_t protocol = (p2 >> 2) & 3 ? kProtocols[(p2 >> 2) & 3] : static_cast<uint16_t>(p3);
      std::vector<uint8_t> hdr(kGeneveBaseBytes);
      const bool wrote = WriteGeneve(hdr.data(), id, protocol, option_bytes, oam, critical);
      FUZZ_CHECK(wrote == (option_bytes % 4 == 0 && option_bytes <= 252));
      if (!wrote) {
        return;
      }
      std::vector<uint8_t> options(option_bytes, 0);
      std::copy_n(extra.begin(), std::min(extra.size(), option_bytes), options.begin());
      l4.resize(kUdpBytes);
      const size_t payload = kGeneveBaseBytes + option_bytes + inner.size();
      WriteUdp(l4.data(), 0xc123, written, static_cast<uint16_t>(payload));
      l4.insert(l4.end(), hdr.begin(), hdr.end());
      l4.insert(l4.end(), options.begin(), options.end());
      l4.insert(l4.end(), inner.begin(), inner.end());
      f = Outer(v6, vlan, 17, l4);
      inner_offset = f.l4 + 16 + option_bytes;
      ports.geneve = asked;
      if (!port_matches) {
        expect = DecapError::kNotTunnel;
      } else if (protocol == 0x6558 && inner.size() < kEthernetBytes) {
        expect = DecapError::kNoInner;
      }
      ParsedFlowPacket o;
      GeneveInfo g{};
      const DecapError s = DecapGeneve(f.bytes, o, g, asked);
      FUZZ_CHECK(s == expect);
      if (s == DecapError::kOk) {
        FUZZ_CHECK(g.tunnel.id == (id & 0xffffff) && g.tunnel.protocol == protocol &&
                   g.tunnel.inner_offset == inner_offset);
        FUZZ_CHECK(g.options_offset == f.l4 + 16 && g.options_bytes == option_bytes &&
                   g.oam == oam && g.critical == critical);
        FUZZ_CHECK(std::equal(options.begin(), options.end(),
                              f.bytes.begin() + g.options_offset));
        CheckInner(f.bytes, o, g.tunnel.inner_offset, inner);
        CheckCut(f.bytes, cut_in % (f.bytes.size() + 1), inner_offset, false,
                 [&](std::span<const uint8_t> b, size_t total) {
                   GeneveInfo g2{};
                   ParsedFlowPacket o2;
                   const bool ok = DecapGeneve(b, o2, g2, asked, total) == DecapError::kOk;
                   FUZZ_CHECK(!ok || SameG(g, g2));
                   return ok;
                 });
      }
      break;
    }
    case kGre: {
      GreOptions opt;
      if (p1 & 1) opt.key = id;
      if (p1 & 2) opt.sequence = p3;
      opt.checksum = (p1 & 4) != 0;
      const uint16_t protocol = p2;
      const size_t hdr = GreBytes(opt);
      l4.resize(hdr);
      l4.insert(l4.end(), inner.begin(), inner.end());
      FUZZ_CHECK(WriteGre(l4.data(), protocol, opt, std::span<const uint8_t>(l4).subspan(hdr)) ==
                 hdr);
      f = Outer(v6, vlan, 47, l4);
      inner_offset = f.l4 + hdr;
      needs_all = opt.checksum;
      if (inner.empty()) {
        expect = DecapError::kNoInner;
      }
      ParsedFlowPacket o;
      Decapsulated d{};
      const DecapError s = DecapGre(f.bytes, o, d);
      FUZZ_CHECK(s == expect);
      GreInfo gi{};
      FUZZ_CHECK(ParseGre(f.bytes, f.l4, gi) == DecapError::kOk);
      FUZZ_CHECK(gi.header_offset == f.l4 && gi.payload_offset == inner_offset &&
                 gi.protocol == protocol && gi.key == opt.key && gi.sequence == opt.sequence);
      if (s == DecapError::kOk) {
        FUZZ_CHECK(d.id == opt.key.value_or(0) && d.protocol == protocol &&
                   d.inner_offset == inner_offset);
        CheckInner(f.bytes, o, d.inner_offset, inner);
        CheckCut(f.bytes, cut_in % (f.bytes.size() + 1), inner_offset, needs_all,
                 [&](std::span<const uint8_t> b, size_t total) {
                   Decapsulated d2{};
                   ParsedFlowPacket o2;
                   const bool ok = DecapGre(b, o2, d2, total) == DecapError::kOk;
                   FUZZ_CHECK(!ok || SameD(d, d2));
                   return ok;
                 });
      }
      break;
    }
    default: {  // kGtpu
      const uint8_t flags = p1 & 0x07;
      const bool other_type = (p1 & 0x08) != 0;
      const uint8_t type = other_type ? static_cast<uint8_t>(p1 >> 4) : kGtpuGpdu;  // < 16
      const uint16_t sequence = p2;
      // Optional fields and an extension chain: up to three headers of
      // 1-3 units, types (non-zero) and contents from `extra`.
      std::vector<uint8_t> opt;
      std::optional<uint8_t> first;
      if (flags != 0) {
        const unsigned n_ext = (flags & 0x04) ? (p3 >> 8) % 4 : 0;
        std::array<uint8_t, 3> types{};
        for (unsigned i = 0; i < n_ext; i++) {
          types[i] = static_cast<uint8_t>((p3 >> (16 + 5 * i)) | 1);
        }
        opt.push_back(static_cast<uint8_t>(sequence >> 8));
        opt.push_back(static_cast<uint8_t>(sequence));
        opt.push_back(static_cast<uint8_t>(p3));  // N-PDU number
        // Without E the next-type byte means nothing and is ignored.
        opt.push_back(n_ext > 0 ? types[0] : (flags & 0x04) ? 0 : static_cast<uint8_t>(p3 >> 24));
        if (n_ext > 0) first = types[0];
        size_t e = 0;
        for (unsigned i = 0; i < n_ext; i++) {
          const size_t units = 1 + (p3 >> (2 * i)) % 3;
          opt.push_back(static_cast<uint8_t>(units));
          for (size_t j = 0; j + 2 < units * 4; j++) {
            opt.push_back(e < extra.size() ? extra[e++] : 0);
          }
          opt.push_back(i + 1 < n_ext ? types[i + 1] : 0);
        }
      }
      const size_t after = opt.size() + inner.size();  // after the mandatory 8 bytes
      std::vector<uint8_t> g(kGtpuBaseBytes);
      WriteGtpu(g.data(), id, static_cast<uint16_t>(after));
      g[0] |= flags;
      g[1] = type;
      l4.resize(kUdpBytes);
      WriteUdp(l4.data(), 0xc123, written, static_cast<uint16_t>(kGtpuBaseBytes + after));
      l4.insert(l4.end(), g.begin(), g.end());
      l4.insert(l4.end(), opt.begin(), opt.end());
      l4.insert(l4.end(), inner.begin(), inner.end());
      f = Outer(v6, vlan, 17, l4);
      inner_offset = f.l4 + 16 + opt.size();
      ports.gtpu = asked;
      if (!port_matches || other_type) {
        expect = DecapError::kNotTunnel;
      } else if (inner.empty()) {
        expect = DecapError::kNoInner;
      }
      ParsedFlowPacket o;
      GtpuInfo t{};
      const DecapError s = DecapGtpu(f.bytes, o, t, asked);
      FUZZ_CHECK(s == expect);
      if (port_matches) {
        FUZZ_CHECK(t.message_type == type && t.tunnel.id == id);
        FUZZ_CHECK(t.sequence == ((flags & 0x02) ? std::optional<uint16_t>(sequence) : std::nullopt));
        FUZZ_CHECK(t.first_extension == first);
      }
      if (s == DecapError::kOk) {
        FUZZ_CHECK(t.tunnel.inner_offset == inner_offset && t.tunnel.protocol == 0);
        CheckInner(f.bytes, o, t.tunnel.inner_offset, inner);
        CheckCut(f.bytes, cut_in % (f.bytes.size() + 1), inner_offset, false,
                 [&](std::span<const uint8_t> b, size_t total) {
                   GtpuInfo t2{};
                   ParsedFlowPacket o2;
                   const bool ok = DecapGtpu(b, o2, t2, asked, total) == DecapError::kOk;
                   FUZZ_CHECK(!ok || SameT(t, t2));
                   return ok;
                 });
      }
      break;
    }
  }
  CheckRaw(f.bytes, 0, ports, f.l4);
}

}  // namespace
}  // namespace bess::tunnel

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  bess::fuzz::FuzzInput in(data, size);
  if ((in.U8() & 1) == 0) {
    bess::tunnel::RunRaw(in);
  } else {
    bess::tunnel::RunRoundTrip(in);
  }
  return 0;
}
