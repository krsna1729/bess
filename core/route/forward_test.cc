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

#include <array>
#include <cstdint>
#include <memory>
#include <span>

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

BESS_INSTANTIATE_ROUTE_BACKENDS(ForwardChainTest);

}  // namespace
}  // namespace bess::route
