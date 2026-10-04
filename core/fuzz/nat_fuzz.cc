// SPDX-License-Identifier: BSD-3-Clause

// Fuzzes the NAT engine (core/nat/nat.h): Nat::Translate, Nat::TranslateBatch,
// Nat::Expire and, through them, Rewrite, the port pool and the binding table,
// on frames parsed by conntrack::ParseFrame.
//
// Each input builds two identical Nats (one of three small fixed address
// configurations, capacity 1-16, timeout 1000 ticks, wheel granularity 2^0 or
// 2^3, seeded port choice) and runs a sequence of steps against them.
//
// Input:
//   u8  config: bits 0-7 % 3 select the addresses, bit 7 the granularity 2^3
//   u8  capacity - 1 (mod 16)
//   u16 port-choice seed
//   then up to 16 steps:
//     u8  ctl: bit 0 reverse (inbound), bits 1-3 frames - 1, bit 4 expire,
//              bit 5 a u16 tick delta (else u8)
//     u8|u16 delta added to the clock
//     [u8 budget, if expire: 0xff unbounded, else budget & 15]
//     frames, each:
//       u8 kind % 4
//         0 raw:      u16 length % 601, that many bytes
//         1 IPv4:     u8 proto (% 4: TCP, UDP, ICMP, other = aux), u8 aux (TCP
//                     flags / ICMP type index / IP protocol), src, u16 sport,
//                     dst, u16 dport, u8 payload length % 64, u8 opts
//                     [u8 cut if opts bit 6] [u8 pad if opts bit 7]
//         2 binding:  u8 sel, u8 binding index, remote, u16 remote port,
//                     u8 payload length, u8 opts [cut] [pad]: a packet built
//                     from a live mapping of the model (sel % 4: reply to its
//                     external endpoint, outbound from its internal one,
//                     outbound from its external one, inbound to its internal
//                     one); an IPv4 template when there is none
//         3 IPv6:     u8 proto (% 3: TCP, UDP, ICMPv6), u16 sport, u16 dport,
//                     u8 payload length
//       u8 split: nonzero and shorter than the frame: the frame is a chained
//          packet whose first segment is that many bytes (total_len = all)
//     An address is u8 index % 8 into a palette of inside, external and remote
//     addresses; index 7 reads a raw u32. opts: bit 0 VLAN tag, 1 IP options,
//     2 UDP checksum 0, 3 bad IP checksum, 4 bad L4 checksum, 5 MF (first
//     fragment), 6 cut bytes off the end, 7 Ethernet padding.
//
// Every frame (and every first segment) is an exact-size heap vector, so
// ASan proves Translate writes nothing outside it. Oracles, per step:
//   - Differential: Nat A translates the step's frames one by one, Nat B gets
//     copies through TranslateBatch; verdicts, bytes and table sizes agree.
//   - Reference model: a list of live mappings (internal, external, last
//     outbound tick) predicts every verdict exactly (kNotIpv4, kUnsupported,
//     kNoBinding, kPortZero, kFull at capacity, kExhausted when no usable
//     range of the hashed address has a free port of the class, kConflict for
//     an outbound packet from a mapping's external endpoint or a new external
//     endpoint that is a live internal one) and the translation: the same
//     external endpoint for a live internal one (EIM), a new one on the
//     hashed address in the first range with free ports, within the port class
//     span, unused. Both Nats' tables hold exactly the model's mappings.
//   - Rewrite: an untranslated frame is unchanged; a translated one changes
//     only the IPv4 checksum, the rewritten address and port/identifier and
//     the L4 checksum (at the parser's offsets), to the model's endpoint. A
//     checksum valid before (IPv4 header; L4 over the IP payload with the
//     pseudo header; a UDP checksum 0 stays 0) is valid after.
//   - Round trip: each translated frame's reply (addresses and ports
//     swapped, payload kept) is translated in the other direction on both
//     Nats and must come back as exactly the original frame swapped (checksum
//     fields equal up to the one's complement zero, 0x0000 = 0xffff).
//   - Expire(now, budget): a mapping with an outbound packet within the
//     timeout is never removed; with an unbounded budget every mapping due by
//     the wheel's granularity is removed; the count returned is the number of
//     mappings gone. After that, its replies get kNoBinding (model).

#include <rte_config.h>
#include <rte_hash_crc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "conntrack/packet_parse.h"
#include "fuzz/fuzz_support.h"
#include "nat/nat.h"

namespace {

using bess::conntrack::L3Kind;
using bess::conntrack::L4Kind;
using bess::conntrack::ParsedFlowPacket;
using bess::conntrack::ParseFrame;
using bess::conntrack::ParseStatus;
using bess::fuzz::FuzzInput;
using bess::nat::be16_t;
using bess::nat::be32_t;
using bess::nat::Direction;
using bess::nat::ExternalAddress;
using bess::nat::Nat;
using bess::nat::Verdict;

constexpr uint64_t kTimeout = 1000;
constexpr size_t kMaxSteps = 16;
constexpr size_t kMaxFrames = 8;
constexpr size_t kMaxRaw = 600;

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a << 24 | b << 16 | c << 8 | d;
}
constexpr std::array<uint32_t, 7> kPalette = {
    Ip(10, 0, 0, 1),      Ip(10, 0, 0, 2),      Ip(10, 0, 0, 3), Ip(192, 168, 1, 9),
    Ip(198, 51, 100, 1),  Ip(198, 51, 100, 2),  Ip(8, 8, 8, 8),
};

std::vector<ExternalAddress> Addresses(int variant) {
  switch (variant) {
    case 0:  // one address, a small range straddling 1024, a suspended one
      return {{be32_t(Ip(198, 51, 100, 1)), {{1020, 1030, false}, {5000, 5100, true}}}};
    case 1:  // two addresses; port 0 for ICMP; a range ending at 65536
      return {{be32_t(Ip(198, 51, 100, 1)), {{0, 3, false}, {1022, 1026, false}}},
              {be32_t(Ip(198, 51, 100, 2)), {{65530, 65536, false}}}};
    default:  // one IP listed twice with overlapping ranges; an inside address
      return {{be32_t(Ip(198, 51, 100, 1)), {{2000, 2004, false}}},
              {be32_t(Ip(198, 51, 100, 1)), {{2002, 2006, false}}},
              {be32_t(Ip(10, 0, 0, 1)), {{40000, 40002, false}, {1000, 1002, false}}}};
  }
}

uint16_t Get16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t *p) { return uint32_t{Get16(p)} << 16 | Get16(p + 2); }
void Put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
void Put32(uint8_t *p, uint32_t v) {
  Put16(p, static_cast<uint16_t>(v >> 16));
  Put16(p + 2, static_cast<uint16_t>(v));
}

// One's complement arithmetic (RFC 1071), independent of utils/checksum.h.
uint64_t Sum(const uint8_t *p, size_t n) {
  uint64_t s = 0;
  size_t i = 0;
  for (; i + 1 < n; i += 2) s += Get16(p + i);
  if (i < n) s += uint64_t{p[i]} << 8;
  return s;
}
uint16_t Fold(uint64_t s) {
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  return static_cast<uint16_t>(s);
}
uint16_t Checksum(uint64_t sum) { return static_cast<uint16_t>(~Fold(sum)); }

// A packet: its first segment (what Translate sees) and the rest.
struct Pkt {
  std::vector<uint8_t> head;
  std::vector<uint8_t> tail;
  std::vector<uint8_t> Logical() const {
    std::vector<uint8_t> all(head);
    all.insert(all.end(), tail.begin(), tail.end());
    return all;
  }
  ParseStatus Parse(ParsedFlowPacket &p) const {
    return ParseFrame(head, p, head.size() + tail.size());
  }
};

struct Ep {
  uint32_t addr;
  uint16_t port;
  uint8_t proto;
  friend bool operator==(const Ep &, const Ep &) = default;
};

bess::nat::Endpoint ToNat(const Ep &e) { return {be32_t(e.addr), be16_t(e.port), e.proto}; }

Ep EndpointOf(const ParsedFlowPacket &p, Direction dir) {
  const bool fwd = dir == Direction::kForward;
  return {Get32((fwd ? p.src : p.dst).data()), fwd ? p.src_port : p.dst_port, p.protocol};
}

bool Supported(const ParsedFlowPacket &p) {
  if (p.l4 == L4Kind::kTcp || p.l4 == L4Kind::kUdp) return true;
  if (p.l4 != L4Kind::kIcmp) return false;
  const uint8_t t = p.icmp_type;
  return t == 0 || t == 8 || t == 13 || t == 14 || t == 15 || t == 16;
}

struct Mapping {
  Ep internal;
  Ep external;
  uint64_t last;  // creation or last outbound packet
};

struct World {
  std::vector<ExternalAddress> addrs;
  size_t capacity = 1;
  unsigned shift = 0;
  std::unique_ptr<Nat> a, b;
  std::vector<Mapping> live;
  uint64_t now = 0;

  const Mapping *ByInternal(const Ep &e) const {
    for (const Mapping &m : live) {
      if (m.internal == e) return &m;
    }
    return nullptr;
  }
  Mapping *ByInternal(const Ep &e) {
    return const_cast<Mapping *>(static_cast<const World *>(this)->ByInternal(e));
  }
  const Mapping *ByExternal(const Ep &e) const {
    for (const Mapping &m : live) {
      if (m.external == e) return &m;
    }
    return nullptr;
  }
};

// -- frame builders --------------------------------------------------------------

struct V4Spec {
  uint8_t proto = 17;
  uint8_t tcp_flags = 0;
  uint8_t icmp_type = 8;
  uint32_t src = 0, dst = 0;
  uint16_t sport = 0, dport = 0;
  size_t payload = 0;
  uint8_t opts = 0;
  uint8_t cut = 0;
  uint8_t pad = 0;
};

constexpr std::array<uint8_t, 10> kIcmpTypes = {8, 0, 13, 14, 15, 16, 3, 11, 5, 17};

bool IcmpError(uint8_t t) { return t == 3 || t == 4 || t == 5 || t == 11 || t == 12; }

std::vector<uint8_t> BuildV4(const V4Spec &s) {
  std::vector<uint8_t> f = {0x02, 0, 0, 0, 0, 0x02, 0x02, 0, 0, 0, 0, 0x01};
  if (s.opts & 0x01) f.insert(f.end(), {0x81, 0x00, 0x00, 0x05});
  f.insert(f.end(), {0x08, 0x00});
  const size_t l3 = f.size();
  const size_t ihl = (s.opts & 0x02) ? 28 : 20;
  const size_t l4hdr = s.proto == 6 ? 20 : (s.proto == 17 || s.proto == 1) ? 8 : 0;
  std::vector<uint8_t> payload;
  if (s.proto == 1 && IcmpError(s.icmp_type)) {
    // Quote the flow this error is about: the packet in the other direction.
    std::vector<uint8_t> q(28, 0);
    q[0] = 0x45;
    Put16(&q[2], 48);
    q[8] = 64;
    q[9] = 17;
    Put32(&q[12], s.dst);
    Put32(&q[16], s.src);
    Put16(&q[10], Checksum(Sum(q.data(), 20)));
    Put16(&q[20], s.dport);
    Put16(&q[22], s.sport);
    Put16(&q[24], 28);
    payload = q;
  }
  for (size_t i = 0; i < s.payload; i++) payload.push_back(static_cast<uint8_t>(0x30 + i * 7));
  const size_t total = ihl + l4hdr + payload.size();
  f.resize(l3 + total, 0);
  uint8_t *ip = f.data() + l3;
  ip[0] = static_cast<uint8_t>(0x40 | ihl / 4);
  Put16(ip + 2, static_cast<uint16_t>(total));
  Put16(ip + 4, 0x1234);
  Put16(ip + 6, (s.opts & 0x20) ? 0x2000 : 0x4000);
  ip[8] = 64;
  ip[9] = s.proto;
  Put32(ip + 12, s.src);
  Put32(ip + 16, s.dst);
  for (size_t i = 20; i < ihl; i++) ip[i] = 0x01;  // NOP options
  uint8_t *l4 = ip + ihl;
  std::copy(payload.begin(), payload.end(), l4 + l4hdr);
  const uint16_t l4len = static_cast<uint16_t>(l4hdr + payload.size());
  const uint64_t pseudo = Sum(ip + 12, 8) + s.proto + l4len;
  if (s.proto == 6) {
    Put16(l4, s.sport);
    Put16(l4 + 2, s.dport);
    Put32(l4 + 4, 1);
    Put32(l4 + 8, (s.tcp_flags & 0x10) ? 1 : 0);
    l4[12] = 5 << 4;
    l4[13] = s.tcp_flags;
    Put16(l4 + 14, 65535);
    Put16(l4 + 16, Checksum(pseudo + Sum(l4, l4len)));
    if (s.opts & 0x10) l4[17] ^= 1;
  } else if (s.proto == 17) {
    Put16(l4, s.sport);
    Put16(l4 + 2, s.dport);
    Put16(l4 + 4, l4len);
    if (!(s.opts & 0x04)) {
      const uint16_t c = Checksum(pseudo + Sum(l4, l4len));
      Put16(l4 + 6, c != 0 ? c : 0xffff);
      if (s.opts & 0x10) l4[7] ^= 1;
    }
  } else if (s.proto == 1) {
    l4[0] = s.icmp_type;
    Put16(l4 + 4, s.sport);  // identifier
    Put16(l4 + 6, s.dport);  // sequence
    Put16(l4 + 2, Checksum(Sum(l4, l4len)));
    if (s.opts & 0x10) l4[3] ^= 1;
  }
  Put16(ip + 10, Checksum(Sum(ip, ihl)));
  if (s.opts & 0x08) ip[11] ^= 1;
  if (s.opts & 0x80) f.resize(f.size() + s.pad, 0);
  if ((s.opts & 0x40) && s.cut < f.size()) f.resize(f.size() - s.cut);
  return f;
}

std::vector<uint8_t> BuildV6(uint8_t proto, uint16_t sport, uint16_t dport, size_t payload) {
  constexpr size_t l3 = 14;
  const size_t l4hdr = proto == 6 ? 20 : 8;
  std::vector<uint8_t> f(l3 + 40 + l4hdr + payload, 0x41);
  constexpr std::array<uint8_t, l3> kEth = {0x02, 0, 0, 0, 0, 0x02, 0x02, 0, 0, 0, 0, 0x01, 0x86, 0xdd};
  std::copy(kEth.begin(), kEth.end(), f.begin());
  uint8_t *ip = f.data() + l3;
  std::fill(ip, ip + 40 + l4hdr, 0);
  ip[0] = 0x60;
  Put16(ip + 4, static_cast<uint16_t>(l4hdr + payload));
  ip[6] = proto;
  ip[7] = 64;
  ip[8] = 0x20;
  ip[9] = 0x01;
  ip[23] = 1;
  ip[24] = 0x20;
  ip[25] = 0x01;
  ip[39] = 2;
  uint8_t *l4 = ip + 40;
  if (proto == 58) {
    l4[0] = 128;
    Put16(l4 + 4, sport);
    Put16(l4 + 6, dport);
  } else {
    Put16(l4, sport);
    Put16(l4 + 2, dport);
    if (proto == 6) {
      l4[12] = 5 << 4;
      l4[13] = 0x02;
    } else {
      Put16(l4 + 4, static_cast<uint16_t>(8 + payload));
    }
  }
  return f;
}

uint32_t ReadAddr(FuzzInput &in) {
  const uint8_t i = in.U8() % 8;
  return i < kPalette.size() ? kPalette[i] : in.U32();
}

void ReadOpts(FuzzInput &in, V4Spec &s) {
  s.opts = in.U8();
  if (s.opts & 0x40) s.cut = in.U8();
  if (s.opts & 0x80) s.pad = in.U8() % 16;
}

std::vector<uint8_t> ReadTemplate(FuzzInput &in) {
  V4Spec s;
  const uint8_t sel = in.U8() % 4;
  const uint8_t aux = in.U8();
  s.proto = sel == 0 ? 6 : sel == 1 ? 17 : sel == 2 ? 1 : aux;
  s.tcp_flags = aux;
  s.icmp_type = aux < kIcmpTypes.size() ? kIcmpTypes[aux] : aux;
  s.src = ReadAddr(in);
  s.sport = in.U16();
  s.dst = ReadAddr(in);
  s.dport = in.U16();
  s.payload = in.U8() % 64;
  ReadOpts(in, s);
  return BuildV4(s);
}

std::vector<uint8_t> ReadFromMapping(FuzzInput &in, const World &w) {
  const uint8_t sel = in.U8() % 4;
  const uint8_t index = in.U8();
  V4Spec s;
  const uint32_t remote = ReadAddr(in);
  const uint16_t rport = in.U16();
  s.payload = in.U8() % 64;
  ReadOpts(in, s);
  Ep near{Ip(198, 51, 100, 1), 1024, 17};  // no mapping yet: a plausible reply
  if (!w.live.empty()) {
    const Mapping &m = w.live[index % w.live.size()];
    near = (sel == 0 || sel == 2) ? m.external : m.internal;
  }
  const bool toward = sel == 0 || sel == 3;  // `near` is the destination
  s.proto = near.proto;
  s.tcp_flags = toward ? 0x12 : 0x10;
  s.icmp_type = toward ? 0 : 8;
  s.src = toward ? remote : near.addr;
  s.dst = toward ? near.addr : remote;
  if (near.proto == 1) {
    s.sport = near.port;  // the identifier
    s.dport = rport;
  } else {
    s.sport = toward ? rport : near.port;
    s.dport = toward ? near.port : rport;
  }
  return BuildV4(s);
}

Pkt ReadFrame(FuzzInput &in, const World &w) {
  std::vector<uint8_t> f;
  switch (in.U8() % 4) {
    case 0: {
      const auto bytes = in.Bytes(in.U16() % (kMaxRaw + 1));
      f.assign(bytes.begin(), bytes.end());
      break;
    }
    case 1:
      f = ReadTemplate(in);
      break;
    case 2:
      f = ReadFromMapping(in, w);
      break;
    default: {
      const uint8_t proto = std::array<uint8_t, 3>{6, 17, 58}[in.U8() % 3];
      const uint16_t sport = in.U16();
      const uint16_t dport = in.U16();
      f = BuildV6(proto, sport, dport, in.U8() % 64);
      break;
    }
  }
  const uint8_t split = in.U8();
  Pkt p;
  if (split != 0 && split < f.size()) {
    p.head.assign(f.begin(), f.begin() + split);
    p.tail.assign(f.begin() + split, f.end());
  } else {
    p.head = std::move(f);
  }
  return p;
}

// -- oracles ---------------------------------------------------------------------

struct Sums {
  bool ip;
  bool l4;
  uint16_t udp_field;
};

// Checksum validity of a parsed IPv4 TCP/UDP/ICMP packet over its logical bytes:
// the IPv4 header, and the L4 checksum over the IP payload (with the pseudo
// header for TCP and UDP; a UDP checksum 0 is "absent", not valid).
Sums Validity(const std::vector<uint8_t> &f, const ParsedFlowPacket &p) {
  const uint8_t *ip = f.data() + p.l3_offset;
  const uint8_t *l4 = f.data() + p.l4_offset;
  Sums s{};
  s.ip = Fold(Sum(ip, p.l4_offset - p.l3_offset)) == 0xffff;
  uint64_t sum = Sum(l4, p.l4_length);
  if (p.protocol != 1) sum += Sum(ip + 12, 8) + p.protocol + p.l4_length;
  s.l4 = Fold(sum) == 0xffff;
  if (p.protocol == 17) {
    s.udp_field = Get16(l4 + 6);
    s.l4 = s.l4 && s.udp_field != 0;
  }
  return s;
}

size_t L4ChecksumOffset(uint8_t proto) { return proto == 6 ? 16 : proto == 17 ? 6 : 2; }

// The bytes Rewrite may change for a translated packet.
bool Rewritable(size_t i, const ParsedFlowPacket &p, Direction dir) {
  const bool fwd = dir == Direction::kForward;
  auto in = [i](size_t base, size_t off, size_t n) { return i >= base + off && i < base + off + n; };
  const size_t l3 = p.l3_offset, l4 = p.l4_offset;
  if (in(l3, 10, 2) || in(l3, fwd ? 12 : 16, 4)) return true;
  if (p.protocol == 6 || p.protocol == 17) {
    return in(l4, fwd ? 0 : 2, 2) || in(l4, L4ChecksumOffset(p.protocol), 2);
  }
  return in(l4, 2, 4);
}

void CheckTables(const World &w) {
  for (const Nat *nat : {w.a.get(), w.b.get()}) {
    FUZZ_CHECK(nat->size() == w.live.size());
    for (const Mapping &m : w.live) {
      const bess::nat::Binding *bi = nat->Find(ToNat(m.internal));
      FUZZ_CHECK(bi != nullptr);
      FUZZ_CHECK(bi->internal == ToNat(m.internal));
      FUZZ_CHECK(bi->external == ToNat(m.external));
      FUZZ_CHECK(nat->Find(ToNat(m.external)) == bi);
    }
  }
}

// The external port span of `range` for a mapping of `internal` (the
// documented classes: ICMP anywhere; privileged to 1-1023; others >= 1024).
std::pair<uint32_t, uint32_t> Span(const bess::nat::PortRange &r, const Ep &internal) {
  if (internal.proto != 6 && internal.proto != 17) return {r.begin, r.end};
  if (internal.port < 1024) return {std::max<uint32_t>(1, r.begin), std::min<uint32_t>(1024, r.end)};
  return {std::max<uint32_t>(1024, r.begin), r.end};
}

// Checks one packet of Nat A against the model and updates the model.
// `before`/`after` are the logical bytes around Translate.
void CheckOne(World &w, const std::vector<uint8_t> &before, const std::vector<uint8_t> &after,
              ParseStatus st, const ParsedFlowPacket &p, Direction dir, Verdict v) {
  if (v != Verdict::kTranslated) {
    FUZZ_CHECK(before == after);
  }
  if (st != ParseStatus::kOk) {
    FUZZ_CHECK(v == Verdict::kUnsupported);
    return;
  }
  if (p.l3 != L3Kind::kIpv4) {
    FUZZ_CHECK(v == Verdict::kNotIpv4);
    return;
  }
  if (!Supported(p)) {
    FUZZ_CHECK(v == Verdict::kUnsupported);
    return;
  }
  const Ep ep = EndpointOf(p, dir);
  Ep target{};  // the endpoint the packet must now carry
  if (dir == Direction::kReverse) {
    const Mapping *m = w.ByExternal(ep);
    if (m == nullptr) {
      FUZZ_CHECK(v == Verdict::kNoBinding);
      return;
    }
    FUZZ_CHECK(v == Verdict::kTranslated);
    target = m->internal;
  } else if (Mapping *m = w.ByInternal(ep); m != nullptr) {
    FUZZ_CHECK(v == Verdict::kTranslated);
    m->last = w.now;
    target = m->external;
  } else if (w.ByExternal(ep) != nullptr) {
    FUZZ_CHECK(v == Verdict::kConflict);
    return;
  } else if ((ep.proto == 6 || ep.proto == 17) && ep.port == 0) {
    FUZZ_CHECK(v == Verdict::kPortZero);
    return;
  } else if (w.live.size() >= w.capacity) {
    FUZZ_CHECK(v == Verdict::kFull);
    return;
  } else {
    // A new mapping on the address the internal one hashes to (REQ-2), in the
    // first usable range with a free port of the class.
    const uint32_t raw = be32_t(ep.addr).raw_value();
    const ExternalAddress &ext = w.addrs[rte_hash_crc(&raw, sizeof(raw), 0) % w.addrs.size()];
    const uint32_t ext_ip = ext.addr.value();
    std::vector<uint16_t> free;
    for (const auto &range : ext.ranges) {
      if (range.suspended) continue;
      const auto [lo, hi] = Span(range, ep);
      for (uint32_t port = lo; port < hi; port++) {
        if (w.ByExternal({ext_ip, static_cast<uint16_t>(port), ep.proto}) == nullptr) {
          free.push_back(static_cast<uint16_t>(port));
        }
      }
      if (!free.empty()) break;
    }
    if (free.empty()) {
      FUZZ_CHECK(v == Verdict::kExhausted);
      return;
    }
    if (v == Verdict::kConflict) {
      // The chosen port's endpoint is a live internal one, or the packet's own
      // (one key space: a mapping cannot be its own alias).
      bool possible = false;
      for (uint16_t port : free) {
        const Ep chosen{ext_ip, port, ep.proto};
        possible |= chosen == ep || w.ByInternal(chosen) != nullptr;
      }
      FUZZ_CHECK(possible);
      return;
    }
    FUZZ_CHECK(v == Verdict::kTranslated);
    ParsedFlowPacket q;
    FUZZ_CHECK(ParseFrame(after, q) == ParseStatus::kOk);
    target = EndpointOf(q, dir);
    FUZZ_CHECK(target.addr == ext_ip && target.proto == ep.proto);
    FUZZ_CHECK(std::find(free.begin(), free.end(), target.port) != free.end());
    FUZZ_CHECK(target != ep && w.ByInternal(target) == nullptr);
    w.live.push_back({ep, target, w.now});
  }
  // Translated: only the rewritable bytes changed, to the target endpoint.
  FUZZ_CHECK(before.size() == after.size());
  for (size_t i = 0; i < before.size(); i++) {
    FUZZ_CHECK(before[i] == after[i] || Rewritable(i, p, dir));
  }
  ParsedFlowPacket q;
  FUZZ_CHECK(ParseFrame(after, q) == ParseStatus::kOk);
  FUZZ_CHECK(EndpointOf(q, dir) == target);
  const Sums was = Validity(before, p), now = Validity(after, p);
  FUZZ_CHECK(!was.ip || now.ip);
  FUZZ_CHECK(!was.l4 || now.l4);
  FUZZ_CHECK(p.protocol != 17 || was.udp_field != 0 || now.udp_field == 0);
}

// Translates `pkts` (in place, as Nat A did) one by one on A, copies through
// TranslateBatch on B; checks each against the model and A against B.
void RunBatch(World &w, std::vector<Pkt> &pkts, Direction dir, Verdict *out) {
  const size_t n = pkts.size();
  std::vector<Pkt> copies = pkts;
  for (size_t i = 0; i < n; i++) {
    ParsedFlowPacket p;
    const ParseStatus st = pkts[i].Parse(p);
    const std::vector<uint8_t> before = pkts[i].Logical();
    out[i] = st == ParseStatus::kOk ? w.a->Translate(pkts[i].head, p, dir, w.now)
                                    : Verdict::kUnsupported;
    CheckOne(w, before, pkts[i].Logical(), st, p, dir, out[i]);
  }
  std::span<uint8_t> frames[kMaxFrames];
  ParsedFlowPacket parsed[kMaxFrames];
  bool ok[kMaxFrames];
  Verdict got[kMaxFrames];
  for (size_t i = 0; i < n; i++) {
    frames[i] = copies[i].head;
    ok[i] = copies[i].Parse(parsed[i]) == ParseStatus::kOk;
  }
  w.b->TranslateBatch(std::span<const std::span<uint8_t>>(frames, n),
                      std::span<const ParsedFlowPacket>(parsed, n), std::span<const bool>(ok, n),
                      dir, w.now, std::span<Verdict>(got, n));
  for (size_t i = 0; i < n; i++) {
    FUZZ_CHECK(got[i] == out[i]);
    FUZZ_CHECK(copies[i].head == pkts[i].head);
    FUZZ_CHECK(copies[i].tail == pkts[i].tail);
  }
  CheckTables(w);
}

// The reply to a packet: addresses swapped, TCP/UDP ports swapped (an ICMP
// query keeps its identifier). Checksums stay valid: the sums commute.
void Swap(std::vector<uint8_t> &f, const ParsedFlowPacket &p) {
  uint8_t *ip = f.data() + p.l3_offset;
  std::swap_ranges(ip + 12, ip + 16, ip + 16);
  if (p.protocol == 6 || p.protocol == 17) {
    uint8_t *l4 = f.data() + p.l4_offset;
    std::swap_ranges(l4, l4 + 2, l4 + 2);
  }
}

// Equal, except that checksum fields may differ by the one's complement zero.
bool SameUpToZero(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
                  const ParsedFlowPacket &p) {
  if (a.size() != b.size()) return false;
  const size_t sums[2] = {size_t{p.l3_offset} + 10, p.l4_offset + L4ChecksumOffset(p.protocol)};
  for (size_t i = 0; i < a.size(); i++) {
    if (i == sums[0] || i == sums[1]) {
      const uint16_t x = Get16(&a[i]), y = Get16(&b[i]);
      if (x != y && !((x == 0 && y == 0xffff) || (x == 0xffff && y == 0))) return false;
      i++;
    } else if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

void Expire(World &w, size_t budget) {
  const size_t removed_a = w.a->Expire(w.now, budget);
  const size_t removed_b = w.b->Expire(w.now, budget);
  FUZZ_CHECK(removed_a == removed_b);
  const uint64_t grain = uint64_t{1} << w.shift;
  size_t removed = 0;
  for (auto it = w.live.begin(); it != w.live.end();) {
    const uint64_t due = it->last + kTimeout;
    const bool gone = w.a->Find(ToNat(it->internal)) == nullptr;
    FUZZ_CHECK(gone == (w.b->Find(ToNat(it->internal)) == nullptr));
    if (gone) {
      FUZZ_CHECK(due <= w.now);  // never one used within the timeout
      it = w.live.erase(it);
      removed++;
    } else {
      // Unbounded: everything due at the wheel's granularity is gone.
      FUZZ_CHECK(budget != ~size_t{0} || w.now < (due + grain - 1) / grain * grain);
      ++it;
    }
  }
  FUZZ_CHECK(removed == removed_a);
  CheckTables(w);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FuzzInput in(data, size);
  World w;
  const uint8_t config = in.U8();
  w.addrs = Addresses(config % 3);
  w.shift = (config & 0x80) ? 3 : 0;
  w.capacity = 1 + in.U8() % 16;
  Nat::Config c;
  c.addresses = w.addrs;
  c.capacity = w.capacity;
  c.timeout = kTimeout;
  c.granularity_shift = w.shift;
  c.start = 0;
  c.seed = in.U16();
  auto a = Nat::Create(c);
  auto b = Nat::Create(c);
  FUZZ_CHECK(a.has_value() && b.has_value());
  w.a = std::move(*a);
  w.b = std::move(*b);

  for (size_t step = 0; step < kMaxSteps && !in.empty(); step++) {
    const uint8_t ctl = in.U8();
    const Direction dir = (ctl & 1) ? Direction::kReverse : Direction::kForward;
    const Direction back = (ctl & 1) ? Direction::kForward : Direction::kReverse;
    const size_t n = 1 + (ctl >> 1 & 7);
    w.now += (ctl & 0x20) ? in.U16() : in.U8();
    if (ctl & 0x10) {
      const uint8_t budget = in.U8();
      Expire(w, budget == 0xff ? ~size_t{0} : budget & 15);
    }
    std::vector<Pkt> pkts;
    for (size_t i = 0; i < n; i++) pkts.push_back(ReadFrame(in, w));
    const std::vector<Pkt> orig = pkts;
    Verdict v[kMaxFrames];
    RunBatch(w, pkts, dir, v);

    // Round trip: the replies to what was translated come back as the
    // originals, swapped.
    std::vector<Pkt> replies;
    std::vector<std::vector<uint8_t>> want;
    std::vector<ParsedFlowPacket> layout;
    for (size_t i = 0; i < n; i++) {
      if (v[i] != Verdict::kTranslated) continue;
      Pkt r;
      r.head = pkts[i].Logical();
      ParsedFlowPacket p;
      FUZZ_CHECK(ParseFrame(r.head, p) == ParseStatus::kOk);
      Swap(r.head, p);
      std::vector<uint8_t> o = orig[i].Logical();
      Swap(o, p);
      replies.push_back(std::move(r));
      want.push_back(std::move(o));
      layout.push_back(p);
    }
    if (replies.empty()) continue;
    Verdict rv[kMaxFrames];
    RunBatch(w, replies, back, rv);
    for (size_t j = 0; j < replies.size(); j++) {
      FUZZ_CHECK(rv[j] == Verdict::kTranslated);
      FUZZ_CHECK(SameUpToZero(replies[j].head, want[j], layout[j]));
    }
  }
  return 0;
}
