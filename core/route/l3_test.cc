// SPDX-License-Identifier: BSD-3-Clause

// L3 packet mechanics and the neighbor table (M15, D-065).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "route/l3_packet.h"
#include "route/neighbor_table.h"
#include "runtime/runtime_state.h"
#include "testing/allocation_faults.h"
#include "utils/checksum.h"

namespace bess::route {
namespace {

using utils::Ethernet;
using utils::Ipv4;

Ipv4 Header(uint8_t ttl, uint16_t length = 100, uint16_t frag = 0) {
  Ipv4 ip{};
  ip.version = 4;
  ip.header_length = 5;
  ip.length = be16_t(length);
  ip.id = be16_t(0x1234);
  ip.fragment_offset = be16_t(frag);
  ip.ttl = ttl;
  ip.protocol = Ipv4::kUdp;
  ip.src = be32_t(0x0a000001);
  ip.dst = be32_t(0x0a000102);
  ip.checksum = utils::CalculateIpv4NoOptChecksum(ip);
  return ip;
}

TEST(L3PacketTest, TtlDecrementKeepsTheChecksumValid) {
  std::mt19937 rng(1);
  for (int i = 0; i < 2000; i++) {
    Ipv4 ip = Header(static_cast<uint8_t>(2 + rng() % 254));
    ip.protocol = static_cast<uint8_t>(rng());
    ip.src = be32_t(static_cast<uint32_t>(rng()));
    ip.checksum = utils::CalculateIpv4NoOptChecksum(ip);
    const uint8_t ttl = ip.ttl;
    ASSERT_EQ(TtlResult::kForward, DecrementTtl(ip));
    ASSERT_EQ(ttl - 1, ip.ttl);
    ASSERT_TRUE(utils::VerifyIpv4NoOptChecksum(ip)) << "ttl " << int(ttl);
    ASSERT_EQ(utils::CalculateIpv4NoOptChecksum(ip), ip.checksum);
  }
  for (uint8_t ttl : {0, 1}) {
    Ipv4 ip = Header(ttl);
    const Ipv4 before = ip;
    EXPECT_EQ(TtlResult::kExpired, DecrementTtl(ip));
    EXPECT_EQ(0, std::memcmp(&before, &ip, sizeof(ip))) << "an expired header is untouched";
  }
  std::array<uint8_t, 40> v6{};
  v6[7] = 2;
  EXPECT_EQ(TtlResult::kForward, DecrementHopLimit(v6));
  EXPECT_EQ(1, v6[7]);
  EXPECT_EQ(TtlResult::kExpired, DecrementHopLimit(v6));
  EXPECT_EQ(1, v6[7]);
  EXPECT_EQ(TtlResult::kExpired, DecrementHopLimit(std::span<uint8_t>(v6.data(), 39)));
}

TEST(L3PacketTest, MtuCheckReportsWithoutDeciding) {
  EXPECT_EQ(MtuResult::kFits, CheckMtu(Header(64, 1500), 1500));
  EXPECT_EQ(MtuResult::kTooBigFragmentable, CheckMtu(Header(64, 1501), 1500));
  EXPECT_EQ(MtuResult::kFragmentationNeeded, CheckMtu(Header(64, 1501, Ipv4::kDF), 1500));
  EXPECT_EQ(MtuResult::kFits, CheckMtu(Header(64, 9000, Ipv4::kDF), 9000));
}

std::vector<uint8_t> Packet(const Ipv4 &ip, size_t payload) {
  std::vector<uint8_t> p(sizeof(ip) + payload);
  std::memcpy(p.data(), &ip, sizeof(ip));
  for (size_t i = 0; i < payload; i++) {
    p[sizeof(ip) + i] = static_cast<uint8_t>(0xa0 + i);
  }
  return p;
}

TEST(L3PacketTest, IcmpErrorsQuoteTheHeaderAndEightBytes) {
  const std::vector<uint8_t> orig = Packet(Header(1, 120, Ipv4::kDF), 100);
  std::array<uint8_t, 128> out{};
  Icmpv4ErrorSpec spec{IcmpType::kTimeExceeded, kIcmpCodeTtlExceeded, be32_t(0xc0a80001)};
  auto n = BuildIcmpv4Error(orig, spec, out);
  ASSERT_TRUE(n.has_value());
  ASSERT_EQ(20u + 8u + 28u, *n);
  Ipv4 ip;
  std::memcpy(&ip, out.data(), sizeof(ip));
  EXPECT_TRUE(utils::VerifyIpv4NoOptChecksum(ip));
  EXPECT_EQ(Ipv4::kIcmp, ip.protocol);
  EXPECT_EQ(56, ip.length.value());
  EXPECT_EQ(0xc0a80001u, ip.src.value());
  EXPECT_EQ(0x0a000001u, ip.dst.value()) << "back to the original source";
  EXPECT_EQ(11, out[20]);
  EXPECT_EQ(0, out[21]);
  EXPECT_TRUE(utils::VerifyGenericChecksum(out.data() + 20, 8 + 28));
  EXPECT_EQ(0, std::memcmp(out.data() + 28, orig.data(), 28));

  // Fragmentation needed carries the next-hop MTU (RFC 1191).
  Icmpv4ErrorSpec frag{IcmpType::kDestinationUnreachable, kIcmpCodeFragmentationNeeded,
                       be32_t(0xc0a80001), 1400};
  n = BuildIcmpv4Error(orig, frag, out);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(0x05, out[26]);
  EXPECT_EQ(0x78, out[27]);
  EXPECT_TRUE(utils::VerifyGenericChecksum(out.data() + 20, *n - 20));

  // A short original quotes what there is.
  const std::vector<uint8_t> tiny = Packet(Header(1, 23), 3);
  n = BuildIcmpv4Error(tiny, spec, out);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(20u + 8u + 23u, *n);
}

TEST(L3PacketTest, IcmpErrorsAreRefusedWhereRfc1122ForbidsThem) {
  std::array<uint8_t, 128> out{};
  const Icmpv4ErrorSpec spec{IcmpType::kTimeExceeded, 0, be32_t(0xc0a80001)};
  auto refusal = [&](const std::vector<uint8_t> &p, size_t room = 128) {
    auto r = BuildIcmpv4Error(p, spec, std::span<uint8_t>(out.data(), room));
    return r.has_value() ? static_cast<IcmpRefusal>(255) : r.error();
  };
  Ipv4 ip = Header(1);
  ip.protocol = Ipv4::kIcmp;
  std::vector<uint8_t> p = Packet(ip, 8);
  p[20] = 11;  // an ICMP error
  EXPECT_EQ(IcmpRefusal::kIcmpError, refusal(p));
  p[20] = 8;  // an echo request may be answered
  EXPECT_TRUE(BuildIcmpv4Error(p, spec, out).has_value());

  ip = Header(1);
  ip.dst = be32_t(0xe0000001);  // multicast
  EXPECT_EQ(IcmpRefusal::kNotUnicast, refusal(Packet(ip, 8)));
  ip = Header(1);
  ip.dst = be32_t(0xffffffff);
  EXPECT_EQ(IcmpRefusal::kNotUnicast, refusal(Packet(ip, 8)));
  for (const uint32_t src : {0u, 0x7f000001u, 0x7fffffffu, 0x00010203u, 0xe0000001u}) {
    ip = Header(1);
    ip.src = be32_t(src);  // none, loopback, 0/8, multicast
    EXPECT_EQ(IcmpRefusal::kNotUnicast, refusal(Packet(ip, 8))) << std::hex << src;
  }
  ip = Header(1);
  ip.src = be32_t(0x80000001);  // 128.0.0.1: an ordinary host
  EXPECT_TRUE(BuildIcmpv4Error(Packet(ip, 8), spec, out).has_value());
  EXPECT_EQ(IcmpRefusal::kNonInitialFragment, refusal(Packet(Header(1, 100, 185), 8)));
  EXPECT_EQ(IcmpRefusal::kOutputTooSmall, refusal(Packet(Header(1), 8), 55));
  std::vector<uint8_t> bad = Packet(Header(1), 8);
  bad[0] = 0x65;  // version 6
  EXPECT_EQ(IcmpRefusal::kMalformed, refusal(bad));
  bad[0] = 0x44;  // IHL 4
  EXPECT_EQ(IcmpRefusal::kMalformed, refusal(bad));
  EXPECT_EQ(IcmpRefusal::kMalformed, refusal(std::vector<uint8_t>(19, 0x45)));
}

TEST(L3PacketTest, ArpRequestAndReplyRoundTrip) {
  Ethernet::Address me, peer;
  me.FromString("02:00:00:00:00:01");
  peer.FromString("02:00:00:00:00:02");
  std::array<uint8_t, kArpFrameSize> frame{};
  ASSERT_EQ(kArpFrameSize, BuildArpRequest(peer, be32_t(0x0a000002), be32_t(0x0a000001), frame));
  auto request = ParseArp(frame);
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(utils::Arp::kRequest, request->opcode);
  EXPECT_TRUE(frame[0] == 0xff && frame[5] == 0xff) << "broadcast";
  EXPECT_EQ(0x0a000001u, request->target_ip.value());

  std::array<uint8_t, kArpFrameSize> reply_frame{};
  ASSERT_EQ(kArpFrameSize, BuildArpReply(*request, me, reply_frame));
  auto reply = ParseArp(reply_frame);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(utils::Arp::kReply, reply->opcode);
  EXPECT_EQ(me, reply->sender_mac);
  EXPECT_EQ(0x0a000001u, reply->sender_ip.value());
  EXPECT_EQ(peer, reply->target_mac);
  EXPECT_EQ(0x0a000002u, reply->target_ip.value());
  EXPECT_EQ(0, std::memcmp(reply_frame.data(), peer.bytes, 6)) << "unicast to the requester";

  EXPECT_EQ(0u, BuildArpReply(*reply, me, reply_frame)) << "a reply is not answered";
  EXPECT_EQ(0u, BuildArpRequest(me, be32_t(1), be32_t(2),
                                std::span<uint8_t>(frame.data(), kArpFrameSize - 1)));
  reply_frame[12] = 0x08;
  reply_frame[13] = 0x00;  // IPv4, not ARP
  EXPECT_FALSE(ParseArp(reply_frame).has_value());
  EXPECT_FALSE(ParseArp(std::span<const uint8_t>(frame.data(), kArpFrameSize - 1)).has_value());
}

// Neighbor changes are next-hop updates: two routes through one resolved
// neighbor follow its MAC with no route change, and a controller statement
// and an ARP-learned answer produce the same NextHop.
TEST(NeighborTableTest, ResolutionRepublishesBoundNextHopsOnly) {
  LpmRouteTable::Config config;
  auto made = Router::Create("neighbors", config, 16, bess::runtime::runtime().rcu());
  ASSERT_TRUE(made.has_value());
  Router &router = **made;
  NeighborTable neighbors;
  Ethernet::Address if_mac, peer_mac, other_mac;
  if_mac.FromString("02:00:00:00:00:aa");
  peer_mac.FromString("02:00:00:00:00:01");
  other_mac.FromString("02:00:00:00:00:02");
  const NeighborTable::Key gw{dataplane::InterfaceId(3), 0x0a000001};

  const NextHop initial = neighbors.Bind(NextHopId(1), gw, if_mac);
  EXPECT_EQ(NeighborState::kIncomplete, initial.neighbor);
  ASSERT_TRUE(router.SetNextHop(NextHopId(1), initial));
  ASSERT_TRUE(router.SetNextHop(NextHopId(2), neighbors.Bind(NextHopId(2), gw, if_mac)));
  ASSERT_TRUE(router.SetRoute(Ipv4Prefix::Make(0x14000000, 8).value(), NextHopId(1)));
  ASSERT_TRUE(router.SetRoute(Ipv4Prefix::Make(0x1e000000, 8).value(), NextHopId(2)));

  // An ARP reply resolves the gateway.
  auto updates = neighbors.Update(gw, NeighborState::kResolved, peer_mac);
  ASSERT_EQ(2u, updates.size());
  ASSERT_TRUE(Publish(router, updates));
  for (uint32_t dst : {0x14010101u, 0x1e010101u}) {
    const NextHop *hop = router.Resolve(dst);
    ASSERT_NE(nullptr, hop);
    EXPECT_EQ(NeighborState::kResolved, hop->neighbor);
    EXPECT_EQ(peer_mac, hop->dst_mac);
    EXPECT_EQ(if_mac, hop->src_mac);
    EXPECT_EQ(dataplane::InterfaceId(3), hop->egress);
  }
  EXPECT_EQ(2u, router.route_count());

  // The same statement again changes nothing; a new MAC changes both.
  EXPECT_TRUE(neighbors.Update(gw, NeighborState::kResolved, peer_mac).empty());
  updates = neighbors.Update(gw, NeighborState::kResolved, other_mac);
  ASSERT_TRUE(Publish(router, updates));
  EXPECT_EQ(other_mac, router.Resolve(0x14010101)->dst_mac);

  // Unreachable: the hops say so and carry no destination MAC.
  updates = neighbors.Update(gw, NeighborState::kUnreachable, other_mac);
  ASSERT_TRUE(Publish(router, updates));
  EXPECT_EQ(NeighborState::kUnreachable, router.Resolve(0x1e010101)->neighbor);
  EXPECT_EQ(Ethernet::Address{}, router.Resolve(0x1e010101)->dst_mac);

  // A hop bound after resolution starts resolved; an unbound hop is not
  // republished.
  neighbors.Update(gw, NeighborState::kResolved, peer_mac);
  neighbors.Unbind(NextHopId(2));
  const NextHop late = neighbors.Bind(NextHopId(3), gw, if_mac);
  EXPECT_EQ(NeighborState::kResolved, late.neighbor);
  EXPECT_EQ(peer_mac, late.dst_mac);
  updates = neighbors.Update(gw, NeighborState::kResolved, other_mac);
  ASSERT_EQ(2u, updates.size());
  for (const auto &[id, hop] : updates) {
    EXPECT_NE(NextHopId(2), id);
  }
  // A neighbor on another interface with the same address is another neighbor.
  EXPECT_FALSE(neighbors.Find({dataplane::InterfaceId(4), 0x0a000001}).has_value());
}

// -- allocation failure in neighbor updates ------------------------------------------

using fault_injection::AllocationFaults;
using fault_injection::ForEachFailurePoint;

Ethernet::Address MacOf(uint8_t last) {
  Ethernet::Address mac{};
  mac.bytes[0] = 0x02;
  mac.bytes[5] = last;
  return mac;
}

const NeighborTable::Key kGwA{dataplane::InterfaceId(3), 0x0a000001};
const NeighborTable::Key kGwB{dataplane::InterfaceId(3), 0x0a000002};
const NeighborTable::Key kGwC{dataplane::InterfaceId(4), 0x0a000003};

// What a caller can observe of a NeighborTable: each neighbor's state and
// MAC, and the next hops (with their source MACs) an update of it would
// republish -- the latter probed on a copy, so the table itself is not
// touched.
std::string Observe(const NeighborTable &table) {
  std::string s = std::to_string(table.size());
  for (const NeighborTable::Key &key : {kGwA, kGwB, kGwC}) {
    const auto n = table.Find(key);
    s += " |" + (n ? std::to_string(static_cast<int>(n->state)) + "/" + n->mac.ToString()
                   : std::string("absent"));
    NeighborTable probe = table;
    for (const auto &[id, hop] : probe.Update(key, NeighborState::kResolved, MacOf(0xee))) {
      s += " h" + std::to_string(id.value()) + "@" + hop.src_mac.ToString();
    }
  }
  return s;
}

// The table each scenario starts from: A has hops 1 and 2 (learned), B has
// hop 3 (never learned), C is unknown.
NeighborTable StartingTable() {
  NeighborTable t;
  (void)t.Bind(NextHopId(1), kGwA, MacOf(0xa1));
  (void)t.Bind(NextHopId(2), kGwA, MacOf(0xa2));
  (void)t.Update(kGwA, NeighborState::kResolved, MacOf(0x01));
  (void)t.Bind(NextHopId(3), kGwB, MacOf(0xb3));
  return t;
}

// Bind allocates (a neighbor entry, the hop in its set, the binding). Each
// allocation refused in turn: Bind throws, and the table -- including a
// binding the hop had before -- is exactly as it was; the same Bind then
// succeeds and leaves the table as an undisturbed Bind would.
TEST(NeighborTableFaultTest, BindFailureAtEveryAllocationLeavesNoTrace) {
  struct Scenario {
    const char *name;
    NextHopId hop;
    NeighborTable::Key key;
  };
  const Scenario scenarios[] = {
      {"new hop, new neighbor", NextHopId(4), kGwC},
      {"new hop, learned neighbor", NextHopId(4), kGwA},
      {"rebind the only hop of an unlearned neighbor elsewhere", NextHopId(3), kGwC},
      {"rebind a hop of a learned neighbor elsewhere", NextHopId(1), kGwB},
      {"rebind a hop to its own neighbor", NextHopId(2), kGwA},
  };
  for (const Scenario &sc : scenarios) {
    SCOPED_TRACE(sc.name);
    NeighborTable reference = StartingTable();
    const NextHop expected = reference.Bind(sc.hop, sc.key, MacOf(0xcc));
    const size_t points = ForEachFailurePoint([&](size_t k) {
      SCOPED_TRACE(::testing::Message() << "failing allocation " << k);
      NeighborTable t = StartingTable();
      const std::string before = Observe(t);
      bool threw = false, injected = false;
      {
        const AllocationFaults faults(k);
        try {
          (void)t.Bind(sc.hop, sc.key, MacOf(0xcc));
        } catch (const std::bad_alloc &) {
          threw = true;
        }
        injected = faults.injected();
      }
      if (!injected) {
        ASSERT_FALSE(threw);
        ASSERT_EQ(Observe(reference), Observe(t));
        return;
      }
      ASSERT_TRUE(threw) << "a refused allocation was swallowed";
      ASSERT_EQ(before, Observe(t)) << "a failed Bind left a trace";
      const NextHop retried = t.Bind(sc.hop, sc.key, MacOf(0xcc));
      EXPECT_EQ(expected.neighbor, retried.neighbor);
      EXPECT_EQ(expected.dst_mac, retried.dst_mac);
      ASSERT_EQ(Observe(reference), Observe(t));
    });
    EXPECT_GE(points, 1u);
  }
}

// Update allocates (a new neighbor entry, the list of next hops to
// republish). Each allocation refused in turn: Update throws and changes
// nothing -- so the retry still reports every hop to republish, rather than
// finding the neighbor already "unchanged" and reporting none.
TEST(NeighborTableFaultTest, UpdateFailureAtEveryAllocationLeavesNoTrace) {
  struct Scenario {
    const char *name;
    NeighborTable::Key key;
    NeighborState state;
  };
  const Scenario scenarios[] = {
      {"a learned neighbor moves", kGwA, NeighborState::kResolved},
      {"an unlearned neighbor resolves", kGwB, NeighborState::kResolved},
      {"an unknown neighbor is learned", kGwC, NeighborState::kUnreachable},
  };
  for (const Scenario &sc : scenarios) {
    SCOPED_TRACE(sc.name);
    NeighborTable reference = StartingTable();
    const NeighborTable::Updates expected = reference.Update(sc.key, sc.state, MacOf(0x77));
    const size_t points = ForEachFailurePoint([&](size_t k) {
      SCOPED_TRACE(::testing::Message() << "failing allocation " << k);
      NeighborTable t = StartingTable();
      const std::string before = Observe(t);
      bool threw = false, injected = false;
      {
        const AllocationFaults faults(k);
        try {
          (void)t.Update(sc.key, sc.state, MacOf(0x77));
        } catch (const std::bad_alloc &) {
          threw = true;
        }
        injected = faults.injected();
      }
      if (!injected) {
        ASSERT_FALSE(threw);
        ASSERT_EQ(Observe(reference), Observe(t));
        return;
      }
      ASSERT_TRUE(threw) << "a refused allocation was swallowed";
      ASSERT_EQ(before, Observe(t)) << "a failed Update left a trace";
      const NeighborTable::Updates retried = t.Update(sc.key, sc.state, MacOf(0x77));
      ASSERT_EQ(expected.size(), retried.size()) << "the retry lost next hops to republish";
      for (size_t i = 0; i < expected.size(); i++) {
        EXPECT_EQ(expected[i].first, retried[i].first);
        EXPECT_EQ(expected[i].second.dst_mac, retried[i].second.dst_mac);
      }
      ASSERT_EQ(Observe(reference), Observe(t));
    });
    EXPECT_GE(points, 1u);
  }
}

}  // namespace
}  // namespace bess::route
