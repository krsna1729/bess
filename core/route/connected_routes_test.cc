// SPDX-License-Identifier: BSD-3-Clause

// D-178: the routes an interface address implies. The derivation is a pure
// function of the configured addresses (deterministic order, stable keys,
// duplicates collapsed, exact conflicts refused), and installation is one
// transaction over the Router's own resources: a refused transaction leaves
// nothing behind and the same derivation can be retried.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "route/connected_routes.h"
#include "route/forward.h"
#include "route/route_table.h"
#include "route/router.h"
#include "runtime/runtime_state.h"
#include "testing/route_backends.h"

namespace bess::route {
namespace {

using dataplane::Op;
using dataplane::TransactionEngine;
using Outcome = TransactionEngine::Outcome;

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a << 24 | b << 16 | c << 8 | d;
}

Ipv4Prefix P(uint32_t addr, uint8_t len) { return Ipv4Prefix::Make(addr, len).value(); }

RouteKey Key(uint32_t addr, uint8_t len, RouteDomainId domain = kDefaultRouteDomainId) {
  return RouteKey{.domain = domain, .prefix = P(addr, len)};
}

InterfaceAddress Iface(uint16_t id, uint32_t address, uint8_t length,
                       RouteDomainId domain = kDefaultRouteDomainId,
                       DerivedRouteFlags flags = DerivedRouteFlags::kAll) {
  return InterfaceAddress{.interface = dataplane::InterfaceId(id),
                          .domain = domain,
                          .address = address,
                          .length = length,
                          .flags = flags};
}

std::vector<InterfaceAddress> One(const InterfaceAddress &address) { return {address}; }

std::vector<NextHopId> Ids(size_t n, uint32_t first = 1) {
  std::vector<NextHopId> ids;
  ids.reserve(n);
  for (size_t i = 0; i < n; i++) {
    ids.push_back(NextHopId(first + static_cast<uint32_t>(i)));
  }
  return ids;
}

class ConnectedRoutesTest : public bess::testing::RouteBackendTest {
 protected:
  std::unique_ptr<Router> MakeRouter(uint32_t max_routes = 64, size_t next_hops = 64,
                                     size_t domains = 1) {
    LpmRouteTable::Config config = bess::testing::TestRouteConfig();
    config.max_routes = max_routes;
    config.tbl8_groups = 32;
    auto router = Router::Create("derived", config, next_hops,
                                 bess::runtime::runtime().rcu(), domains);
    EXPECT_TRUE(router.has_value()) << RouteErrorName(router.error());
    return std::move(router).value();
  }

  TransactionEngine::Result Apply(std::vector<Op> ops) { return engine_.Apply(ops); }

  void Settle() {
    while (engine_.ReclaimRetired() != 0) {
    }
    bess::runtime::runtime().rcu().Drain();
  }

  TransactionEngine engine_{bess::runtime::runtime().rcu()};
};

TEST_P(ConnectedRoutesTest, AnAddressDerivesTheSubnetAndItsOwnHostRoute) {
  const auto derived = DerivedRoutes::Derive(One(Iface(1, Ip(192, 0, 2, 77), 24)));
  ASSERT_TRUE(derived.has_value()) << DerivationErrorName(derived.error());
  ASSERT_EQ(2u, derived->size());

  // Deterministic order: the connected prefix sorts before the /32.
  const DerivedRoute &connected = derived->entries()[0];
  EXPECT_EQ(DerivedRouteKind::kConnected, connected.kind);
  EXPECT_EQ(P(Ip(192, 0, 2, 0), 24), connected.prefix);
  EXPECT_EQ(dataplane::InterfaceId(uint16_t{1}), connected.egress);
  EXPECT_EQ(NeighborState::kIncomplete, connected.hop().neighbor)
      << "the hop names the interface, not a neighbor";
  EXPECT_EQ(NextHopKind::kForward, connected.hop().kind);

  const DerivedRoute &local = derived->entries()[1];
  EXPECT_EQ(DerivedRouteKind::kLocal, local.kind);
  EXPECT_EQ(P(Ip(192, 0, 2, 77), 32), local.prefix);
  EXPECT_EQ(dataplane::kInvalidInterfaceId, local.egress);
  EXPECT_EQ(NextHopKind::kLocal, local.hop().kind);
  EXPECT_EQ(dataplane::kInvalidInterfaceId, local.hop().egress);
}

TEST_P(ConnectedRoutesTest, ThePrefixLengthEdges) {
  // /31: both addresses are usable, and the node's own one is still local.
  const auto pair = DerivedRoutes::Derive(One(Iface(1, Ip(10, 0, 0, 1), 31)));
  ASSERT_TRUE(pair.has_value());
  ASSERT_EQ(2u, pair->size());
  EXPECT_EQ(P(Ip(10, 0, 0, 0), 31), pair->entries()[0].prefix);

  // /32: the connected route would be the local route's own key. The address
  // itself is the only derived route.
  const auto host = DerivedRoutes::Derive(One(Iface(1, Ip(10, 0, 0, 7), 32)));
  ASSERT_TRUE(host.has_value());
  ASSERT_EQ(1u, host->size());
  EXPECT_EQ(DerivedRouteKind::kLocal, host->entries()[0].kind);
  EXPECT_EQ(P(Ip(10, 0, 0, 7), 32), host->entries()[0].prefix);

  // /0: the connected route is the default; the address is still local.
  const auto any = DerivedRoutes::Derive(One(Iface(1, Ip(10, 0, 0, 7), 0)));
  ASSERT_TRUE(any.has_value());
  ASSERT_EQ(2u, any->size());
  EXPECT_EQ(P(0, 0), any->entries()[0].prefix);

  const auto bad_length = DerivedRoutes::Derive(One(Iface(1, Ip(10, 0, 0, 1), 33)));
  ASSERT_FALSE(bad_length.has_value());
  EXPECT_EQ(DerivationError::kInvalidPrefixLength, bad_length.error());
  const auto bad_interface = DerivedRoutes::Derive(One(Iface(0, Ip(10, 0, 0, 1), 24)));
  ASSERT_FALSE(bad_interface.has_value());
  EXPECT_EQ(DerivationError::kInvalidInterface, bad_interface.error());
}

TEST_P(ConnectedRoutesTest, FlagsSelectWhatIsDerived) {
  const auto connected_only = DerivedRoutes::Derive(
      One(Iface(1, Ip(192, 0, 2, 77), 24, kDefaultRouteDomainId, DerivedRouteFlags::kConnected)));
  ASSERT_TRUE(connected_only.has_value());
  ASSERT_EQ(1u, connected_only->size());
  EXPECT_EQ(DerivedRouteKind::kConnected, connected_only->entries()[0].kind);

  const auto local_only = DerivedRoutes::Derive(
      One(Iface(1, Ip(192, 0, 2, 77), 24, kDefaultRouteDomainId, DerivedRouteFlags::kLocal)));
  ASSERT_TRUE(local_only.has_value());
  ASSERT_EQ(1u, local_only->size());
  EXPECT_EQ(DerivedRouteKind::kLocal, local_only->entries()[0].kind);

  const auto none = DerivedRoutes::Derive(
      One(Iface(1, Ip(192, 0, 2, 77), 24, kDefaultRouteDomainId, DerivedRouteFlags::kNone)));
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->empty());
}

TEST_P(ConnectedRoutesTest, MultipleAddressesOverlappingPrefixesAndDuplicates) {
  const std::vector<InterfaceAddress> addresses = {
      Iface(1, Ip(10, 0, 0, 1), 24),      // connected 10.0.0.0/24
      Iface(1, Ip(10, 0, 0, 1), 24),      // the same address again: a duplicate
      Iface(1, Ip(10, 0, 1, 1), 16),      // overlapping (10.0.0.0/16)
      Iface(1, Ip(10, 0, 1, 1), 32),      // its host route is already derived
      Iface(2, Ip(198, 51, 100, 7), 25),  // another interface, another subnet
  };
  const auto derived = DerivedRoutes::Derive(addresses);
  ASSERT_TRUE(derived.has_value()) << DerivationErrorName(derived.error());
  // 10.0.0.0/24, 10.0.0.0/16, 198.51.100.0/25, and local /32 x3 (10.0.0.1
  // twice collapses to one).
  ASSERT_EQ(6u, derived->size());

  ASSERT_NE(nullptr, derived->Find(Key(Ip(10, 0, 0, 0), 24)));
  EXPECT_EQ(dataplane::InterfaceId(uint16_t{1}),
            derived->Find(Key(Ip(10, 0, 0, 0), 24))->egress);
  EXPECT_NE(nullptr, derived->Find(Key(Ip(10, 0, 0, 0), 16)));
  EXPECT_NE(nullptr, derived->Find(Key(Ip(198, 51, 100, 0), 25)));
  ASSERT_NE(nullptr, derived->Find(Key(Ip(10, 0, 0, 1), 32)));
  EXPECT_EQ(DerivedRouteKind::kLocal, derived->Find(Key(Ip(10, 0, 0, 1), 32))->kind);
  ASSERT_NE(nullptr, derived->Find(Key(Ip(10, 0, 1, 1), 32)));
  EXPECT_EQ(DerivedRouteKind::kLocal, derived->Find(Key(Ip(10, 0, 1, 1), 32))->kind);
  ASSERT_NE(nullptr, derived->Find(Key(Ip(198, 51, 100, 7), 32)));
  EXPECT_EQ(DerivedRouteKind::kLocal, derived->Find(Key(Ip(198, 51, 100, 7), 32))->kind);
  EXPECT_EQ(nullptr, derived->Find(Key(Ip(10, 0, 0, 0), 30)))
      << "a prefix nothing derives";
}

TEST_P(ConnectedRoutesTest, TheOrderDoesNotDependOnTheInputOrder) {
  const std::vector<InterfaceAddress> forward = {
      Iface(2, Ip(198, 51, 100, 7), 24),
      Iface(1, Ip(192, 0, 2, 77), 24),
      Iface(3, Ip(10, 0, 0, 1), 8),
  };
  std::vector<InterfaceAddress> reversed(forward.rbegin(), forward.rend());
  const auto a = DerivedRoutes::Derive(forward);
  const auto b = DerivedRoutes::Derive(reversed);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_TRUE(std::equal(a->entries().begin(), a->entries().end(),
                         b->entries().begin(), b->entries().end()));

  // And the order is the documented one: domain, prefix address, length, kind.
  const std::span<const DerivedRoute> e = a->entries();
  for (size_t i = 1; i < e.size(); i++) {
    const auto order = [](const DerivedRoute &x, const DerivedRoute &y) {
      if (x.domain != y.domain) return x.domain < y.domain;
      if (x.prefix != y.prefix) return x.prefix < y.prefix;
      return x.kind < y.kind;
    };
    EXPECT_TRUE(order(e[i - 1], e[i])) << "entry " << i << " is out of order";
  }
}

TEST_P(ConnectedRoutesTest, TwoInterfacesDerivingOneKeyIsAConflict) {
  // The same subnet on two interfaces: picking one would silently blackhole
  // the other's hosts.
  const auto subnet = DerivedRoutes::Derive(std::vector<InterfaceAddress>{
      Iface(1, Ip(10, 0, 0, 1), 24), Iface(2, Ip(10, 0, 0, 2), 24)});
  ASSERT_FALSE(subnet.has_value());
  EXPECT_EQ(DerivationError::kConflictingRoute, subnet.error());

  // The same address on two interfaces is the same conflict for the local /32.
  const auto address = DerivedRoutes::Derive(std::vector<InterfaceAddress>{
      Iface(1, Ip(10, 0, 0, 1), 32), Iface(2, Ip(10, 0, 0, 1), 32)});
  ASSERT_FALSE(address.has_value());
  EXPECT_EQ(DerivationError::kConflictingRoute, address.error());

  // The same prefix in two domains is not a conflict: they are two routes.
  const auto domains = DerivedRoutes::Derive(std::vector<InterfaceAddress>{
      Iface(1, Ip(10, 0, 0, 1), 24, RouteDomainId(1)),
      Iface(2, Ip(10, 0, 0, 1), 24, RouteDomainId(2))});
  ASSERT_TRUE(domains.has_value());
  EXPECT_EQ(4u, domains->size());
}

TEST_P(ConnectedRoutesTest, InstallationIsOneTransactionAndResolvesAsDerived) {
  const auto derived = DerivedRoutes::Derive(One(Iface(1, Ip(192, 0, 2, 77), 24)));
  ASSERT_TRUE(derived.has_value());
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_.registry()));

  const std::vector<NextHopId> ids = Ids(derived->size());
  const auto ops = InstallationOps(*router, derived->entries(), ids);
  ASSERT_TRUE(ops.has_value()) << DerivationErrorName(ops.error());
  EXPECT_EQ(derived->size() * 2, ops->size());
  EXPECT_EQ(0u, router->route_count()) << "building operations writes no table";
  EXPECT_EQ(0u, router->next_hop_count());

  const auto applied = Apply(*ops);
  ASSERT_EQ(Outcome::kApplied, applied.outcome);
  EXPECT_EQ(derived->size(), router->route_count());

  // The address itself is local delivery; another host on its subnet is the
  // connected route (its neighbor unresolved); anything else is a miss.
  const NextHop *local = router->Resolve(Ip(192, 0, 2, 77));
  ASSERT_NE(nullptr, local);
  EXPECT_EQ(NextHopKind::kLocal, local->kind);
  EXPECT_EQ(dataplane::kInvalidInterfaceId, local->egress);

  const NextHop *connected = router->Resolve(Ip(192, 0, 2, 99));
  ASSERT_NE(nullptr, connected);
  EXPECT_EQ(NextHopKind::kForward, connected->kind);
  EXPECT_EQ(dataplane::InterfaceId(uint16_t{1}), connected->egress);
  EXPECT_EQ(NeighborState::kIncomplete, connected->neighbor);

  EXPECT_EQ(nullptr, router->Resolve(Ip(203, 0, 113, 1)));
  Settle();
}

TEST_P(ConnectedRoutesTest, ARefusedTransactionLeavesNothingAndARetryConverges) {
  const auto derived = DerivedRoutes::Derive(One(Iface(1, Ip(198, 51, 100, 7), 24)));
  ASSERT_TRUE(derived.has_value());
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  const std::vector<NextHopId> ids = Ids(derived->size());
  auto ops = InstallationOps(*router, derived->entries(), ids);
  ASSERT_TRUE(ops.has_value());

  // One operation names a next hop nothing creates: the engine reserves every
  // operation, then refuses the transaction. None of the derived operations is
  // visible, not even the routes it had already placed during prepare.
  std::vector<Op> failing = *ops;
  failing.push_back(router->SetRouteOp(kDefaultRouteDomainId, P(Ip(203, 0, 113, 0), 24),
                                       NextHopId{99}));
  const auto refused = Apply(std::move(failing));
  EXPECT_EQ(Outcome::kRejected, refused.outcome);
  EXPECT_EQ(0u, router->route_count());
  EXPECT_EQ(0u, router->next_hop_count());
  EXPECT_EQ(nullptr, router->Resolve(Ip(198, 51, 100, 7)));
  Settle();

  // The same derivation, applied as it was built: it converges.
  const auto retried = Apply(*ops);
  EXPECT_EQ(Outcome::kApplied, retried.outcome);
  EXPECT_EQ(derived->size(), router->route_count());
  EXPECT_NE(nullptr, router->Resolve(Ip(198, 51, 100, 7)));
  Settle();
}

TEST_P(ConnectedRoutesTest, DuplicateInstallationIsIdempotent) {
  const auto derived = DerivedRoutes::Derive(One(Iface(1, Ip(192, 0, 2, 77), 24)));
  ASSERT_TRUE(derived.has_value());
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  const std::vector<NextHopId> ids = Ids(derived->size());
  const auto ops = InstallationOps(*router, derived->entries(), ids);
  ASSERT_TRUE(ops.has_value());

  ASSERT_EQ(Outcome::kApplied, Apply(*ops).outcome);
  const size_t routes = router->route_count();
  const size_t hops = router->next_hop_count();
  ASSERT_EQ(Outcome::kApplied, Apply(*ops).outcome);
  EXPECT_EQ(routes, router->route_count());
  EXPECT_EQ(hops, router->next_hop_count());
  ASSERT_NE(nullptr, router->Resolve(Ip(192, 0, 2, 77)));
  EXPECT_EQ(NextHopKind::kLocal, router->Resolve(Ip(192, 0, 2, 77))->kind);
  Settle();
}

TEST_P(ConnectedRoutesTest, AddressAddRemoveAndChangeUseStableKeys) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_.registry()));

  const auto before = DerivedRoutes::Derive(One(Iface(1, Ip(192, 0, 2, 77), 24)));
  ASSERT_TRUE(before.has_value());
  const auto before_ids = Ids(before->size());
  const auto before_ops = InstallationOps(*router, before->entries(), before_ids);
  ASSERT_TRUE(before_ops.has_value());
  ASSERT_EQ(Outcome::kApplied, Apply(*before_ops).outcome);
  const DerivedRoute *stable_connected = before->Find(Key(Ip(192, 0, 2, 0), 24));
  ASSERT_NE(nullptr, stable_connected);
  const RouteKey connected_key = stable_connected->key();

  // The same interface moves to another address on the same subnet: the
  // connected key is unchanged (it is the subnet's), the old local /32 is
  // stale, the new one appears.
  const auto changed = DerivedRoutes::Derive(One(Iface(1, Ip(192, 0, 2, 78), 24)));
  ASSERT_TRUE(changed.has_value());
  const std::vector<RouteKey> stale = StaleRouteKeys(before->entries(), changed->entries());
  ASSERT_EQ(1u, stale.size());
  EXPECT_EQ(Key(Ip(192, 0, 2, 77), 32), stale[0]);
  ASSERT_NE(nullptr, changed->Find(connected_key)) << "the connected key is stable";

  const auto changed_ids = Ids(changed->size(), 10);
  const auto changed_ops = InstallationOps(*router, changed->entries(), changed_ids);
  ASSERT_TRUE(changed_ops.has_value());
  std::vector<Op> ops = *changed_ops;
  for (Op &erase : EraseOps(*router, stale)) {
    ops.push_back(std::move(erase));
  }
  ASSERT_EQ(Outcome::kApplied, Apply(std::move(ops)).outcome);

  EXPECT_EQ(2u, router->route_count());
  ASSERT_NE(nullptr, router->Resolve(Ip(192, 0, 2, 78)));
  EXPECT_EQ(NextHopKind::kLocal, router->Resolve(Ip(192, 0, 2, 78))->kind);
  const NextHop *old_local = router->Resolve(Ip(192, 0, 2, 77));
  ASSERT_NE(nullptr, old_local) << "the connected /24 still covers it";
  EXPECT_EQ(NextHopKind::kForward, old_local->kind);
  EXPECT_EQ(dataplane::InterfaceId(uint16_t{1}), old_local->egress);

  // Remove the address entirely: both keys are stale, and the domain has no
  // derived route left.
  const auto removed = DerivedRoutes::Derive(std::vector<InterfaceAddress>{});
  ASSERT_TRUE(removed.has_value());
  const std::vector<RouteKey> all_stale = StaleRouteKeys(changed->entries(), removed->entries());
  EXPECT_EQ(2u, all_stale.size());
  const auto erased = Apply(EraseOps(*router, all_stale));
  ASSERT_EQ(Outcome::kApplied, erased.outcome);
  EXPECT_EQ(0u, router->route_count());
  Settle();
}

TEST_P(ConnectedRoutesTest, InstallationRefusesAnInconsistentPlan) {
  auto router = MakeRouter();
  const DerivedRoute connected{.domain = kDefaultRouteDomainId,
                               .kind = DerivedRouteKind::kConnected,
                               .prefix = P(Ip(10, 0, 0, 0), 24),
                               .interface = dataplane::InterfaceId(uint16_t{1}),
                               .egress = dataplane::InterfaceId(uint16_t{1})};
  const DerivedRoute local{.domain = kDefaultRouteDomainId,
                           .kind = DerivedRouteKind::kLocal,
                           .prefix = P(Ip(10, 0, 0, 1), 32),
                           .interface = dataplane::InterfaceId(uint16_t{1}),
                           .egress = dataplane::kInvalidInterfaceId};

  // One id per entry.
  const auto mismatch = InstallationOps(*router, std::span(&connected, 1), Ids(2));
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(DerivationError::kHopCountMismatch, mismatch.error());
  // Zero is not an id.
  const std::vector<NextHopId> zero = {kInvalidNextHopId};
  const auto zero_id = InstallationOps(*router, std::span(&connected, 1), zero);
  ASSERT_FALSE(zero_id.has_value());
  EXPECT_EQ(DerivationError::kInvalidHopId, zero_id.error());
  // One id cannot name two hops.
  const std::vector<DerivedRoute> both = {connected, local};
  const std::vector<NextHopId> shared = {NextHopId{1}, NextHopId{1}};
  const auto duplicate_id = InstallationOps(*router, both, shared);
  ASSERT_FALSE(duplicate_id.has_value());
  EXPECT_EQ(DerivationError::kDuplicateHopId, duplicate_id.error());
  // A local route that names an interface.
  const DerivedRoute local_with_egress{.domain = kDefaultRouteDomainId,
                                       .kind = DerivedRouteKind::kLocal,
                                       .prefix = P(Ip(10, 0, 0, 1), 32),
                                       .interface = dataplane::InterfaceId(uint16_t{2}),
                                       .egress = dataplane::InterfaceId(uint16_t{2})};
  const std::vector<NextHopId> one = {NextHopId{1}};
  const auto bad_local = InstallationOps(*router, std::span(&local_with_egress, 1), one);
  ASSERT_FALSE(bad_local.has_value());
  EXPECT_EQ(DerivationError::kInvalidRoute, bad_local.error());
  // A key twice.
  const std::vector<DerivedRoute> duplicate = {connected, connected};
  const std::vector<NextHopId> two = {NextHopId{1}, NextHopId{2}};
  const auto duplicate_key = InstallationOps(*router, duplicate, two);
  ASSERT_FALSE(duplicate_key.has_value());
  EXPECT_EQ(DerivationError::kConflictingRoute, duplicate_key.error());
}

TEST_P(ConnectedRoutesTest, SharedHopIdAcrossIdenticalHopsDeduplicatesSetNextHopOp) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  // Two distinct interfaces with /32 addresses derive two local routes.
  // Both routes have identical LocalNextHop() hops and can share one NextHopId.
  const std::vector<InterfaceAddress> ifaces = {
      Iface(1, Ip(10, 0, 0, 1), 32),
      Iface(2, Ip(10, 0, 0, 2), 32),
  };
  const auto derived = DerivedRoutes::Derive(ifaces);
  ASSERT_TRUE(derived.has_value());
  ASSERT_EQ(2u, derived->size());

  const std::vector<NextHopId> shared_id = {NextHopId{5}, NextHopId{5}};
  const auto ops = InstallationOps(*router, derived->entries(), shared_id);
  ASSERT_TRUE(ops.has_value());
  // Exactly 3 ops: 1 SetNextHopOp for id 5, and 2 SetRouteOp for the two prefixes.
  EXPECT_EQ(3u, ops->size());

  const auto outcome = Apply(*ops);
  EXPECT_EQ(Outcome::kApplied, outcome.outcome);
  EXPECT_EQ(2u, router->route_count());
  ASSERT_NE(nullptr, router->Resolve(Ip(10, 0, 0, 1)));
  EXPECT_EQ(NextHopKind::kLocal, router->Resolve(Ip(10, 0, 0, 1))->kind);
  ASSERT_NE(nullptr, router->Resolve(Ip(10, 0, 0, 2)));
  EXPECT_EQ(NextHopKind::kLocal, router->Resolve(Ip(10, 0, 0, 2))->kind);
}

TEST_P(ConnectedRoutesTest, PrefixLengthFromMask) {
  EXPECT_EQ(0, PrefixLengthFromMask(0));
  EXPECT_EQ(8, PrefixLengthFromMask(0xff000000));
  EXPECT_EQ(16, PrefixLengthFromMask(0xffff0000));
  EXPECT_EQ(24, PrefixLengthFromMask(0xffffff00));
  EXPECT_EQ(31, PrefixLengthFromMask(0xfffffffe));
  EXPECT_EQ(32, PrefixLengthFromMask(0xffffffff));
  EXPECT_FALSE(PrefixLengthFromMask(0xffffff0f).has_value());
  EXPECT_FALSE(PrefixLengthFromMask(0xff00ff00).has_value());
}

TEST_P(ConnectedRoutesTest, LocalDeliveryIsNotForwarding) {
  // The installed set, seen through the chain both callers use: the node's own
  // address is delivery (D-178), another host on the subnet is the connected
  // hop, a routed prefix forwards, and a miss has no route.
  const auto derived = DerivedRoutes::Derive(One(Iface(1, Ip(192, 0, 2, 77), 24)));
  ASSERT_TRUE(derived.has_value());
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  const std::vector<NextHopId> ids = Ids(derived->size(), 10);
  const auto derived_ops = InstallationOps(*router, derived->entries(), ids);
  ASSERT_TRUE(derived_ops.has_value());
  ASSERT_EQ(Outcome::kApplied, Apply(*derived_ops).outcome);
  NextHop routed;
  routed.egress = dataplane::InterfaceId(uint16_t{2});
  routed.neighbor = NeighborState::kResolved;
  ASSERT_EQ(Outcome::kApplied,
            Apply({router->SetNextHopOp(NextHopId{20}, routed),
                   router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                      NextHopId{20})})
                .outcome);

  const auto frame = [](uint32_t dst) {
    std::vector<uint8_t> f(60, 0);
    f[12] = 0x08;
    f[14] = 0x45;
    f[17] = 28;
    f[22] = 64;
    f[23] = 17;
    f[30] = static_cast<uint8_t>(dst >> 24);
    f[31] = static_cast<uint8_t>(dst >> 16);
    f[32] = static_cast<uint8_t>(dst >> 8);
    f[33] = static_cast<uint8_t>(dst);
    f[38] = 0;  // UDP length: the header alone, which the parse checks
    f[39] = 8;
    return f;
  };
  const auto local = frame(Ip(192, 0, 2, 77));
  const FrameRoute delivered = ResolveFrame(*router, kDefaultRouteDomainId, local, local.size());
  EXPECT_EQ(ForwardVerdict::kLocalDelivery, delivered.verdict);
  EXPECT_EQ(nullptr, delivered.hop) << "there is nothing to write";
  EXPECT_EQ(dataplane::kInvalidInterfaceId, delivered.egress);

  const auto host = frame(Ip(192, 0, 2, 99));
  const FrameRoute connected = ResolveFrame(*router, kDefaultRouteDomainId, host, host.size());
  EXPECT_EQ(ForwardVerdict::kUnresolved, connected.verdict);
  EXPECT_EQ(dataplane::InterfaceId(uint16_t{1}), connected.egress);

  const auto other = frame(Ip(10, 1, 2, 3));
  const FrameRoute forwarded = ResolveFrame(*router, kDefaultRouteDomainId, other, other.size());
  EXPECT_EQ(ForwardVerdict::kForward, forwarded.verdict);
  EXPECT_EQ(dataplane::InterfaceId(uint16_t{2}), forwarded.egress);

  const auto miss = frame(Ip(203, 0, 113, 1));
  EXPECT_EQ(ForwardVerdict::kNoRoute,
            ResolveFrame(*router, kDefaultRouteDomainId, miss, miss.size()).verdict);

  // The batch path settles every frame the same way.
  std::array<std::span<const uint8_t>, 4> frames = {local, host, other, miss};
  std::array<size_t, 4> lengths = {local.size(), host.size(), other.size(), miss.size()};
  std::array<FrameRoute, 4> out;
  ResolveFrames(*router, kDefaultRouteDomainId, frames, lengths, out);
  EXPECT_EQ(ForwardVerdict::kLocalDelivery, out[0].verdict);
  EXPECT_EQ(ForwardVerdict::kUnresolved, out[1].verdict);
  EXPECT_EQ(ForwardVerdict::kForward, out[2].verdict);
  EXPECT_EQ(ForwardVerdict::kNoRoute, out[3].verdict);
  Settle();
}

BESS_INSTANTIATE_ROUTE_BACKENDS(ConnectedRoutesTest);

}  // namespace
}  // namespace bess::route
