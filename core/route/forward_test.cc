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

std::unique_ptr<Router> MakeRouter() {
  LpmRouteTable::Config config = bess::testing::TestRouteConfig();
  config.max_routes = 64;
  config.tbl8_groups = 8;
  auto router = Router::Create("forward_test", config, /*max_next_hops=*/8,
                               bess::runtime::runtime().rcu());
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
class ForwardChainTest : public bess::testing::RouteBackendTest {};

TEST_P(ForwardChainTest, EgressIsReadBeforeTheNeighborState) {
  auto router = MakeRouter();
  // A hop that names no egress interface and whose neighbor is not resolved:
  // the blackhole shape (D-160) as a controller writes it, and the shape of a
  // local /32, whose neighbor nothing ever resolves. Its frames must not be
  // reported as merely unresolved -- a graph punts those (D-162) -- and its
  // neighbor state must not decide anything at all.
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
  EXPECT_EQ(ForwardVerdict::kNotIpv4, batch[7].verdict);
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

BESS_INSTANTIATE_ROUTE_BACKENDS(ForwardChainTest);

}  // namespace
}  // namespace bess::route
