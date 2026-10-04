// SPDX-License-Identifier: BSD-3-Clause

// Fuzz target: connection tracking (conntrack/packet_parse.h, conntrack.h).
//
// The first input byte picks one of two stages (bit 0).
//
// Stage A, parser (bit 0 clear): [total_len mode u8][k u16][frame: rest].
// total_len is 0, the frame's size, size - k (clamped at 0) or size + k.
// ParseFrame's result is checked against the frame bytes: an accepted packet's
// L3 offset is the Ethernet header length (walked here independently), its L4
// offset the IP header's end, every offset and length lies inside the frame
// (headers) or total_len (IP and L4 lengths), the addresses, protocol, ports,
// flags and ICMP fields are the bytes at those offsets. The parse is a pure
// function (called twice; total_len 0 and total_len <= size agree), reads
// nothing past the headers it accepted (the frame cut to the headers' extent
// gives the same result, one byte less is refused), agrees with ParseIpPacket
// on the IP packet, and with the same frame zero-padded up to total_len.
//
// Stage B, tracker (bit 0 set): [capacity u8][config u8][start u16] then
// steps [op u8][tick delta u8][operands]. A Conntrack<uint32_t> of capacity
// 1-16 with a fixed policy (distinct short timeouts, tcp_pickup and the
// wheel granularity 0/1/3 from config) runs Track on frames built from
// templates (TCP with fuzzed flags, UDP, ICMP echo request/reply, ICMP errors
// quoting a TCP/UDP/echo packet, GRE, untracked ICMP, fragments; IPv4/IPv6,
// VLAN, endpoints and ports from a small pool so flows collide) or on raw
// bytes, Expire(now, budget), Remove and SetDeadline on live or dead handles.
// A std::map model (canonical key -> handle, TCP state, initiator, replied,
// deadline, user stamp) predicts every Track result (status, direction,
// handle, entry fields; Linux's TCP table is the one input taken from the
// header), creation refusals (policy, may_create, capacity), ICMP-error
// relation, TCP reopen after TIME_WAIT/CLOSE (user data reset), and which
// flows Expire removes: exactly the flows whose deadline (last packet +
// state timeout, or SetDeadline) is due at the wheel's granularity, a subset
// of them when the budget is small, never one in the future. After every step:
// size == model size <= capacity, every live handle resolves to its key and
// entry, Find agrees, dead handles and keys stay dead until re-created, and a
// packet and its reverse map to the same flow.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "conntrack/conntrack.h"
#include "conntrack/packet_parse.h"
#include "fuzz/fuzz_support.h"

namespace bess::conntrack {
namespace {

using fuzz::FuzzInput;

uint16_t Be16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

bool Same(const ParsedFlowPacket &a, const ParsedFlowPacket &b) {
  return a.l3 == b.l3 && a.l4 == b.l4 && a.protocol == b.protocol &&
         a.tcp_flags == b.tcp_flags && a.l3_offset == b.l3_offset &&
         a.l4_offset == b.l4_offset && a.l4_length == b.l4_length && a.src == b.src &&
         a.dst == b.dst && a.src_port == b.src_port && a.dst_port == b.dst_port &&
         a.icmp_type == b.icmp_type && a.icmp_code == b.icmp_code &&
         a.first_fragment == b.first_fragment;
}

// The L4 header bytes the parser must have read to accept a packet.
size_t L4HeaderBytes(L4Kind k) {
  switch (k) {
    case L4Kind::kTcp:
      return 20;
    case L4Kind::kUdp:
    case L4Kind::kIcmp:
    case L4Kind::kIcmpv6:
      return 8;
    default:
      return 0;
  }
}

// The Ethernet header length (up to two 802.1Q/802.1ad tags) and the
// EtherType after it; 0 if the frame cannot hold them.
size_t L2Length(std::span<const uint8_t> frame, uint16_t &type) {
  if (frame.size() < 14) {
    return 0;
  }
  size_t off = 12;
  type = Be16(frame.data() + off);
  for (int tags = 0; tags < 2 && (type == 0x8100 || type == 0x88a8); tags++) {
    off += 4;
    if (frame.size() < off + 2) {
      return 0;
    }
    type = Be16(frame.data() + off);
  }
  return off + 2;
}

// -- stage A: the parser ----------------------------------------------------------

void CheckAccepted(std::span<const uint8_t> frame, size_t total, const ParsedFlowPacket &p) {
  uint16_t type = 0;
  const size_t l2 = L2Length(frame, type);
  FUZZ_CHECK(l2 != 0 && p.l3_offset == l2);
  const uint8_t *ip = frame.data() + l2;
  size_t ip_total;
  if (p.l3 == L3Kind::kIpv4) {
    FUZZ_CHECK(type == 0x0800 && l2 + 20 <= frame.size() && (ip[0] >> 4) == 4);
    const size_t ihl = size_t{ip[0] & 0x0fu} * 4;
    ip_total = Be16(ip + 2);
    FUZZ_CHECK(ihl >= 20 && ihl <= ip_total && p.l4_offset == l2 + ihl);
    const uint16_t frag = Be16(ip + 6);
    FUZZ_CHECK((frag & 0x1fff) == 0 && p.first_fragment == ((frag & 0x2000) != 0));
    FUZZ_CHECK(p.protocol == ip[9]);
    FUZZ_CHECK(std::memcmp(p.src.data(), ip + 12, 4) == 0 &&
               std::memcmp(p.dst.data(), ip + 16, 4) == 0);
    for (size_t i = 4; i < 16; i++) {
      FUZZ_CHECK(p.src[i] == 0 && p.dst[i] == 0);
    }
  } else {
    FUZZ_CHECK(p.l3 == L3Kind::kIpv6);
    FUZZ_CHECK(type == 0x86dd && l2 + 40 <= frame.size() && (ip[0] >> 4) == 6);
    ip_total = 40 + size_t{Be16(ip + 4)};
    FUZZ_CHECK(p.l4_offset >= l2 + 40 && (p.l4_offset - l2 - 40) % 8 == 0);
    FUZZ_CHECK(std::memcmp(p.src.data(), ip + 8, 16) == 0 &&
               std::memcmp(p.dst.data(), ip + 24, 16) == 0);
  }
  // The IP packet lies in the packet; its L4 part ends where it ends.
  FUZZ_CHECK(l2 + ip_total <= total);
  FUZZ_CHECK(p.l4_offset <= frame.size());
  FUZZ_CHECK(size_t{p.l4_offset} + p.l4_length == l2 + ip_total);
  const size_t hdr = L4HeaderBytes(p.l4);
  FUZZ_CHECK(p.l4_offset + hdr <= frame.size() && hdr <= p.l4_length);
  const uint8_t *l4 = frame.data() + p.l4_offset;
  switch (p.l4) {
    case L4Kind::kTcp: {
      const size_t doff = static_cast<size_t>(l4[12] >> 4) * 4;
      FUZZ_CHECK(p.protocol == 6 && doff >= 20 && (p.first_fragment || doff <= p.l4_length));
      FUZZ_CHECK(p.src_port == Be16(l4) && p.dst_port == Be16(l4 + 2) &&
                 p.tcp_flags == l4[13]);
      break;
    }
    case L4Kind::kUdp: {
      const uint16_t len = Be16(l4 + 4);
      FUZZ_CHECK(p.protocol == 17 && len >= 8 && (p.first_fragment || len <= p.l4_length));
      FUZZ_CHECK(p.src_port == Be16(l4) && p.dst_port == Be16(l4 + 2) && p.tcp_flags == 0);
      break;
    }
    case L4Kind::kIcmp:
    case L4Kind::kIcmpv6:
      FUZZ_CHECK(p.protocol == (p.l4 == L4Kind::kIcmp ? 1 : 58));
      FUZZ_CHECK(p.icmp_type == l4[0] && p.icmp_code == l4[1]);
      FUZZ_CHECK(p.src_port == Be16(l4 + 4) && p.dst_port == p.src_port);
      break;
    case L4Kind::kOther:
      FUZZ_CHECK(p.protocol != 6 && p.protocol != 17 && p.protocol != 1 && p.protocol != 58);
      FUZZ_CHECK(p.src_port == 0 && p.dst_port == 0 && p.tcp_flags == 0);
      break;
    case L4Kind::kNone:
      FUZZ_CHECK(false && "accepted without an L4 kind");
  }

  // Nothing past the headers was needed, and every header byte was.
  const size_t extent = p.l4_offset + hdr;
  ParsedFlowPacket q;
  FUZZ_CHECK(ParseFrame(frame.first(extent), q, total) == ParseStatus::kOk && Same(p, q));
  FUZZ_CHECK(ParseFrame(frame.first(extent - 1), q, total) != ParseStatus::kOk);

  // The same IP packet without its Ethernet header.
  if (total == frame.size()) {
    FUZZ_CHECK(ParseIpPacket(frame.subspan(l2), q) == ParseStatus::kOk);
    FUZZ_CHECK(q.l3_offset == 0 && q.l4_offset == p.l4_offset - l2);
    q.l3_offset = p.l3_offset;
    q.l4_offset = p.l4_offset;
    FUZZ_CHECK(Same(p, q));
  }
  // The bytes beyond the frame were never read: any contents give the same.
  if (total > frame.size() && total - frame.size() <= 4096) {
    std::vector<uint8_t> padded(frame.begin(), frame.end());
    padded.resize(total, 0);
    FUZZ_CHECK(ParseFrame(padded, q) == ParseStatus::kOk && Same(p, q));
  }
}

void CheckParse(std::span<const uint8_t> frame, size_t total_len) {
  ParsedFlowPacket a;
  ParsedFlowPacket b;
  const ParseStatus s = ParseFrame(frame, a, total_len);
  FUZZ_CHECK(ParseFrame(frame, b, total_len) == s && Same(a, b));
  if (total_len <= frame.size()) {
    FUZZ_CHECK(ParseFrame(frame, b) == s && Same(a, b));
  }
  const size_t total = std::max(total_len, frame.size());
  uint16_t type = 0;
  const size_t l2 = L2Length(frame, type);
  switch (s) {
    case ParseStatus::kOk:
      CheckAccepted(frame, total, a);
      break;
    case ParseStatus::kNotIp:
      FUZZ_CHECK(l2 != 0 && type != 0x0800 && type != 0x86dd);
      break;
    case ParseStatus::kFragment:
      FUZZ_CHECK(l2 != 0 && a.l3_offset == l2 && a.l3 != L3Kind::kNone &&
                 a.l4 == L4Kind::kNone);
      FUZZ_CHECK(l2 + (a.l3 == L3Kind::kIpv4 ? 20 : 48) <= frame.size());
      if (a.l3 == L3Kind::kIpv4) {
        FUZZ_CHECK((Be16(frame.data() + l2 + 6) & 0x1fff) != 0);
      }
      break;
    case ParseStatus::kMalformed:
      break;
  }
}

void RunParse(FuzzInput &in) {
  const uint8_t mode = in.U8();
  const uint16_t k = in.U16();
  const std::span<const uint8_t> frame = in.Rest();
  size_t total_len = 0;
  switch (mode % 4) {
    case 0:
      total_len = 0;
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
  CheckParse(frame, total_len);
}

// -- stage B: the tracker against a model ----------------------------------------

constexpr std::array<std::array<uint8_t, 4>, 4> kV4 = {{
    {10, 0, 0, 1}, {10, 0, 0, 2}, {192, 168, 7, 9}, {10, 0, 0, 1}}};
constexpr std::array<std::array<uint8_t, 16>, 4> kV6 = {{
    {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2},
    {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7, 0x9},
    {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}};
constexpr std::array<uint16_t, 3> kPorts = {1000, 2000, 80};

uint16_t Port(FuzzInput &in, unsigned index) {
  return index < kPorts.size() ? kPorts[index] : in.U16();
}

void Put16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

std::vector<uint8_t> Tcp(uint16_t sp, uint16_t dp, uint8_t flags) {
  std::vector<uint8_t> l4;
  Put16(l4, sp);
  Put16(l4, dp);
  l4.insert(l4.end(), {0, 0, 0, 1, 0, 0, 0, 0, 0x50, flags, 0xff, 0xff, 0, 0, 0, 0});
  return l4;
}

std::vector<uint8_t> Udp(uint16_t sp, uint16_t dp, size_t payload, size_t stated_extra = 0) {
  std::vector<uint8_t> l4;
  Put16(l4, sp);
  Put16(l4, dp);
  Put16(l4, static_cast<uint16_t>(8 + payload + stated_extra));
  Put16(l4, 0);
  l4.resize(8 + payload, 0x5a);
  return l4;
}

std::vector<uint8_t> Icmp(uint8_t type, uint8_t code, uint16_t ident) {
  std::vector<uint8_t> l4 = {type, code, 0, 0};
  Put16(l4, ident);
  Put16(l4, 1);
  return l4;
}

// An IP header (no options, no extension headers) in front of `l4`.
std::vector<uint8_t> Ip(bool v6, unsigned src, unsigned dst, uint8_t proto,
                        const std::vector<uint8_t> &l4, uint16_t frag = 0x4000) {
  std::vector<uint8_t> ip;
  if (!v6) {
    ip = {0x45, 0};
    Put16(ip, static_cast<uint16_t>(20 + l4.size()));
    Put16(ip, 1);
    Put16(ip, frag);
    ip.insert(ip.end(), {64, proto, 0, 0});
    ip.insert(ip.end(), kV4[src].begin(), kV4[src].end());
    ip.insert(ip.end(), kV4[dst].begin(), kV4[dst].end());
  } else {
    ip = {0x60, 0, 0, 0};
    Put16(ip, static_cast<uint16_t>(l4.size()));
    ip.insert(ip.end(), {proto, 64});
    ip.insert(ip.end(), kV6[src].begin(), kV6[src].end());
    ip.insert(ip.end(), kV6[dst].begin(), kV6[dst].end());
  }
  ip.insert(ip.end(), l4.begin(), l4.end());
  return ip;
}

enum TemplateKind : unsigned {
  kTcpPacket,
  kUdpPacket,
  kEchoRequest,
  kEchoReply,
  kIcmpError,
  kGre,
  kIcmpUntracked,
  kFragmentPacket,
};

struct Built {
  std::vector<uint8_t> frame;
  ParseStatus expect;
};

// A frame from a template: [kind/family/vlan/swap u8][endpoints u8][operands].
Built BuildTemplate(FuzzInput &in) {
  const uint8_t b0 = in.U8();
  const uint8_t b1 = in.U8();
  const unsigned kind = b0 % 8;
  const bool v6 = (b0 & 8) != 0;
  const bool vlan = (b0 & 16) != 0;
  const bool swap = (b0 & 32) != 0;
  unsigned si = b1 & 3;
  unsigned di = (b1 >> 2) & 3;
  uint16_t sp = Port(in, (b1 >> 4) & 3);
  uint16_t dp = Port(in, (b1 >> 6) & 3);
  if (swap) {
    std::swap(si, di);
    std::swap(sp, dp);
  }
  const uint8_t icmp_proto = v6 ? 58 : 1;
  ParseStatus expect = ParseStatus::kOk;
  std::vector<uint8_t> ip;
  switch (kind) {
    case kTcpPacket:
      ip = Ip(v6, si, di, 6, Tcp(sp, dp, in.U8()));
      break;
    case kUdpPacket:
      ip = Ip(v6, si, di, 17, Udp(sp, dp, in.U8() % 4));
      break;
    case kEchoRequest:
      ip = Ip(v6, si, di, icmp_proto, Icmp(v6 ? 128 : 8, 0, sp));
      break;
    case kEchoReply:
      ip = Ip(v6, si, di, icmp_proto, Icmp(v6 ? 129 : 0, 0, sp));
      break;
    case kIcmpError: {
      // An error from si to di about a packet di sent: the quote is that
      // packet's IP header and first 8 L4 bytes.
      const uint8_t b2 = in.U8();
      const unsigned qdst = b2 & 3;
      const uint16_t qsp = Port(in, (b2 >> 2) & 3);
      const uint16_t qdp = Port(in, (b2 >> 4) & 3);
      std::vector<uint8_t> inner;
      switch ((b2 >> 6) % 3) {
        case 0:
          inner = Ip(v6, di, qdst, 6, Tcp(qsp, qdp, 0x02));
          break;
        case 1:
          inner = Ip(v6, di, qdst, 17, Udp(qsp, qdp, 0));
          break;
        default:
          inner = Ip(v6, di, qdst, icmp_proto, Icmp(v6 ? 128 : 8, 0, qsp));
          break;
      }
      inner.resize((v6 ? 40 : 20) + 8);
      const uint8_t type = v6 ? ((b2 & 0x80) ? 3 : 1) : ((b2 & 0x80) ? 11 : 3);
      std::vector<uint8_t> l4 = {type, 0, 0, 0, 0, 0, 0, 0};
      l4.insert(l4.end(), inner.begin(), inner.end());
      ip = Ip(v6, si, di, icmp_proto, l4);
      break;
    }
    case kGre:
      ip = Ip(v6, si, di, 47, {0, 0, 0x08, 0x00});
      break;
    case kIcmpUntracked:
      ip = Ip(v6, si, di, icmp_proto, Icmp(v6 ? 135 : 13, 0, sp));
      break;
    default: {  // kFragmentPacket
      const bool first = in.Bool();
      if (first) {  // offset 0, more fragments: the UDP length says more
        if (!v6) {
          ip = Ip(false, si, di, 17, Udp(sp, dp, 8, 64), 0x2000);
        } else {
          std::vector<uint8_t> ext = {17, 0, 0, 1, 0, 0, 0, 7};
          const auto udp = Udp(sp, dp, 8, 64);
          ext.insert(ext.end(), udp.begin(), udp.end());
          ip = Ip(true, si, di, 44, ext);
        }
      } else {
        expect = ParseStatus::kFragment;
        if (!v6) {
          ip = Ip(false, si, di, 17, std::vector<uint8_t>(16, 0x5a), 0x00b9);
        } else {
          std::vector<uint8_t> ext = {17, 0, 0x05, 0xc8, 0, 0, 0, 7};
          ext.resize(24, 0x5a);
          ip = Ip(true, si, di, 44, ext);
        }
      }
      break;
    }
  }
  std::vector<uint8_t> f = {0x02, 0, 0, 0, 0, 0x02, 0x02, 0, 0, 0, 0, 0x01};
  if (vlan) {
    f.insert(f.end(), {0x81, 0x00, 0x00, 0x07});
  }
  Put16(f, v6 ? 0x86dd : 0x0800);
  f.insert(f.end(), ip.begin(), ip.end());
  return {std::move(f), expect};
}

struct KeyLess {
  bool operator()(const CtKey &a, const CtKey &b) const {
    return std::memcmp(&a, &b, sizeof(CtKey)) < 0;
  }
};

bool SameKey(const CtKey &a, const CtKey &b) {
  return std::memcmp(&a, &b, sizeof(CtKey)) == 0;
}

// The connection key, written from its definition: the lesser (address,
// port) endpoint is A.
CtKey ModelKey(const std::array<uint8_t, 16> &src, const std::array<uint8_t, 16> &dst,
               uint16_t sport, uint16_t dport, uint8_t protocol, bool v4, uint16_t zone,
               bool &src_is_a) {
  const int c = std::memcmp(src.data(), dst.data(), 16);
  src_is_a = c < 0 || (c == 0 && sport <= dport);
  CtKey k{};
  k.addr_a = src_is_a ? src : dst;
  k.addr_b = src_is_a ? dst : src;
  k.port_a = src_is_a ? sport : dport;
  k.port_b = src_is_a ? dport : sport;
  k.protocol = protocol;
  k.family = v4 ? 4 : 6;
  k.zone = zone;
  return k;
}

TimeoutPolicy Policy(bool pickup) {
  TimeoutPolicy p;
  p.tcp = {5, 11, 7, 41, 13, 9, 3, 17, 2, 19};
  p.udp_unreplied = 4;
  p.udp_replied = 23;
  p.icmp = 6;
  p.other = 29;
  p.tcp_pickup = pickup;
  return p;
}

class Tracker {
 public:
  using Ct = Conntrack<uint32_t>;

  Tracker(size_t capacity, const TimeoutPolicy &policy, uint64_t start, unsigned shift)
      : ct_(Ct::Create(capacity, policy, start, shift).value()),
        policy_(policy),
        capacity_(capacity),
        shift_(shift),
        now_(start) {}

  void Advance(uint8_t d) { now_ += d < 192 ? d & 7u : uint64_t{d - 192u} * 8; }

  void Track(std::span<const uint8_t> frame, ParseStatus expect, bool check_expect,
             uint16_t zone, bool may_create) {
    ParsedFlowPacket p;
    const ParseStatus s = ParseFrame(frame, p);
    if (check_expect) {
      FUZZ_CHECK(s == expect);
    }
    if (s != ParseStatus::kOk && s != ParseStatus::kFragment) {
      return;
    }
    const Ct::Result r = ct_->Track(frame, p, now_, zone, may_create);
    Model(frame, p, zone, may_create, r);
  }

  void Expire(size_t budget) {
    const size_t removed = ct_->Expire(now_, budget);
    size_t gone = 0;
    for (auto it = flows_.begin(); it != flows_.end();) {
      if (ct_->Lookup(it->second.handle) != nullptr) {
        // A flow left behind is not due, unless the budget ran out.
        FUZZ_CHECK(budget != std::numeric_limits<size_t>::max() || !Due(it->second.deadline));
        ++it;
        continue;
      }
      FUZZ_CHECK(Due(it->second.deadline));  // never removed before its time
      gone++;
      Bury(it->second.handle, it->first);
      it = flows_.erase(it);
    }
    FUZZ_CHECK(removed == gone);
  }

  void Remove(uint8_t pick) {
    const auto [live, dead, handle] = Pick(pick);
    if (live != flows_.end()) {
      FUZZ_CHECK(ct_->Remove(handle));
      Bury(handle, live->first);
      flows_.erase(live);
    } else {
      FUZZ_CHECK(!ct_->Remove(handle));
    }
    (void)dead;
  }

  void SetDeadline(uint8_t pick, uint8_t ahead) {
    const auto [live, dead, handle] = Pick(pick);
    const uint64_t deadline = now_ + 1 + ahead % 32;
    if (live != flows_.end()) {
      FUZZ_CHECK(ct_->SetDeadline(handle, deadline));
      live->second.deadline = deadline;
    } else {
      FUZZ_CHECK(!ct_->SetDeadline(handle, deadline));
    }
    (void)dead;
  }

  void CheckAll() {
    FUZZ_CHECK(ct_->capacity() == capacity_);
    FUZZ_CHECK(ct_->size() == flows_.size() && flows_.size() <= capacity_);
    for (const auto &[key, f] : flows_) {
      Ct::Entry *e = ct_->Lookup(f.handle);
      FUZZ_CHECK(e != nullptr && Matches(*e, f));
      const CtKey *k = ct_->KeyOf(f.handle);
      FUZZ_CHECK(k != nullptr && SameKey(*k, key));
      FUZZ_CHECK(ct_->Find(key) == e);
    }
    for (const auto &[handle, key] : dead_) {
      FUZZ_CHECK(ct_->Lookup(handle) == nullptr);
      if (flows_.find(key) == flows_.end()) {
        FUZZ_CHECK(ct_->Find(key) == nullptr);
      }
    }
  }

 private:
  struct Flow {
    flow::FlowHandle handle;
    TcpState tcp;
    bool initiator_is_a;
    bool replied;
    uint64_t deadline;
    uint32_t user;
  };
  using Flows = std::map<CtKey, Flow, KeyLess>;

  static bool Matches(const Ct::Entry &e, const Flow &f) {
    return e.tcp == f.tcp && e.initiator_is_a == f.initiator_is_a && e.replied == f.replied &&
           e.user == f.user;
  }

  // Fires at the first Expire whose `now`, rounded down to the granularity,
  // reaches the deadline rounded up to it (expiry_wheel.h).
  bool Due(uint64_t deadline) const {
    const uint64_t unit = uint64_t{1} << shift_;
    return (now_ & ~(unit - 1)) >= ((deadline + unit - 1) & ~(unit - 1));
  }

  uint64_t Timeout(L4Kind l4, const Flow &f) const {
    switch (l4) {
      case L4Kind::kTcp:
        return policy_.tcp[static_cast<size_t>(f.tcp)];
      case L4Kind::kUdp:
        return f.replied ? policy_.udp_replied : policy_.udp_unreplied;
      case L4Kind::kIcmp:
      case L4Kind::kIcmpv6:
        return policy_.icmp;
      default:
        return policy_.other;
    }
  }

  void Bury(flow::FlowHandle h, const CtKey &key) {
    dead_.emplace_back(h, key);
    if (dead_.size() > 32) {
      dead_.pop_front();
    }
  }

  struct Picked {
    Flows::iterator live;
    bool dead;
    flow::FlowHandle handle;
  };
  // A live flow, a dead handle or the handle of no flow.
  Picked Pick(uint8_t pick) {
    const size_t i = pick % (flows_.size() + dead_.size() + 1);
    if (i < flows_.size()) {
      auto it = std::next(flows_.begin(), static_cast<ptrdiff_t>(i));
      return {it, false, it->second.handle};
    }
    if (i - flows_.size() < dead_.size()) {
      return {flows_.end(), true, dead_[i - flows_.size()].first};
    }
    return {flows_.end(), false, flow::kNoFlow};
  }

  static void ExpectNone(const Ct::Result &r, TrackStatus s,
                         Direction d = Direction::kOriginal) {
    FUZZ_CHECK(r.status == s && r.direction == d && r.entry == nullptr);
  }

  void ExpectFlow(const Ct::Result &r, TrackStatus s, Direction d, Flow &f) {
    FUZZ_CHECK(r.status == s && r.direction == d && r.handle == f.handle);
    FUZZ_CHECK(r.entry != nullptr && r.entry == ct_->Lookup(f.handle) && Matches(*r.entry, f));
    r.entry->user = f.user = ++stamp_;
  }

  enum Cls { kSyn, kSynAck, kFin, kAck, kRst, kNoFlags };
  static Cls ClassOf(uint8_t flags) {
    if (flags & kTcpRst) return kRst;
    if (flags & kTcpSyn) return (flags & kTcpAck) ? kSynAck : kSyn;
    if (flags & kTcpFin) return kFin;
    if (flags & kTcpAck) return kAck;
    return kNoFlags;
  }

  // The quote of an ICMP error, read from the definition: an IP header
  // (IPv4: options allowed, not a non-initial fragment) and the first 8 bytes
  // of its L4 (TCP/UDP ports; for ICMP only an echo's identifier).
  static bool Quote(std::span<const uint8_t> q, bool v6, std::array<uint8_t, 16> &src,
                    std::array<uint8_t, 16> &dst, uint16_t &sport, uint16_t &dport,
                    uint8_t &proto) {
    src = {};
    dst = {};
    sport = dport = 0;
    size_t hdr;
    if (!v6) {
      if (q.size() < 20 || (q[0] >> 4) != 4) return false;
      hdr = size_t{q[0] & 0x0fu} * 4;
      if (hdr < 20 || hdr > q.size() || (Be16(q.data() + 6) & 0x1fff) != 0) return false;
      proto = q[9];
      std::memcpy(src.data(), q.data() + 12, 4);
      std::memcpy(dst.data(), q.data() + 16, 4);
    } else {
      if (q.size() < 40 || (q[0] >> 4) != 6) return false;
      hdr = 40;
      proto = q[6];
      std::memcpy(src.data(), q.data() + 8, 16);
      std::memcpy(dst.data(), q.data() + 24, 16);
    }
    if (proto == 6 || proto == 17) {
      if (q.size() < hdr + 4) return false;
      sport = Be16(q.data() + hdr);
      dport = Be16(q.data() + hdr + 2);
    } else if (proto == 1 || proto == 58) {
      if (q.size() < hdr + 8) return false;
      const uint8_t t = q[hdr];
      const bool echo = proto == 1 ? (t == 0 || t == 8) : (t == 128 || t == 129);
      if (!echo) return false;
      sport = dport = Be16(q.data() + hdr + 4);
    }
    return true;
  }

  void Related(std::span<const uint8_t> frame, const ParsedFlowPacket &p, uint16_t zone,
               const Ct::Result &r) {
    const size_t start = size_t{p.l4_offset} + 8;
    const size_t end = std::min(frame.size(), size_t{p.l4_offset} + p.l4_length);
    std::array<uint8_t, 16> src;
    std::array<uint8_t, 16> dst;
    uint16_t sport;
    uint16_t dport;
    uint8_t proto;
    const bool v6 = p.l3 == L3Kind::kIpv6;
    if (end < start || !Quote(frame.subspan(start, end - start), v6, src, dst, sport, dport,
                              proto)) {
      ExpectNone(r, TrackStatus::kInvalid);
      return;
    }
    bool src_is_a;
    const CtKey key = ModelKey(src, dst, sport, dport, proto, !v6, zone, src_is_a);
    const auto it = flows_.find(key);
    if (it == flows_.end()) {
      ExpectNone(r, TrackStatus::kInvalid);
      return;
    }
    // The error travels opposite to the quoted packet.
    const bool quoted_original = src_is_a == it->second.initiator_is_a;
    ExpectFlow(r, TrackStatus::kRelated,
               quoted_original ? Direction::kReply : Direction::kOriginal, it->second);
  }

  void Model(std::span<const uint8_t> frame, const ParsedFlowPacket &p, uint16_t zone,
             bool may_create, const Ct::Result &r) {
    if (p.l3 == L3Kind::kNone || p.l4 == L4Kind::kNone) {
      ExpectNone(r, TrackStatus::kUntracked);
      return;
    }
    const bool icmp = p.l4 == L4Kind::kIcmp || p.l4 == L4Kind::kIcmpv6;
    const bool icmp4 = p.l4 == L4Kind::kIcmp;
    const uint8_t t = p.icmp_type;
    if (icmp) {
      const bool error = icmp4 ? (t == 3 || t == 4 || t == 5 || t == 11 || t == 12)
                               : (t >= 1 && t <= 4);
      if (error) {
        Related(frame, p, zone, r);
        return;
      }
      if (!(icmp4 ? (t == 8 || t == 0) : (t == 128 || t == 129))) {
        ExpectNone(r, TrackStatus::kUntracked);
        return;
      }
    }
    bool src_is_a;
    const CtKey key = ModelKey(p.src, p.dst, p.src_port, p.dst_port, p.protocol,
                               p.l3 == L3Kind::kIpv4, zone, src_is_a);
    const CanonicalKey made = MakeKey(p, zone);
    FUZZ_CHECK(SameKey(made.key, key) && made.src_is_a == src_is_a);
    // The reverse packet has the same key from the other side.
    ParsedFlowPacket rev = p;
    std::swap(rev.src, rev.dst);
    std::swap(rev.src_port, rev.dst_port);
    const CanonicalKey made_rev = MakeKey(rev, zone);
    FUZZ_CHECK(SameKey(made_rev.key, key));
    FUZZ_CHECK(made_rev.src_is_a != src_is_a || (p.src == p.dst && p.src_port == p.dst_port));

    const auto it = flows_.find(key);
    if (it != flows_.end()) {
      Flow &f = it->second;
      const Direction dir = src_is_a == f.initiator_is_a ? Direction::kOriginal
                                                          : Direction::kReply;
      if (p.l4 == L4Kind::kTcp) {
        const uint8_t next = ct_internal::kTcpTable[static_cast<int>(dir)][ClassOf(p.tcp_flags)]
                                                   [static_cast<int>(f.tcp)];
        if (next == ct_internal::kIv) {
          ExpectNone(r, TrackStatus::kInvalid, dir);
          return;
        }
        if (next == static_cast<uint8_t>(TcpState::kSynSent) &&
            (f.tcp == TcpState::kTimeWait || f.tcp == TcpState::kClose)) {
          if (!may_create) {
            ExpectNone(r, TrackStatus::kInvalid, dir);
            return;
          }
          // A new connection on the old tuple: nothing of the old one stays.
          f.tcp = TcpState::kSynSent;
          f.initiator_is_a = src_is_a;
          f.replied = false;
          f.user = 0;
          f.deadline = now_ + Timeout(p.l4, f);
          ExpectFlow(r, TrackStatus::kNew, Direction::kOriginal, f);
          FUZZ_CHECK(ct_->Find(made_rev.key) == r.entry);
          return;
        }
        if (next != ct_internal::kIg) {
          f.tcp = static_cast<TcpState>(next);
        }
      }
      if (dir == Direction::kReply) {
        f.replied = true;
      }
      f.deadline = now_ + Timeout(p.l4, f);
      ExpectFlow(r, TrackStatus::kExisting, dir, f);
      FUZZ_CHECK(ct_->Find(made_rev.key) == r.entry);
      return;
    }

    // No connection: may this packet start one?
    TcpState tcp = TcpState::kNone;
    if (p.l4 == L4Kind::kTcp) {
      const Cls c = ClassOf(p.tcp_flags);
      if (c == kSyn) {
        tcp = TcpState::kSynSent;
      } else if (c == kAck && policy_.tcp_pickup) {
        tcp = TcpState::kEstablished;
      } else {
        ExpectNone(r, TrackStatus::kInvalid);
        return;
      }
    } else if (icmp && !(icmp4 ? t == 8 : t == 128)) {
      ExpectNone(r, TrackStatus::kInvalid);  // a reply with no request
      return;
    }
    if (!may_create) {
      ExpectNone(r, TrackStatus::kInvalid);
      return;
    }
    if (flows_.size() == capacity_) {
      ExpectNone(r, TrackStatus::kFull);
      return;
    }
    FUZZ_CHECK(r.status == TrackStatus::kNew && r.handle != flow::kNoFlow);
    for (const auto &[k, other] : flows_) {
      FUZZ_CHECK(other.handle != r.handle);
    }
    for (const auto &[h, k] : dead_) {
      FUZZ_CHECK(h != r.handle);  // a dead handle never names a new flow
    }
    Flow f{r.handle, tcp, src_is_a, false, 0, 0};
    f.deadline = now_ + Timeout(p.l4, f);
    Flow &stored = flows_.emplace(key, f).first->second;
    ExpectFlow(r, TrackStatus::kNew, Direction::kOriginal, stored);
    FUZZ_CHECK(ct_->Find(made_rev.key) == r.entry);
  }

  std::unique_ptr<Ct> ct_;
  TimeoutPolicy policy_;
  size_t capacity_;
  unsigned shift_;
  uint64_t now_;
  uint32_t stamp_ = 0;
  Flows flows_;
  std::deque<std::pair<flow::FlowHandle, CtKey>> dead_;
};

void RunTracker(FuzzInput &in) {
  const size_t capacity = in.U8() % 16 + 1;
  const uint8_t config = in.U8();
  constexpr unsigned kShifts[4] = {0, 0, 1, 3};
  const uint64_t start = in.U16();
  Tracker t(capacity, Policy((config & 4) != 0), start, kShifts[config & 3]);
  for (int step = 0; step < 256 && !in.empty(); step++) {
    const uint8_t op = in.U8();
    t.Advance(in.U8());
    const uint16_t zone = (op >> 3) & 1;
    switch (op & 7) {
      case 0:
      case 1:
      case 2:
      case 3: {
        const Built b = BuildTemplate(in);
        CheckParse(b.frame, 0);
        t.Track(b.frame, b.expect, true, zone, (op & 0x30) != 0x30);
        break;
      }
      case 4:
        t.Expire((op & 8) ? std::numeric_limits<size_t>::max() : (op >> 4) & 3);
        break;
      case 5:
        t.Remove(in.U8());
        break;
      case 6: {
        const uint8_t pick = in.U8();
        t.SetDeadline(pick, in.U8());
        break;
      }
      default: {
        const auto raw = in.Bytes(in.U8());
        const std::vector<uint8_t> frame(raw.begin(), raw.end());
        CheckParse(frame, 0);
        t.Track(frame, ParseStatus::kOk, false, zone, true);
        break;
      }
    }
    t.CheckAll();
  }
}

}  // namespace
}  // namespace bess::conntrack

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  bess::fuzz::FuzzInput in(data, size);
  if ((in.U8() & 1) == 0) {
    bess::conntrack::RunParse(in);
  } else {
    bess::conntrack::RunTracker(in);
  }
  return 0;
}
