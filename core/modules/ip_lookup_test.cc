// SPDX-License-Identifier: BSD-3-Clause

// IPLookup in process (was bessctl/module_tests/iplookup.py): longest-prefix
// routes to gates, deleted and unknown routes, prefixes with host bits, and
// wire gates that do not fit gate_idx_t; the "<name>/routes" resource (D-129):
// applied, read back canonically, refused; its commands went with the legacy
// service (D-140).

#include <gtest/gtest.h>

#include <cerrno>
#include <random>
#include <string>
#include <vector>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "testing/route_backends.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::TcpPacket;

// Every test runs with backend LPM and FIB (D-132).
class IpLookupTest : public bess::testing::RouteBackendTest {
 protected:
  bess::pb::IPLookupArg Arg() const {
    bess::pb::IPLookupArg arg;
    arg.set_backend(GetParam() == bess::route::RouteBackend::kRteFib
                        ? bess::pb::ROUTE_BACKEND_FIB
                        : bess::pb::ROUTE_BACKEND_LPM);
    return arg;
  }
};

bess::pb::IPLookupRouteKey RouteKey(const std::string &prefix, uint64_t len) {
  bess::pb::IPLookupRouteKey key;
  key.set_prefix(prefix);
  key.set_prefix_len(len);
  return key;
}

bess::pb::IPLookupRouteValue RouteValue(uint64_t gate) {
  bess::pb::IPLookupRouteValue value;
  value.set_gate(gate);
  return value;
}

TEST_P(IpLookupTest, RoutesToTheGateOfTheMatchingPrefix) {
  std::mt19937 rng(1);
  const Bytes to22 = TcpPacket({.sip = "12.22.22.22", .dip = "22.22.22.22"}, rng);
  const Bytes to32 = TcpPacket({.sip = "12.22.22.22", .dip = "32.22.22.22"}, rng);
  const Bytes to42 = TcpPacket({.sip = "12.22.22.22", .dip = "42.22.22.22"}, rng);

  ModuleHarness h;
  Module *ipl = h.Create("IPLookup", Arg(), "ipl");
  ASSERT_NE(nullptr, ipl);
  EXPECT_EQ("", h.UpsertResource("ipl/routes", RouteKey("22.22.22.0", 24), RouteValue(0)));
  EXPECT_EQ("", h.UpsertResource("ipl/routes", RouteKey("32.22.22.0", 24), RouteValue(1)));
  EXPECT_EQ("", h.UpsertResource("ipl/routes", RouteKey("42.22.22.0", 24), RouteValue(1)));
  EXPECT_EQ("", h.EraseResource("ipl/routes", RouteKey("42.22.22.0", 24)));
  EXPECT_NE("", h.EraseResource("ipl/routes", RouteKey("52.22.22.0", 24)))
      << "deleting a route that is not there";

  auto out = h.Run(ipl, 0, {to22, to32, to42}, {0, 1});
  EXPECT_EQ((std::vector<Bytes>{to22}), out[0]);
  EXPECT_EQ((std::vector<Bytes>{to32}), out[1]) << "the deleted route forwards nothing";
}

// The gate is uint64 on the wire; 65536 + g used to route to gate g.
TEST_P(IpLookupTest, GateDoesNotWrap) {
  std::mt19937 rng(2);
  const Bytes pkt = TcpPacket({.sip = "12.0.0.1", .dip = "10.1.2.3"}, rng);
  ModuleHarness h;
  Module *ipl = h.Create("IPLookup", Arg(), "ipl");
  ASSERT_NE(nullptr, ipl);
  EXPECT_NE("", h.UpsertResource("ipl/routes", RouteKey("10.0.0.0", 8),
                                 RouteValue(65536 + 1)));
  EXPECT_NE("", h.UpsertResource("ipl/routes", RouteKey("10.0.0.0", 8),
                                 RouteValue(uint64_t{1} << 32)));
  EXPECT_TRUE(h.Run(ipl, 0, {pkt}, {0, 1})[1].empty()) << "a refused route is not installed";
  EXPECT_EQ("", h.UpsertResource("ipl/routes", RouteKey("10.0.0.0", 8), RouteValue(1)));
  EXPECT_EQ((std::vector<Bytes>{pkt}), h.Run(ipl, 0, {pkt}, {0, 1})[1]);
}

// Routes upserted through "<name>/routes" decide the gates (longest prefix
// first, the /0 route for the rest), and read back as the client sent them,
// so the same manifest applies as a no-op. A route the legacy add installed
// reads back the same way.
TEST_P(IpLookupTest, RoutesResourceRoundTrip) {
  std::mt19937 rng(3);
  const Bytes to8 = TcpPacket({.sip = "12.0.0.1", .dip = "10.2.0.1"}, rng);
  const Bytes to16 = TcpPacket({.sip = "12.0.0.1", .dip = "10.1.2.3"}, rng);
  const Bytes to24 = TcpPacket({.sip = "12.0.0.1", .dip = "192.168.1.5"}, rng);
  const Bytes elsewhere = TcpPacket({.sip = "12.0.0.1", .dip = "8.8.8.8"}, rng);

  ModuleHarness h;
  Module *ipl = h.Create("IPLookup", Arg(), "ipl");
  ASSERT_NE(nullptr, ipl);
  const auto k8 = RouteKey("10.0.0.0", 8);
  const auto k16 = RouteKey("10.1.0.0", 16);
  const auto k24 = RouteKey("192.168.1.0", 24);
  const auto k0 = RouteKey("0.0.0.0", 0);
  const auto g1 = RouteValue(1);
  const auto g2 = RouteValue(2);
  const auto g0 = RouteValue(0);
  const auto g3 = RouteValue(3);
  EXPECT_EQ("", h.UpsertResource("ipl/routes", k8, g1));
  EXPECT_EQ("", h.UpsertResource("ipl/routes", k16, g2));
  EXPECT_EQ("", h.UpsertResource("ipl/routes", k24, g0));
  EXPECT_EQ("", h.UpsertResource("ipl/routes", k0, g3));

  auto out = h.Run(ipl, 0, {to8, to16, to24, elsewhere}, {0, 1, 2, 3});
  EXPECT_EQ((std::vector<Bytes>{to24}), out[0]);
  EXPECT_EQ((std::vector<Bytes>{to8}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{to16}), out[2]);
  EXPECT_EQ((std::vector<Bytes>{elsewhere}), out[3]) << "the default route";

  EXPECT_EQ("", h.ReadBackDiffers("ipl/routes", {{&k8, &g1}, {&k16, &g2}, {&k24, &g0}, {&k0, &g3}}));
  for (const auto &[key, value] : h.ReadResource("ipl/routes")) {
    bess::pb::IPLookupRouteKey k;
    ASSERT_TRUE(key.UnpackTo(&k));
    EXPECT_TRUE(k.prefix() == "10.0.0.0" || k.prefix() == "10.1.0.0" ||
                k.prefix() == "192.168.1.0" || k.prefix() == "0.0.0.0")
        << k.ShortDebugString();
  }

  // Erasing the default route: the rest is dropped again.
  EXPECT_EQ("", h.EraseResource("ipl/routes", k0));
  EXPECT_TRUE(h.Run(ipl, 0, {elsewhere}, {3})[3].empty());
  EXPECT_EQ("", h.ReadBackDiffers("ipl/routes", {{&k8, &g1}, {&k16, &g2}, {&k24, &g0}}));
}

// The resource refuses what add refuses: host bits, a prefix length past 32,
// a missing or malformed prefix, and gates that do not fit; nothing changes.
TEST_P(IpLookupTest, RoutesResourceRefusesBadRoutes) {
  std::mt19937 rng(4);
  const Bytes pkt = TcpPacket({.sip = "12.0.0.1", .dip = "10.1.2.3"}, rng);
  ModuleHarness h;
  Module *ipl = h.Create("IPLookup", Arg(), "ipl");
  ASSERT_NE(nullptr, ipl);
  EXPECT_NE("", h.UpsertResource("ipl/routes", RouteKey("10.1.0.0", 8), RouteValue(1)));
  EXPECT_NE("", h.UpsertResource("ipl/routes", RouteKey("10.0.0.0", 33), RouteValue(1)));
  EXPECT_NE("", h.UpsertResource("ipl/routes", RouteKey("", 0), RouteValue(1)));
  EXPECT_NE("", h.UpsertResource("ipl/routes", RouteKey("10.0.0", 8), RouteValue(1)));
  EXPECT_NE("", h.UpsertResource("ipl/routes", RouteKey("10.0.0.0", 8), RouteValue(65536 + 1)));
  EXPECT_NE("", h.UpsertResource("ipl/routes", RouteKey("10.0.0.0", 8), RouteValue(8193)));
  EXPECT_NE("", h.EraseResource("ipl/routes", RouteKey("10.0.0.0", 8))) << "not there";
  EXPECT_TRUE(h.ReadResource("ipl/routes").empty());
  EXPECT_TRUE(h.Run(ipl, 0, {pkt}, {1})[1].empty());
}

BESS_INSTANTIATE_ROUTE_BACKENDS(IpLookupTest);

}  // namespace
