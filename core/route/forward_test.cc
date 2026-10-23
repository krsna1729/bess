// SPDX-License-Identifier: BSD-3-Clause

// D-157: the forwarding chain both callers share (route/forward.h) -- the same
// two calls the Router module's destination path and the appliance's direct
// path make, so that the two paths cannot disagree. What it decides for a hop
// is a contract, and one ordering inside it is what this file pins: a hop that
// names no egress interface is the blackhole shape (D-160) whatever its
// neighbor state says, so its frames come back kNoEgress -- the outcome the
// module counts apart from a miss -- and never kUnresolved, which a graph is
// free to punt (D-162).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "dpdk.h"
#include "route/forward.h"
#include "route/route_table.h"
#include "route/router.h"
#include "runtime/runtime_state.h"
#include "testing/route_backends.h"
#include "utils/checksum.h"
#include "utils/endian.h"
#include "utils/ether.h"
#include "utils/ip.h"

namespace bess::route {
namespace {

using utils::Ethernet;
using utils::Ipv4;

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a << 24 | b << 16 | c << 8 | d;
}

Ipv4Prefix P(uint32_t addr, uint8_t len) { return Ipv4Prefix::Make(addr, len).value(); }

// Untagged Ethernet + IPv4 + UDP, 14 + 20 + 26 = 60 bytes: the shape the
// parse's fast path reads, with the ports a group route's flow hash needs, a
// UDP length that covers the datagram (a length under 8 is malformed), and a
// valid IPv4 header checksum -- DecrementTtl updates it incrementally, so a
// frame with a zero checksum would leave the header corrupt if a test ever
// wrote it.
std::array<uint8_t, 60> Frame(uint32_t dst, uint8_t ttl = 64) {
  std::array<uint8_t, 60> frame{};
  auto *eth = reinterpret_cast<Ethernet *>(frame.data());
  eth->ether_type = be16_t(0x0800);
  auto *ip = reinterpret_cast<Ipv4 *>(frame.data() + sizeof(Ethernet));
  ip->version = 4;
  ip->header_length = 5;
  ip->length = be16_t(46);
  ip->ttl = ttl;
  ip->protocol = Ipv4::kUdp;
  ip->src = be32_t(Ip(10, 0, 0, 2));
  ip->dst = be32_t(dst);
  ip->checksum = utils::CalculateIpv4NoOptChecksum(*ip);
  auto *udp = frame.data() + sizeof(Ethernet) + 20;
  udp[0] = 0x04;  // src port 1234
  udp[1] = 0xd2;
  udp[2] = 0x16;  // dst port 5678
  udp[3] = 0x2e;
  udp[4] = 0x00;  // length 26
  udp[5] = 0x1a;
  return frame;
}

// Ethernet + IPv6 + UDP, 14 + 40 + 16 = 70 bytes, from `src` to `dst` with
// the hop limit given and the same ports as Frame().
std::array<uint8_t, 70> Frame6(const char *dst, uint8_t hop_limit = 64,
                               const char *src = "2001:db8:ffff::2",
                               uint16_t src_port = 1234) {
  std::array<uint8_t, 70> frame{};
  auto *eth = reinterpret_cast<Ethernet *>(frame.data());
  eth->ether_type = be16_t(0x86dd);
  uint8_t *ip = frame.data() + sizeof(Ethernet);
  ip[0] = 0x60;  // version 6
  ip[5] = 16;    // payload length
  ip[6] = Ipv4::kUdp;
  ip[7] = hop_limit;
  std::memcpy(ip + 8, utils::ParseIpv6Address(src).value().bytes, 16);
  std::memcpy(ip + 24, utils::ParseIpv6Address(dst).value().bytes, 16);
  uint8_t *udp = ip + 40;
  udp[0] = static_cast<uint8_t>(src_port >> 8);
  udp[1] = static_cast<uint8_t>(src_port);
  udp[2] = 0x16;  // dst port 5678
  udp[3] = 0x2e;
  udp[5] = 16;  // length
  return frame;
}

Ipv6Prefix P6(const char *addr, uint8_t len) {
  return Ipv6Prefix::Make(utils::ParseIpv6Address(addr).value(), len).value();
}

// IPv4 only unless asked: the IPv6 tests ask for the families they need.
std::unique_ptr<Router> MakeRouter(bool ipv4 = true, bool ipv6 = false,
                                   size_t max_groups = 0) {
  Router::Config config;
  if (ipv4) {
    config.ipv4 = bess::testing::TestRouteConfig();
    config.ipv4->max_routes = 64;
    config.ipv4->tbl8_groups = 8;
  }
  if (ipv6) {
    config.ipv6 = Ipv6RouteTable::Config{.max_routes = 64, .tbl8_groups = 64};
  }
  auto router = Router::Create("forward_test", config, /*max_next_hops=*/8,
                               bess::runtime::runtime().rcu(), 1, max_groups);
  EXPECT_TRUE(router.has_value()) << RouteErrorName(router.error());
  return std::move(router).value();
}

// A route that leads to `hop` in the default domain, for the destination the
// frame carries.
void RouteThrough(Router &router, NextHopId id, const NextHop &hop, uint32_t prefix,
                  uint8_t len) {
  ASSERT_TRUE(router.SetNextHop(id, hop));
  ASSERT_TRUE(router.SetRoute(P(prefix, len), id));
}

// The chain is backend-agnostic, but the Router it resolves in is not: the
// suite runs once per backend like the rest of the route tests (D-132).
class ForwardChainTest : public bess::testing::RouteBackendTest {
 protected:
  void TearDown() override { bess::runtime::runtime().rcu().Drain(); }
};

TEST_P(ForwardChainTest, EgressIsReadBeforeTheNeighborState) {
  auto router = MakeRouter();
  // A hop that names no egress interface and whose neighbor is not resolved:
  // the blackhole shape (D-160) as a controller writes it. Its frames must not
  // be reported as merely unresolved -- a graph punts those (D-162) -- and its
  // neighbor state must not decide anything at all. A local route is a
  // different kind (D-178) and is tested in connected_routes_test.cc.
  NextHop hop = BlackholeNextHop();
  hop.neighbor = NeighborState::kIncomplete;
  RouteThrough(*router, NextHopId(1), hop, Ip(192, 0, 2, 0), 24);

  const auto frame = Frame(Ip(192, 0, 2, 7));
  const FrameRoute route =
      ResolveFrame(*router, kDefaultRouteDomainId, frame, frame.size());
  EXPECT_EQ(ForwardVerdict::kNoEgress, route.verdict);
  EXPECT_EQ(dataplane::kInvalidInterfaceId, route.egress);
  EXPECT_EQ(nullptr, route.hop);
}

TEST_P(ForwardChainTest, UnresolvedNeighborWithAnEgressIsUnresolved) {
  auto router = MakeRouter();
  NextHop hop;
  hop.egress = dataplane::InterfaceId(3);
  hop.neighbor = NeighborState::kIncomplete;
  RouteThrough(*router, NextHopId(1), hop, Ip(198, 51, 100, 0), 24);

  const auto frame = Frame(Ip(198, 51, 100, 7));
  const FrameRoute route =
      ResolveFrame(*router, kDefaultRouteDomainId, frame, frame.size());
  EXPECT_EQ(ForwardVerdict::kUnresolved, route.verdict);
  EXPECT_EQ(dataplane::InterfaceId(3), route.egress) << "where the frame was headed";
  EXPECT_EQ(nullptr, route.hop);
}

TEST_P(ForwardChainTest, ResolvedHopForwards) {
  auto router = MakeRouter();
  NextHop hop;
  hop.egress = dataplane::InterfaceId(2);
  hop.neighbor = NeighborState::kResolved;
  hop.dst_mac.bytes[5] = 0x11;
  hop.src_mac.bytes[5] = 0x22;
  RouteThrough(*router, NextHopId(1), hop, Ip(203, 0, 113, 0), 24);

  const auto frame = Frame(Ip(203, 0, 113, 7));
  const FrameRoute route =
      ResolveFrame(*router, kDefaultRouteDomainId, frame, frame.size());
  EXPECT_EQ(ForwardVerdict::kForward, route.verdict);
  EXPECT_EQ(dataplane::InterfaceId(2), route.egress);
  ASSERT_NE(nullptr, route.hop);
  EXPECT_EQ(0x11, route.hop->dst_mac.bytes[5]);
  EXPECT_EQ(sizeof(Ethernet), route.l3_offset);
}

TEST_P(ForwardChainTest, NoRouteIsNotUnresolved) {
  auto router = MakeRouter();
  const auto frame = Frame(Ip(203, 0, 113, 7));
  const FrameRoute route =
      ResolveFrame(*router, kDefaultRouteDomainId, frame, frame.size());
  EXPECT_EQ(ForwardVerdict::kNoRoute, route.verdict);
}

// The read half also reads the TTL, so a frame at the boundary comes back
// kTtlExpired with where it was headed -- the caller can drop it without
// making it writable first (the property the module's path had by hand).
TEST_P(ForwardChainTest, TtlAtTheBoundaryIsItsOwnOutcome) {
  auto router = MakeRouter();
  NextHop hop;
  hop.egress = dataplane::InterfaceId(2);
  hop.neighbor = NeighborState::kResolved;
  RouteThrough(*router, NextHopId(1), hop, Ip(203, 0, 113, 0), 24);

  for (uint8_t ttl : {0, 1}) {
    const auto frame = Frame(Ip(203, 0, 113, 7), ttl);
    const FrameRoute route =
        ResolveFrame(*router, kDefaultRouteDomainId, frame, frame.size());
    EXPECT_EQ(ForwardVerdict::kTtlExpired, route.verdict) << "ttl " << int(ttl);
    EXPECT_EQ(dataplane::InterfaceId(2), route.egress);
    EXPECT_EQ(nullptr, route.hop) << "nothing to write";
  }
}

// The batch read half is the same chain, and this is the promise both callers
// rest on (D-157): for every verdict the chain can return, a run of frames
// resolved in one lookup must agree with the scalar path frame by frame. The
// module's destination path resolves a whole burst this way, so a frame it
// reports must be the frame the scalar path would have reported.
TEST_P(ForwardChainTest, TheBatchPathAgreesWithTheScalarPathFrameByFrame) {
  auto router = MakeRouter();
  NextHop forward_hop;
  forward_hop.egress = dataplane::InterfaceId(2);
  forward_hop.neighbor = NeighborState::kResolved;
  forward_hop.dst_mac.bytes[5] = 0x11;
  forward_hop.src_mac.bytes[5] = 0x22;
  RouteThrough(*router, NextHopId(1), forward_hop, Ip(203, 0, 113, 0), 24);

  NextHop unresolved_hop;
  unresolved_hop.egress = dataplane::InterfaceId(3);
  unresolved_hop.neighbor = NeighborState::kIncomplete;
  RouteThrough(*router, NextHopId(2), unresolved_hop, Ip(198, 51, 100, 0), 24);

  RouteThrough(*router, NextHopId(3), BlackholeNextHop(), Ip(192, 0, 2, 0), 24);

  // One frame per verdict: a forward, an unresolved neighbor, a blackhole hop,
  // a miss, the TTL boundary, a non-initial fragment, a malformed frame and
  // one that is not IP at all.
  std::array<std::array<uint8_t, 60>, 8> storage;
  storage[0] = Frame(Ip(203, 0, 113, 7));
  storage[1] = Frame(Ip(198, 51, 100, 7));
  storage[2] = Frame(Ip(192, 0, 2, 7));
  storage[3] = Frame(Ip(203, 0, 114, 7));
  storage[4] = Frame(Ip(203, 0, 113, 7), 1);
  storage[5] = Frame(Ip(203, 0, 113, 7));
  storage[5][21] = 0x20;  // the fragment field's low byte: an offset, not zero
  storage[6] = Frame(Ip(203, 0, 113, 7));
  storage[6][16] = 0x00;  // an IP length under 20: malformed
  storage[6][17] = 0x0a;
  storage[7] = Frame(Ip(203, 0, 113, 7));
  storage[7][12] = 0x08;  // an ARP ether type: not IP
  storage[7][13] = 0x06;

  std::array<std::span<const uint8_t>, 8> frames;
  std::array<size_t, 8> lens;
  std::array<FrameRoute, 8> batch;
  for (size_t i = 0; i < storage.size(); i++) {
    frames[i] = std::span(storage[i].data(), storage[i].size());
    lens[i] = storage[i].size();
  }
  ResolveFrames(*router, kDefaultRouteDomainId, frames, lens, batch);

  // The scalar path is the oracle: what it decides for a frame, the batch must
  // decide for the frame in the same position.
  for (size_t i = 0; i < storage.size(); i++) {
    const FrameRoute one =
        ResolveFrame(*router, kDefaultRouteDomainId, storage[i], storage[i].size());
    EXPECT_EQ(one.verdict, batch[i].verdict) << "frame " << i;
    EXPECT_EQ(one.egress, batch[i].egress) << "frame " << i;
    EXPECT_EQ(one.hop, batch[i].hop) << "frame " << i;
    EXPECT_EQ(one.l3_offset, batch[i].l3_offset) << "frame " << i;
  }
  // And the verdicts are the ones the frames were built for, so the oracle is
  // not agreeing with the batch on the wrong answer.
  EXPECT_EQ(ForwardVerdict::kForward, batch[0].verdict);
  EXPECT_EQ(ForwardVerdict::kUnresolved, batch[1].verdict);
  EXPECT_EQ(ForwardVerdict::kNoEgress, batch[2].verdict);
  EXPECT_EQ(ForwardVerdict::kNoRoute, batch[3].verdict);
  EXPECT_EQ(ForwardVerdict::kTtlExpired, batch[4].verdict);
  EXPECT_EQ(ForwardVerdict::kNonInitialFragment, batch[5].verdict);
  EXPECT_EQ(ForwardVerdict::kMalformed, batch[6].verdict);
  EXPECT_EQ(ForwardVerdict::kNotIp, batch[7].verdict);
}

// The two bodies' real risk is not the verdict list but the order of the checks
// and the fields each branch sets -- the l3_offset difference this pair already
// had. So this walks the combinations the checks read: a route or a miss, the
// neighbor state, whether the hop names an egress, and the TTL against 0, 1, 2
// and 64. Each combination is compared to the scalar path field by field.
TEST_P(ForwardChainTest, TheBatchPathAgreesWithTheScalarPathOnEveryCombination) {
  auto router = MakeRouter();
  struct Case {
    bool route;
    bool resolved;
    bool egress;
    uint8_t ttl;
  };
  std::vector<Case> cases;
  for (bool route : {false, true}) {
    for (bool resolved : {false, true}) {
      for (bool egress : {false, true}) {
        for (uint8_t ttl : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{64}}) {
          cases.push_back({route, resolved, egress, ttl});
        }
      }
    }
  }
  // Four hops cover the neighbor and egress combinations (the router holds 8);
  // one /24 per case routes to the hop its case names, when it has a route.
  struct HopCase {
    bool resolved;
    bool egress;
  };
  for (const HopCase &hc : {HopCase{true, true}, HopCase{true, false}, HopCase{false, true},
                            HopCase{false, false}}) {
    NextHop hop;
    if (hc.egress) {
      hop.egress = dataplane::InterfaceId(2);
    }
    hop.neighbor = hc.resolved ? NeighborState::kResolved : NeighborState::kIncomplete;
    const size_t h = (hc.resolved ? 0 : 2) + (hc.egress ? 0 : 1);
    ASSERT_TRUE(router->SetNextHop(NextHopId(static_cast<uint32_t>(h + 1)), hop));
  }
  std::vector<std::array<uint8_t, 60>> storage(cases.size());
  for (size_t i = 0; i < cases.size(); i++) {
    if (cases[i].route) {
      const size_t h = (cases[i].resolved ? 0 : 2) + (cases[i].egress ? 0 : 1);
      ASSERT_TRUE(router->SetRoute(P(Ip(10, 0, static_cast<uint32_t>(i), 0), 24),
                                   NextHopId(static_cast<uint32_t>(h + 1))));
    }
    storage[i] = Frame(Ip(10, 0, static_cast<uint32_t>(i), 7), cases[i].ttl);
  }
  for (size_t begin = 0; begin < cases.size(); begin += Router::kMaxBatch) {
    const size_t n = std::min(cases.size() - begin, Router::kMaxBatch);
    std::array<std::span<const uint8_t>, Router::kMaxBatch> frames;
    std::array<size_t, Router::kMaxBatch> lens;
    std::array<FrameRoute, Router::kMaxBatch> batch;
    for (size_t k = 0; k < n; k++) {
      frames[k] = std::span(storage[begin + k].data(), storage[begin + k].size());
      lens[k] = storage[begin + k].size();
    }
    ResolveFrames(*router, kDefaultRouteDomainId, std::span(frames.data(), n),
                  std::span(lens.data(), n), std::span(batch.data(), n));
    for (size_t k = 0; k < n; k++) {
      const size_t i = begin + k;
      const FrameRoute one =
          ResolveFrame(*router, kDefaultRouteDomainId, storage[i], storage[i].size());
      EXPECT_EQ(one.verdict, batch[k].verdict) << "combination " << i;
      EXPECT_EQ(one.egress, batch[k].egress) << "combination " << i;
      EXPECT_EQ(one.hop, batch[k].hop) << "combination " << i;
      EXPECT_EQ(one.l3_offset, batch[k].l3_offset) << "combination " << i;
    }
  }
}

// A resolved hop with both addresses, a hop whose neighbor is unresolved, a
// blackhole, the node's own address, and IPv6 routes to each.
void RouteIpv6Kinds(Router &router) {
  NextHop forward_hop;
  forward_hop.egress = dataplane::InterfaceId(2);
  forward_hop.neighbor = NeighborState::kResolved;
  forward_hop.dst_mac.bytes[5] = 0x11;
  forward_hop.src_mac.bytes[5] = 0x22;
  NextHop unresolved_hop;
  unresolved_hop.egress = dataplane::InterfaceId(3);
  unresolved_hop.neighbor = NeighborState::kIncomplete;
  ASSERT_TRUE(router.SetNextHop(NextHopId(1), forward_hop));
  ASSERT_TRUE(router.SetNextHop(NextHopId(2), unresolved_hop));
  ASSERT_TRUE(router.SetNextHop(NextHopId(3), BlackholeNextHop()));
  ASSERT_TRUE(router.SetNextHop(NextHopId(4), LocalNextHop()));
  ASSERT_TRUE(router.SetRoute(kDefaultRouteDomainId, P6("2001:db8:1::", 48), NextHopId(1)));
  ASSERT_TRUE(router.SetRoute(kDefaultRouteDomainId, P6("2001:db8:2::", 48), NextHopId(2)));
  ASSERT_TRUE(router.SetRoute(kDefaultRouteDomainId, P6("2001:db8:3::", 48), NextHopId(3)));
  ASSERT_TRUE(router.SetRoute(kDefaultRouteDomainId, P6("2001:db8:4::1", 128), NextHopId(4)));
}

// IPv6 runs the same chain as IPv4, with the hop limit where the TTL is.
TEST_P(ForwardChainTest, Ipv6VerdictsFollowTheSameChain) {
  auto router = MakeRouter(/*ipv4=*/false, /*ipv6=*/true);
  RouteIpv6Kinds(*router);
  auto resolve = [&](const std::array<uint8_t, 70> &frame) {
    return ResolveFrame(*router, kDefaultRouteDomainId, frame, frame.size());
  };

  FrameRoute route = resolve(Frame6("2001:db8:1::7"));
  EXPECT_EQ(ForwardVerdict::kForward, route.verdict);
  EXPECT_EQ(L3Family::kIpv6, route.family);
  EXPECT_EQ(dataplane::InterfaceId(2), route.egress);
  ASSERT_NE(nullptr, route.hop);
  EXPECT_EQ(0x11, route.hop->dst_mac.bytes[5]);
  EXPECT_EQ(sizeof(Ethernet), route.l3_offset);

  route = resolve(Frame6("2001:db8:2::7"));
  EXPECT_EQ(ForwardVerdict::kUnresolved, route.verdict);
  EXPECT_EQ(dataplane::InterfaceId(3), route.egress);
  EXPECT_EQ(nullptr, route.hop);

  route = resolve(Frame6("2001:db8:3::7"));
  EXPECT_EQ(ForwardVerdict::kNoEgress, route.verdict);
  EXPECT_EQ(dataplane::kInvalidInterfaceId, route.egress);

  EXPECT_EQ(ForwardVerdict::kLocalDelivery, resolve(Frame6("2001:db8:4::1")).verdict);
  EXPECT_EQ(ForwardVerdict::kNoRoute, resolve(Frame6("2001:db8:4::2")).verdict)
      << "a /128 is the address only";
  EXPECT_EQ(ForwardVerdict::kNoRoute, resolve(Frame6("2001:db8:9::7")).verdict);

  for (uint8_t hop_limit : {0, 1}) {
    route = resolve(Frame6("2001:db8:1::7", hop_limit));
    EXPECT_EQ(ForwardVerdict::kTtlExpired, route.verdict) << int(hop_limit);
    EXPECT_EQ(dataplane::InterfaceId(2), route.egress);
    EXPECT_EQ(nullptr, route.hop) << "nothing to write";
  }
  EXPECT_EQ(ForwardVerdict::kForward, resolve(Frame6("2001:db8:1::7", 2)).verdict);
}

// Link-scoped traffic is never routed (RFC 4291 2.5.6), whatever the table
// says -- even a default route that would carry it -- and the check runs
// before the lookup.
TEST_P(ForwardChainTest, Ipv6LinkScopedFramesAreNeverRouted) {
  auto router = MakeRouter(/*ipv4=*/false, /*ipv6=*/true);
  RouteIpv6Kinds(*router);
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P6("::", 0), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P6("fe80::", 10), NextHopId(1)));
  for (const auto &frame : {Frame6("fe80::1"), Frame6("febf::1"), Frame6("ff02::1"),
                            Frame6("ff0e::1"), Frame6("2001:db8:1::7", 64, "fe80::2")}) {
    const FrameRoute route =
        ResolveFrame(*router, kDefaultRouteDomainId, frame, frame.size());
    EXPECT_EQ(ForwardVerdict::kLinkScope, route.verdict);
    EXPECT_EQ(L3Family::kIpv6, route.family);
    EXPECT_EQ(nullptr, route.hop);
  }
  // Just outside fe80::/10, and a global source: routed.
  for (const auto &frame : {Frame6("fec0::1"), Frame6("2001:db8:1::7", 64, "fec0::2")}) {
    EXPECT_EQ(ForwardVerdict::kForward,
              ResolveFrame(*router, kDefaultRouteDomainId, frame, frame.size()).verdict);
  }
}

// A frame of a family the router does not serve is refused before anything
// else is read, on either path.
TEST_P(ForwardChainTest, AFamilyTheRouterDoesNotServeIsRefused) {
  const auto frame4 = Frame(Ip(203, 0, 113, 7));
  const auto frame6 = Frame6("2001:db8:1::7");
  for (const bool ipv4 : {true, false}) {
    SCOPED_TRACE(ipv4 ? "IPv4 only" : "IPv6 only");
    auto router = MakeRouter(ipv4, !ipv4);
    const FrameRoute route4 =
        ResolveFrame(*router, kDefaultRouteDomainId, frame4, frame4.size());
    const FrameRoute route6 =
        ResolveFrame(*router, kDefaultRouteDomainId, frame6, frame6.size());
    EXPECT_EQ(ipv4 ? ForwardVerdict::kNoRoute : ForwardVerdict::kFamilyDisabled,
              route4.verdict);
    EXPECT_EQ(L3Family::kIpv4, route4.family);
    EXPECT_EQ(ipv4 ? ForwardVerdict::kFamilyDisabled : ForwardVerdict::kNoRoute,
              route6.verdict);
    EXPECT_EQ(L3Family::kIpv6, route6.family);

    const std::span<const uint8_t> frames[] = {frame4, frame6};
    const size_t lens[] = {frame4.size(), frame6.size()};
    FrameRoute batch[2];
    ResolveFrames(*router, kDefaultRouteDomainId, frames, lens, batch);
    EXPECT_EQ(route4.verdict, batch[0].verdict);
    EXPECT_EQ(route6.verdict, batch[1].verdict);
    router.reset();
    bess::runtime::runtime().rcu().Drain();
  }
}

// The write half decrements the hop limit once -- IPv6 has no header checksum
// -- writes both addresses and touches nothing else; at the boundary it
// writes nothing. IPv4 beside it decrements the TTL with a valid checksum.
TEST_P(ForwardChainTest, WriteForwardedDecrementsTheHopLimitOnceAndWritesTheAddresses) {
  auto router = MakeRouter(/*ipv4=*/true, /*ipv6=*/true);
  RouteIpv6Kinds(*router);
  ASSERT_TRUE(router->SetRoute(P(Ip(203, 0, 113, 0), 24), NextHopId(1)));

  auto frame6 = Frame6("2001:db8:1::7");
  const auto before6 = frame6;
  const FrameRoute route6 =
      ResolveFrame(*router, kDefaultRouteDomainId, frame6, frame6.size());
  ASSERT_EQ(ForwardVerdict::kForward, route6.verdict);
  EXPECT_EQ(TtlResult::kForward,
            WriteForwarded(frame6, route6.family, route6.l3_offset, *route6.hop));
  const auto *eth = reinterpret_cast<const Ethernet *>(frame6.data());
  EXPECT_EQ(route6.hop->dst_mac, eth->dst_addr);
  EXPECT_EQ(route6.hop->src_mac, eth->src_addr);
  EXPECT_EQ(63, frame6[sizeof(Ethernet) + 7]);
  for (size_t i = 12; i < frame6.size(); i++) {
    if (i != sizeof(Ethernet) + 7) {
      EXPECT_EQ(before6[i], frame6[i]) << "byte " << i;
    }
  }
  auto edge6 = Frame6("2001:db8:1::7", 1);
  const auto edge6_before = edge6;
  EXPECT_EQ(TtlResult::kExpired,
            WriteForwarded(edge6, L3Family::kIpv6, sizeof(Ethernet), *route6.hop));
  EXPECT_EQ(edge6_before, edge6);

  auto frame4 = Frame(Ip(203, 0, 113, 7));
  const FrameRoute route4 =
      ResolveFrame(*router, kDefaultRouteDomainId, frame4, frame4.size());
  ASSERT_EQ(ForwardVerdict::kForward, route4.verdict);
  EXPECT_EQ(L3Family::kIpv4, route4.family);
  EXPECT_EQ(TtlResult::kForward,
            WriteForwarded(frame4, route4.family, route4.l3_offset, *route4.hop));
  const auto *ip = reinterpret_cast<const Ipv4 *>(frame4.data() + sizeof(Ethernet));
  EXPECT_EQ(63, ip->ttl);
  EXPECT_TRUE(utils::VerifyIpv4NoOptChecksum(*ip));
  EXPECT_EQ(route4.hop->dst_mac, reinterpret_cast<const Ethernet *>(frame4.data())->dst_addr);
}

// One batch of interleaved IPv4 and IPv6 frames -- one IPv4 bulk lookup and
// one IPv6 bulk lookup -- agrees with the scalar path frame by frame, for
// every verdict and for a group route's member choice, which rests on the
// IPv6 flow hash (the xor of each address's four words).
TEST_P(ForwardChainTest, TheBatchPathAgreesWithTheScalarPathForMixedFamilies) {
  auto router = MakeRouter(/*ipv4=*/true, /*ipv6=*/true, /*max_groups=*/2);
  RouteIpv6Kinds(*router);
  NextHop other_hop;
  other_hop.egress = dataplane::InterfaceId(5);
  other_hop.neighbor = NeighborState::kResolved;
  ASSERT_TRUE(router->SetNextHop(NextHopId(5), other_hop));
  const NextHopId members[] = {NextHopId(1), NextHopId(5)};
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(1), members));
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P6("2001:db8:5::", 48),
                               NextHopGroupId(1)));
  ASSERT_TRUE(router->SetRoute(P(Ip(203, 0, 113, 0), 24), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(P(Ip(198, 51, 100, 0), 24), NextHopId(2)));

  std::vector<std::vector<uint8_t>> storage;
  auto add = [&](const auto &frame) { storage.emplace_back(frame.begin(), frame.end()); };
  add(Frame(Ip(203, 0, 113, 7)));          // IPv4 forward
  add(Frame6("2001:db8:1::7"));            // IPv6 forward
  add(Frame6("fe80::7"));                  // link scope
  add(Frame(Ip(203, 0, 114, 7)));          // IPv4 miss
  add(Frame6("2001:db8:9::7"));            // IPv6 miss
  add(Frame6("2001:db8:1::7", 1));         // hop limit
  add(Frame6("2001:db8:2::7"));            // unresolved
  add(Frame(Ip(198, 51, 100, 7)));         // IPv4 unresolved
  add(Frame6("2001:db8:3::7"));            // blackhole
  add(Frame6("2001:db8:4::1"));            // local
  add(Frame(Ip(203, 0, 113, 7), 1));       // TTL
  auto truncated = Frame6("2001:db8:1::7");
  truncated[sizeof(Ethernet) + 5] = 200;   // a payload length past the frame
  add(truncated);                          // malformed
  constexpr size_t kGroupFrames = 16;
  for (uint16_t port = 0; port < kGroupFrames; port++) {
    add(Frame6("2001:db8:5::7", 64, "2001:db8:ffff::2", static_cast<uint16_t>(1000 + port)));
  }
  const size_t n = storage.size();
  ASSERT_LE(n, Router::kMaxBatch);

  std::vector<std::span<const uint8_t>> frames(n);
  std::vector<size_t> lens(n);
  std::vector<FrameRoute> batch(n);
  for (size_t i = 0; i < n; i++) {
    frames[i] = storage[i];
    lens[i] = storage[i].size();
  }
  ResolveFrames(*router, kDefaultRouteDomainId, frames, lens, batch);
  for (size_t i = 0; i < n; i++) {
    const FrameRoute one =
        ResolveFrame(*router, kDefaultRouteDomainId, storage[i], storage[i].size());
    EXPECT_EQ(one.verdict, batch[i].verdict) << "frame " << i;
    EXPECT_EQ(one.family, batch[i].family) << "frame " << i;
    EXPECT_EQ(one.egress, batch[i].egress) << "frame " << i;
    EXPECT_EQ(one.hop, batch[i].hop) << "frame " << i;
    EXPECT_EQ(one.l3_offset, batch[i].l3_offset) << "frame " << i;
  }
  const ForwardVerdict expected[] = {
      ForwardVerdict::kForward,      ForwardVerdict::kForward,   ForwardVerdict::kLinkScope,
      ForwardVerdict::kNoRoute,      ForwardVerdict::kNoRoute,   ForwardVerdict::kTtlExpired,
      ForwardVerdict::kUnresolved,   ForwardVerdict::kUnresolved, ForwardVerdict::kNoEgress,
      ForwardVerdict::kLocalDelivery, ForwardVerdict::kTtlExpired, ForwardVerdict::kMalformed};
  for (size_t i = 0; i < std::size(expected); i++) {
    EXPECT_EQ(expected[i], batch[i].verdict) << "frame " << i;
  }
  EXPECT_EQ(L3Family::kIpv4, batch[0].family);
  EXPECT_EQ(L3Family::kIpv6, batch[1].family);

  // The group frames: each picks the member the documented hash picks, and
  // the ports spread them over both members.
  const auto src = utils::ParseIpv6Address("2001:db8:ffff::2").value();
  const auto dst = utils::ParseIpv6Address("2001:db8:5::7").value();
  auto fold = [](const utils::Ipv6Address &a) {
    uint32_t words[4];
    std::memcpy(words, a.bytes, sizeof(words));
    return words[0] ^ words[1] ^ words[2] ^ words[3];
  };
  bool seen[2] = {false, false};
  for (size_t k = 0; k < kGroupFrames; k++) {
    const FrameRoute &route = batch[n - kGroupFrames + k];
    ASSERT_EQ(ForwardVerdict::kForward, route.verdict) << k;
    const uint32_t hash =
        FlowHash(fold(src), fold(dst), static_cast<uint16_t>(1000 + k), 5678, Ipv4::kUdp);
    EXPECT_EQ(router->Resolve(kDefaultRouteDomainId, dst, hash), route.hop) << k;
    seen[route.egress == dataplane::InterfaceId(2) ? 0 : 1] = true;
  }
  EXPECT_TRUE(seen[0] && seen[1]) << "both members chosen";
}

BESS_INSTANTIATE_ROUTE_BACKENDS(ForwardChainTest);

}  // namespace
}  // namespace bess::route
