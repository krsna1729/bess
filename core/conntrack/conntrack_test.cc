// SPDX-License-Identifier: BSD-3-Clause

// Connection tracking (M17, D-067): the checked parser and the tracker's state
// machines, as traces of real packet sequences.

#include "conntrack/conntrack.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

namespace bess::conntrack {
namespace {

// -- frame builders ---------------------------------------------------------------

struct Ep {
  uint32_t ip;  // IPv4, host order
  uint16_t port;
};

void Put16(std::vector<uint8_t> &f, size_t off, uint16_t v) {
  f[off] = static_cast<uint8_t>(v >> 8);
  f[off + 1] = static_cast<uint8_t>(v);
}
void Put32(std::vector<uint8_t> &f, size_t off, uint32_t v) {
  Put16(f, off, static_cast<uint16_t>(v >> 16));
  Put16(f, off + 2, static_cast<uint16_t>(v));
}

// Ethernet (+ `vlans` tags) + IPv4 (`options` bytes of options) + `l4` bytes.
std::vector<uint8_t> Ipv4Frame(Ep s, Ep d, uint8_t proto, std::vector<uint8_t> l4, int vlans = 0,
                               size_t options = 0, uint16_t frag = 0) {
  std::vector<uint8_t> f(12, 0);
  for (int v = 0; v < vlans; v++) {
    f.push_back(v == 0 && vlans == 2 ? 0x88 : 0x81);
    f.push_back(v == 0 && vlans == 2 ? 0xa8 : 0x00);
    f.push_back(0);
    f.push_back(static_cast<uint8_t>(10 + v));
  }
  f.push_back(0x08);
  f.push_back(0x00);
  const size_t ip = f.size();
  const size_t ihl = 20 + options;
  f.resize(ip + ihl, 0);
  f[ip] = static_cast<uint8_t>(0x40 | (ihl / 4));
  Put16(f, ip + 2, static_cast<uint16_t>(ihl + l4.size()));
  Put16(f, ip + 6, frag);
  f[ip + 8] = 64;
  f[ip + 9] = proto;
  Put32(f, ip + 12, s.ip);
  Put32(f, ip + 16, d.ip);
  if (proto == 6 || proto == 17) {
    Put16(l4, 0, s.port);
    Put16(l4, 2, d.port);
  }
  f.insert(f.end(), l4.begin(), l4.end());
  return f;
}

std::vector<uint8_t> Tcp(Ep s, Ep d, uint8_t flags) {
  std::vector<uint8_t> l4(20, 0);
  l4[12] = 5 << 4;
  l4[13] = flags;
  return Ipv4Frame(s, d, 6, l4);
}
std::vector<uint8_t> Udp(Ep s, Ep d, size_t payload = 4) {
  std::vector<uint8_t> l4(8 + payload, 0);
  Put16(l4, 4, static_cast<uint16_t>(8 + payload));
  return Ipv4Frame(s, d, 17, l4);
}
std::vector<uint8_t> Icmp(Ep s, Ep d, uint8_t type, uint16_t id, std::vector<uint8_t> quote = {}) {
  std::vector<uint8_t> l4(8, 0);
  l4[0] = type;
  Put16(l4, 4, id);
  l4.insert(l4.end(), quote.begin(), quote.end());
  return Ipv4Frame(s, d, 1, l4);
}

ParsedFlowPacket Parse(const std::vector<uint8_t> &f) {
  ParsedFlowPacket p;
  EXPECT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  return p;
}

constexpr Ep kClient{0x0a000001, 40000}, kServer{0x0a000002, 80};
constexpr uint8_t kSA = kTcpSyn | kTcpAck, kFA = kTcpFin | kTcpAck, kA = kTcpAck;

using Ct = Conntrack<>;
std::unique_ptr<Ct> Make(TimeoutPolicy policy = {}, size_t capacity = 64) {
  return Ct::Create(capacity, policy).value();
}

// Tracks a packet built by the caller; returns the result.
Ct::Result Send(Ct &ct, const std::vector<uint8_t> &f, uint64_t now, bool may_create = true) {
  ParsedFlowPacket p = Parse(f);
  return ct.Track(f, p, now, 0, may_create);
}

// -- parser -------------------------------------------------------------------------

TEST(PacketParseTest, VlanTagsOptionsAndPorts) {
  for (int vlans = 0; vlans <= 2; vlans++) {
    for (size_t options : {0u, 4u, 40u}) {
      std::vector<uint8_t> l4(20, 0);
      l4[12] = 5 << 4;
      l4[13] = kSA;
      const auto f = Ipv4Frame(kClient, kServer, 6, l4, vlans, options);
      ParsedFlowPacket p;
      ASSERT_EQ(ParseStatus::kOk, ParseFrame(f, p)) << vlans << " " << options;
      EXPECT_EQ(L3Kind::kIpv4, p.l3);
      EXPECT_EQ(L4Kind::kTcp, p.l4);
      EXPECT_EQ(14u + 4u * vlans, p.l3_offset);
      EXPECT_EQ(p.l3_offset + 20u + options, p.l4_offset);
      EXPECT_EQ(40000, p.src_port);
      EXPECT_EQ(80, p.dst_port);
      EXPECT_EQ(kSA, p.tcp_flags);
    }
  }
}

TEST(PacketParseTest, Ipv6ExtensionHeadersAndFragments) {
  auto v6 = [](std::vector<uint8_t> ext_chain, uint8_t first_next, uint8_t l4proto) {
    std::vector<uint8_t> f(12, 0);
    f.push_back(0x86);
    f.push_back(0xdd);
    const size_t ip = f.size();
    f.resize(ip + 40, 0);
    f[ip] = 0x60;
    f[ip + 6] = first_next;
    f[ip + 23] = 1;  // src ::1
    f[ip + 39] = 2;  // dst ::2
    f.insert(f.end(), ext_chain.begin(), ext_chain.end());
    std::vector<uint8_t> udp(12, 0);
    udp[0] = 0x13;
    udp[1] = 0x88;  // 5000
    udp[3] = 53;
    udp[5] = 12;
    if (l4proto == 17) f.insert(f.end(), udp.begin(), udp.end());
    Put16(f, ip + 4, static_cast<uint16_t>(f.size() - ip - 40));
    return f;
  };
  // Hop-by-hop (8 bytes) -> destination options (16 bytes) -> UDP.
  std::vector<uint8_t> chain = {60, 0, 0, 0, 0, 0, 0, 0, 17, 1};
  chain.resize(8 + 16, 0);
  ParsedFlowPacket p;
  auto f = v6(chain, 0, 17);
  ASSERT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  EXPECT_EQ(L3Kind::kIpv6, p.l3);
  EXPECT_EQ(L4Kind::kUdp, p.l4);
  EXPECT_EQ(14u + 40u + 24u, p.l4_offset);
  EXPECT_EQ(5000, p.src_port);
  EXPECT_EQ(53, p.dst_port);
  // A non-initial fragment has no L4; an initial one is parsed through.
  std::vector<uint8_t> frag = {17, 0, 0x00, 0x08, 0, 0, 0, 1};  // offset 1
  f = v6(frag, 44, 0);
  EXPECT_EQ(ParseStatus::kFragment, ParseFrame(f, p));
  frag[3] = 0x01;  // offset 0, M flag
  f = v6(frag, 44, 17);
  EXPECT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  EXPECT_EQ(53, p.dst_port);
  // An extension header claiming more than the payload, followed by a
  // protocol whose header is not checked (GRE): only the length check can
  // catch it.
  chain = {47, 200, 0, 0, 0, 0, 0, 0};
  f = v6(chain, 0, 47);
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
  // A valid IPv6 packet under EtherType IPv4.
  chain = {17, 0, 0, 0, 0, 0, 0, 0};
  f = v6(chain, 0, 17);
  ASSERT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  f[12] = 0x08;
  f[13] = 0x00;
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
}

// Every truncation of a valid frame is rejected or parsed within bounds: never
// a read past the frame (the sanitizer lanes run this too).
TEST(PacketParseTest, TruncationsAndLiesAreMalformed) {
  const auto full = Tcp(kClient, kServer, kSA);
  for (size_t n = 0; n < full.size(); n++) {
    std::vector<uint8_t> cut(full.begin(), full.begin() + n);
    ParsedFlowPacket p;
    const ParseStatus s = ParseFrame(cut, p);
    ASSERT_NE(ParseStatus::kOk, s) << "truncated to " << n;
  }
  ParsedFlowPacket p;
  auto f = Tcp(kClient, kServer, kA);
  f[14] = 0x44;  // IHL 4
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
  f = Ipv4Frame(kClient, kServer, 47, std::vector<uint8_t>(8, 0));  // GRE: L4 unchecked
  ASSERT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  f[14] = 0x44;
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p)) << "IHL below 5 with an unchecked L4";
  f = Tcp(kClient, kServer, kA);
  Put16(f, 16, 400);  // total length beyond the frame
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
  f = Tcp(kClient, kServer, kA);
  f[14 + 20 + 12] = 4 << 4;  // TCP data offset 4
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
  f = Udp(kClient, kServer);
  Put16(f, 14 + 20 + 4, 7);  // UDP length below its header
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
  f = Tcp(kClient, kServer, kA);
  f[14] = 0x65;  // EtherType IPv4, version 6
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
  f = Tcp(kClient, kServer, kA);
  f[12] = 0x08;
  f[13] = 0x06;  // ARP
  EXPECT_EQ(ParseStatus::kNotIp, ParseFrame(f, p));
  std::vector<uint8_t> l4(20, 0);
  f = Ipv4Frame(kClient, kServer, 6, l4, 0, 0, 0x0010);  // offset 16*8
  EXPECT_EQ(ParseStatus::kFragment, ParseFrame(f, p));
}

// The first fragment of a datagram holds the L4 header but not all of it:
// the UDP length and TCP options may extend into later fragments.
TEST(PacketParseTest, FirstFragmentsAreParsed) {
  std::vector<uint8_t> udp(8 + 92, 0);
  Put16(udp, 4, 3000);  // the whole datagram's length
  auto f = Ipv4Frame(kClient, kServer, 17, udp, 0, 0, 0x2000);  // MF, offset 0
  ParsedFlowPacket p;
  ASSERT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  EXPECT_TRUE(p.first_fragment);
  EXPECT_EQ(40000, p.src_port);
  f = Ipv4Frame(kClient, kServer, 17, udp);  // the same, unfragmented: a lie
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
  std::vector<uint8_t> tcp(20, 0);
  tcp[12] = 15 << 4;  // 60-byte header, 20 present
  f = Ipv4Frame(kClient, kServer, 6, tcp, 0, 0, 0x2000);
  ASSERT_EQ(ParseStatus::kOk, ParseFrame(f, p));
  EXPECT_EQ(L4Kind::kTcp, p.l4);
  f = Ipv4Frame(kClient, kServer, 6, tcp);
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(f, p));
  // IPv6: a fragment header (offset 0, M) in front of a long UDP datagram.
  std::vector<uint8_t> v6(14 + 40 + 8 + 8, 0);
  Put16(v6, 12, 0x86dd);
  v6[14] = 0x60;
  Put16(v6, 18, 16);
  v6[20] = 44;
  v6[14 + 40] = 17;
  v6[14 + 40 + 3] = 1;  // M
  Put16(v6, 14 + 48 + 4, 2000);
  ASSERT_EQ(ParseStatus::kOk, ParseFrame(v6, p));
  EXPECT_TRUE(p.first_fragment);
  v6[14 + 40 + 3] = 0;  // M clear: an atomic fragment, a whole packet (RFC 6946)
  EXPECT_EQ(ParseStatus::kMalformed, ParseFrame(v6, p)) << "the UDP length now must fit";
  Put16(v6, 14 + 48 + 4, 8);
  ASSERT_EQ(ParseStatus::kOk, ParseFrame(v6, p));
  EXPECT_FALSE(p.first_fragment);
}

// -- TCP --------------------------------------------------------------------------

TEST(ConntrackTest, TcpHandshakeDataAndOrderlyClose) {
  auto ct = Make();
  auto r = Send(*ct, Tcp(kClient, kServer, kTcpSyn), 0);
  ASSERT_EQ(TrackStatus::kNew, r.status);
  EXPECT_EQ(TcpState::kSynSent, r.entry->tcp);
  EXPECT_EQ(TrackStatus::kExisting, Send(*ct, Tcp(kClient, kServer, kTcpSyn), 1).status)
      << "a retransmitted SYN";
  EXPECT_EQ(TcpState::kSynSent, r.entry->tcp);
  r = Send(*ct, Tcp(kServer, kClient, kSA), 2);
  ASSERT_EQ(TrackStatus::kExisting, r.status);
  EXPECT_EQ(Direction::kReply, r.direction);
  EXPECT_EQ(TcpState::kSynRecv, r.entry->tcp);
  EXPECT_TRUE(r.entry->replied);
  EXPECT_EQ(TcpState::kSynRecv, Send(*ct, Tcp(kServer, kClient, kSA), 3).entry->tcp)
      << "a retransmitted SYN-ACK";
  r = Send(*ct, Tcp(kClient, kServer, kA), 4);
  EXPECT_EQ(Direction::kOriginal, r.direction);
  EXPECT_EQ(TcpState::kEstablished, r.entry->tcp);
  EXPECT_EQ(TcpState::kEstablished, Send(*ct, Tcp(kServer, kClient, kA), 5).entry->tcp);
  // Close: client FIN, server ACK, server FIN, client ACK.
  EXPECT_EQ(TcpState::kFinWait, Send(*ct, Tcp(kClient, kServer, kFA), 6).entry->tcp);
  EXPECT_EQ(TcpState::kCloseWait, Send(*ct, Tcp(kServer, kClient, kA), 7).entry->tcp);
  EXPECT_EQ(TcpState::kLastAck, Send(*ct, Tcp(kServer, kClient, kFA), 8).entry->tcp);
  r = Send(*ct, Tcp(kClient, kServer, kA), 9);
  EXPECT_EQ(TcpState::kTimeWait, r.entry->tcp);
  // TIME_WAIT lasts 120 ticks from the last packet.
  EXPECT_EQ(0u, ct->Expire(9 + 119, ~size_t{0}));
  EXPECT_EQ(1u, ct->Expire(9 + 121, ~size_t{0}));
  EXPECT_EQ(0u, ct->size());
}

TEST(ConntrackTest, TcpSimultaneousCloseResetAndSimultaneousOpen) {
  auto ct = Make();
  Send(*ct, Tcp(kClient, kServer, kTcpSyn), 0);
  Send(*ct, Tcp(kServer, kClient, kSA), 0);
  Send(*ct, Tcp(kClient, kServer, kA), 0);
  EXPECT_EQ(TcpState::kFinWait, Send(*ct, Tcp(kClient, kServer, kFA), 1).entry->tcp);
  EXPECT_EQ(TcpState::kLastAck, Send(*ct, Tcp(kServer, kClient, kFA), 1).entry->tcp)
      << "both FINs crossed";
  EXPECT_EQ(TcpState::kTimeWait, Send(*ct, Tcp(kClient, kServer, kA), 1).entry->tcp);

  const Ep c2{0x0a000003, 50000};
  Send(*ct, Tcp(c2, kServer, kTcpSyn), 10);
  Send(*ct, Tcp(kServer, c2, kSA), 10);
  Send(*ct, Tcp(c2, kServer, kA), 10);
  auto r = Send(*ct, Tcp(kServer, c2, kTcpRst), 11);
  EXPECT_EQ(TcpState::kClose, r.entry->tcp);
  EXPECT_EQ(1u, ct->Expire(11 + 11, ~size_t{0})) << "CLOSE lasts 10";

  const Ep c3{0x0a000004, 60000};
  Send(*ct, Tcp(c3, kServer, kTcpSyn), 20);
  r = Send(*ct, Tcp(kServer, c3, kTcpSyn), 20);
  EXPECT_EQ(TcpState::kSynSent2, r.entry->tcp) << "simultaneous open";
  EXPECT_EQ(TcpState::kSynRecv, Send(*ct, Tcp(c3, kServer, kSA), 20).entry->tcp);
}

// A SYN on a closed tuple opens a new connection whose initiator is the SYN's
// sender, in either direction, and keeps nothing of the old one (Linux kills
// the old entry and re-evaluates the packet).
TEST(ConntrackTest, TcpReopenAfterTimeWaitOrClose) {
  struct Mark {
    uint32_t verdict;
  };
  auto ct = Conntrack<Mark>::Create(64).value();
  auto send = [&](const std::vector<uint8_t> &f, uint64_t now) {
    ParsedFlowPacket p = Parse(f);
    return ct->Track(f, p, now);
  };
  auto close = [&](uint64_t now) {
    send(Tcp(kClient, kServer, kTcpSyn), now);
    send(Tcp(kServer, kClient, kSA), now);
    send(Tcp(kClient, kServer, kA), now);
    send(Tcp(kClient, kServer, kFA), now);
    send(Tcp(kServer, kClient, kFA), now);
    return send(Tcp(kClient, kServer, kA), now);
  };
  auto r = close(0);
  ASSERT_EQ(TcpState::kTimeWait, r.entry->tcp);
  r.entry->user.verdict = 7;
  // The server opens the reverse way on the same tuple.
  r = send(Tcp(kServer, kClient, kTcpSyn), 1);
  ASSERT_EQ(TrackStatus::kNew, r.status);
  EXPECT_EQ(Direction::kOriginal, r.direction);
  EXPECT_EQ(TcpState::kSynSent, r.entry->tcp);
  EXPECT_FALSE(r.entry->replied);
  EXPECT_EQ(0u, r.entry->user.verdict) << "nothing of the old connection";
  r = send(Tcp(kClient, kServer, kSA), 2);
  ASSERT_EQ(TrackStatus::kExisting, r.status) << "the client's SYN-ACK is the reply";
  EXPECT_EQ(Direction::kReply, r.direction);
  EXPECT_EQ(TcpState::kSynRecv, r.entry->tcp);
  // A reset connection reopened in the original direction.
  send(Tcp(kServer, kClient, kA), 3);
  send(Tcp(kClient, kServer, kTcpRst), 4);
  r = send(Tcp(kServer, kClient, kTcpSyn), 5);
  EXPECT_EQ(TrackStatus::kNew, r.status);
  EXPECT_FALSE(r.entry->replied);
  EXPECT_EQ(1u, ct->size());
}

TEST(ConntrackTest, TcpInvalidSequencesChangeNothing) {
  auto ct = Make();
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Tcp(kClient, kServer, kA), 0).status)
      << "mid-stream ACK, pickup off";
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Tcp(kClient, kServer, kSA), 0).status)
      << "a SYN-ACK starts nothing";
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Tcp(kClient, kServer, kTcpFin), 0).status);
  EXPECT_EQ(0u, ct->size());
  auto r = Send(*ct, Tcp(kClient, kServer, kTcpSyn), 0);
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Tcp(kClient, kServer, kSA), 1).status)
      << "SYN-ACK in the original direction";
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Tcp(kClient, kServer, 0), 1).status) << "no flags";
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Tcp(kServer, kClient, kTcpFin), 1).status)
      << "FIN before the handshake";
  EXPECT_EQ(TcpState::kSynSent, r.entry->tcp);
  EXPECT_FALSE(r.entry->replied);

  TimeoutPolicy loose;
  loose.tcp_pickup = true;
  auto picked = Make(loose);
  r = Send(*picked, Tcp(kClient, kServer, kA), 0);
  EXPECT_EQ(TrackStatus::kNew, r.status);
  EXPECT_EQ(TcpState::kEstablished, r.entry->tcp);
}

// -- UDP, ICMP, other -------------------------------------------------------------

TEST(ConntrackTest, UdpCreateRefreshAndExpire) {
  auto ct = Make();
  auto r = Send(*ct, Udp(kClient, {0x08080808, 53}), 0);
  ASSERT_EQ(TrackStatus::kNew, r.status);
  EXPECT_EQ(0u, ct->Expire(29, ~size_t{0}));
  Send(*ct, Udp(kClient, {0x08080808, 53}), 20);  // refresh, still unreplied: 30
  EXPECT_EQ(0u, ct->Expire(49, ~size_t{0}));
  EXPECT_EQ(1u, ct->Expire(51, ~size_t{0})) << "unreplied: 30 from the last packet";

  Send(*ct, Udp(kClient, {0x08080808, 53}), 100);
  r = Send(*ct, Udp({0x08080808, 53}, kClient), 101);
  EXPECT_EQ(Direction::kReply, r.direction);
  EXPECT_TRUE(r.entry->replied);
  EXPECT_EQ(0u, ct->Expire(101 + 119, ~size_t{0})) << "replied: 120";
  EXPECT_EQ(1u, ct->Expire(101 + 121, ~size_t{0}));
}

TEST(ConntrackTest, IcmpEchoAndErrorsAboutTrackedConnections) {
  auto ct = Make();
  const Ep host{0x0a000009, 0};
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Icmp(host, kClient, 0, 7), 0).status)
      << "an echo reply without a request";
  auto req = Send(*ct, Icmp(kClient, host, 8, 7), 0);
  ASSERT_EQ(TrackStatus::kNew, req.status);
  auto rep = Send(*ct, Icmp(host, kClient, 0, 7), 1);
  EXPECT_EQ(TrackStatus::kExisting, rep.status);
  EXPECT_EQ(Direction::kReply, rep.direction);
  EXPECT_EQ(req.handle, rep.handle);
  EXPECT_EQ(TrackStatus::kUntracked, Send(*ct, Icmp(kClient, host, 13, 1), 1).status)
      << "timestamp: neither echo nor error";

  // A tracked UDP flow, then a router's port-unreachable quoting it.
  const Ep dns{0x08080808, 53};
  auto udp = Send(*ct, Udp(kClient, dns), 2);
  std::vector<uint8_t> quoted = Udp(kClient, dns);
  quoted.erase(quoted.begin(), quoted.begin() + 14);  // the IP packet
  quoted.resize(28);                                 // header + 8 bytes
  auto err = Send(*ct, Icmp(dns, kClient, 3, 0, quoted), 3);
  EXPECT_EQ(TrackStatus::kRelated, err.status);
  EXPECT_EQ(udp.handle, err.handle);
  EXPECT_EQ(Direction::kReply, err.direction);
  EXPECT_FALSE(udp.entry->replied) << "an error is not a reply";
  // An error about an unknown flow, and a quote too short.
  std::vector<uint8_t> other = Udp(kClient, {0x01010101, 53});
  other.erase(other.begin(), other.begin() + 14);
  other.resize(28);
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Icmp(dns, kClient, 3, 0, other), 3).status);
  quoted.resize(22);
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Icmp(dns, kClient, 3, 0, quoted), 3).status);
  // A quoted non-initial fragment has no ports, and a quoted ICMP error no
  // identifier: neither names a connection.
  std::vector<uint8_t> frag = Udp(kClient, dns);
  frag.erase(frag.begin(), frag.begin() + 14);
  frag.resize(28);
  Put16(frag, 6, 0x0010);
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Icmp(dns, kClient, 3, 0, frag), 3).status);
  std::vector<uint8_t> inner = Icmp(kClient, host, 3, 7);  // an error, id field 7
  inner.erase(inner.begin(), inner.begin() + 14);
  inner.resize(28);
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Icmp(host, kClient, 11, 0, inner), 4).status);
  std::vector<uint8_t> echo = Icmp(kClient, host, 8, 7);  // the tracked echo
  echo.erase(echo.begin(), echo.begin() + 14);
  echo.resize(28);
  EXPECT_EQ(TrackStatus::kRelated, Send(*ct, Icmp(host, kClient, 11, 0, echo), 4).status);
}

// -- keys, zones, policy hooks, capacity --------------------------------------------

TEST(ConntrackTest, BothDirectionsShareOneKeyAndZonesSeparate) {
  std::mt19937 rng(17);
  for (int i = 0; i < 2000; i++) {
    const Ep a{static_cast<uint32_t>(rng()), static_cast<uint16_t>(rng())};
    const Ep b{rng() % 4 ? static_cast<uint32_t>(rng()) : a.ip, static_cast<uint16_t>(rng())};
    const auto fwd = MakeKey(Parse(Udp(a, b)), 3);
    const auto rev = MakeKey(Parse(Udp(b, a)), 3);
    ASSERT_EQ(0, std::memcmp(&fwd.key, &rev.key, sizeof(CtKey)));
    if (a.ip != b.ip || a.port != b.port) {
      ASSERT_NE(fwd.src_is_a, rev.src_is_a);
    }
  }
  // IPv6 endpoints that differ only in their low 8 address bytes, with equal
  // ports: the order must look at the whole address.
  for (int i = 0; i < 200; i++) {
    ParsedFlowPacket x{};
    x.l3 = L3Kind::kIpv6;
    x.l4 = L4Kind::kUdp;
    x.protocol = 17;
    x.src[0] = x.dst[0] = 0x20;
    x.src[15] = static_cast<uint8_t>(rng());
    x.dst[15] = static_cast<uint8_t>(x.src[15] + 1 + rng() % 200);
    x.src[9] = static_cast<uint8_t>(rng());
    x.dst[9] = static_cast<uint8_t>(rng());
    x.src_port = x.dst_port = 4789;
    ParsedFlowPacket y = x;
    std::swap(y.src, y.dst);
    const auto fwd = MakeKey(x, 0), rev = MakeKey(y, 0);
    ASSERT_EQ(0, std::memcmp(&fwd.key, &rev.key, sizeof(CtKey)));
    ASSERT_NE(fwd.src_is_a, rev.src_is_a);
  }
  auto ct = Make();
  // TCP and UDP on one tuple are two connections.
  EXPECT_EQ(TrackStatus::kNew, Send(*ct, Tcp(kClient, kServer, kTcpSyn), 0).status);
  EXPECT_EQ(TrackStatus::kNew, Send(*ct, Udp(kClient, kServer), 0).status);
  EXPECT_EQ(2u, ct->size());
  ct = Make();
  const auto f = Udp(kClient, kServer);
  ParsedFlowPacket p = Parse(f);
  EXPECT_EQ(TrackStatus::kNew, ct->Track(f, p, 0, /*zone=*/1).status);
  EXPECT_EQ(TrackStatus::kNew, ct->Track(f, p, 0, /*zone=*/2).status);
  EXPECT_EQ(TrackStatus::kExisting, ct->Track(f, p, 0, /*zone=*/1).status);
  EXPECT_EQ(2u, ct->size());
}

TEST(ConntrackTest, PolicyRefusalCapacityAndControl) {
  auto ct = Make({}, 2);
  EXPECT_EQ(TrackStatus::kInvalid, Send(*ct, Udp(kClient, kServer), 0, /*may_create=*/false).status);
  EXPECT_EQ(0u, ct->size());
  auto a = Send(*ct, Udp(kClient, kServer), 0);
  Send(*ct, Udp(kServer, {1, 1}), 0);
  EXPECT_EQ(TrackStatus::kFull, Send(*ct, Udp({2, 2}, {3, 3}), 0).status);
  // Removal and a policy deadline.
  EXPECT_TRUE(ct->SetDeadline(a.handle, 1000));
  EXPECT_EQ(1u, ct->Expire(500, ~size_t{0})) << "the other one (30)";
  EXPECT_NE(nullptr, ct->Lookup(a.handle));
  EXPECT_TRUE(ct->Remove(a.handle));
  EXPECT_FALSE(ct->Remove(a.handle));
  EXPECT_EQ(0u, ct->Expire(2000, ~size_t{0}));
  EXPECT_EQ(0u, ct->size());
  // An unparsed packet is untracked.
  EXPECT_EQ(TrackStatus::kUntracked, ct->Track({}, ParsedFlowPacket{}, 0).status);
}


// TrackBatch answers exactly as Track packet by packet: a random stream over
// a small set of endpoints, so batches create, use and close connections
// within themselves, with echo, ICMP errors, untracked and unparsed packets
// mixed in, and a table small enough to fill.
TEST(ConntrackTest, TrackBatchMatchesTrackPacketByPacket) {
  for (const uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
    SCOPED_TRACE(seed);
    auto scalar = Conntrack<>::Create(48).value();
    auto batched = Conntrack<>::Create(48).value();
    // Both bodies, whatever the footprint would choose.
    batched->SetBatchBodyForTesting(seed % 2 ? dataplane::LookupBody::kStaged
                                             : dataplane::LookupBody::kPlain);
    std::mt19937 rng(seed);
    const uint8_t kFlags[] = {kTcpSyn, kTcpSyn | kTcpAck, kTcpAck, kTcpFin | kTcpAck,
                              kTcpRst, kTcpAck | ct_internal::kTcpUrg, kTcpSyn | kTcpFin};
    uint64_t now = 1;
    for (int round = 0; round < 400; round++) {
      const size_t n = 1 + rng() % Conntrack<>::kMaxBatch;
      std::vector<std::vector<uint8_t>> frames;
      std::vector<ParsedFlowPacket> parsed(n);
      for (size_t i = 0; i < n; i++) {
        const Ep a{static_cast<uint32_t>(0x0a000000u + rng() % 6), static_cast<uint16_t>(1000 + rng() % 4)};
        const Ep b{static_cast<uint32_t>(0xc0a80000u + rng() % 3), static_cast<uint16_t>(80 + rng() % 2)};
        const bool fwd = rng() % 2;
        const Ep s = fwd ? a : b, d = fwd ? b : a;
        switch (rng() % 8) {
          case 0: case 1: case 2: frames.push_back(Tcp(s, d, kFlags[rng() % 7])); break;
          case 3: case 4: frames.push_back(Udp(s, d)); break;
          case 5: frames.push_back(Icmp(s, d, rng() % 2 ? 8 : 0, static_cast<uint16_t>(rng() % 3))); break;
          case 6: {
            std::vector<uint8_t> q = Udp(d, s);
            q.erase(q.begin(), q.begin() + 14);
            q.resize(28);
            frames.push_back(Icmp(s, d, 3, 0, q));
            break;
          }
          default: frames.push_back(Icmp(s, d, 13, 1)); break;  // untracked
        }
        if (ParseFrame(frames.back(), parsed[i]) != ParseStatus::kOk) {
          parsed[i] = ParsedFlowPacket{};
        }
      }
      if (rng() % 5 == 0) {
        parsed[rng() % n] = ParsedFlowPacket{};  // an unparsed packet
      }
      std::vector<std::span<const uint8_t>> spans(frames.begin(), frames.end());
      std::vector<Conntrack<>::Result> got(n);
      batched->TrackBatch(spans, parsed, now, got);
      // Entries are read once both sides have run the whole batch: a later
      // packet of the batch may move the same connection on.
      std::vector<Conntrack<>::Result> want(n);
      for (size_t i = 0; i < n; i++) {
        want[i] = scalar->Track(frames[i], parsed[i], now);
      }
      for (size_t i = 0; i < n; i++) {
        ASSERT_EQ(want[i].status, got[i].status) << "round " << round << " packet " << i;
        ASSERT_EQ(want[i].direction, got[i].direction) << "round " << round << " packet " << i;
        ASSERT_EQ(want[i].handle, got[i].handle) << "round " << round << " packet " << i;
        ASSERT_EQ(want[i].entry != nullptr, got[i].entry != nullptr);
        if (want[i].entry != nullptr) {
          ASSERT_EQ(want[i].entry->tcp, got[i].entry->tcp) << "round " << round << " packet " << i;
          ASSERT_EQ(want[i].entry->replied, got[i].entry->replied);
        }
      }
      now += rng() % 3 == 0 ? 50'000'000'000ull : 1000;
      ASSERT_EQ(scalar->Expire(now, ~size_t{0}), batched->Expire(now, ~size_t{0}));
      ASSERT_EQ(scalar->size(), batched->size());
    }
  }
}

}  // namespace
}  // namespace bess::conntrack
