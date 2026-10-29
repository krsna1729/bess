// SPDX-License-Identifier: BSD-3-Clause

// A Router enrolled in the transaction engine (Decision D-023): next hops and
// routes as two resources, changed together or not at all, with routes
// placed invisibly during prepare, readers resolving throughout, and every
// allocation of a transaction failed in turn.

#include <gtest/gtest.h>
#include <rte_malloc.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/runtime_state.h"
#include "dataplane/slot_resource.h"
#include "dataplane/transaction_engine.h"
#include "route/route_domain.h"
#include "route/router.h"
#include "testing/allocation_faults.h"
#include "testing/route_backends.h"

namespace bess::route {
namespace {

using dataplane::Op;
using dataplane::TransactionEngine;
using Outcome = TransactionEngine::Outcome;

Ipv4Prefix P(uint32_t addr, uint8_t len) {
  return Ipv4Prefix::Make(addr, len).value();
}

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a << 24 | b << 16 | c << 8 | d;
}

NextHop Hop(uint32_t egress) {
  NextHop hop;
  hop.egress = bess::dataplane::InterfaceId(egress);
  hop.neighbor = NeighborState::kResolved;
  return hop;
}

// The egress `dst` resolves to, or -1 for a miss.
int EgressOf(const Router &router, uint32_t dst) {
  const NextHop *hop = router.Resolve(dst);
  return hop == nullptr ? -1 : static_cast<int>(hop->egress.value());
}

using MakeRouterFn = std::function<std::unique_ptr<Router>()>;

class RouterTransactionTest : public bess::testing::RouteBackendTest {
 protected:
  // IPv4 only unless `ipv6`: a test that writes IPv6 routes asks for them.
  std::unique_ptr<Router> MakeRouter(uint32_t tbl8_groups = 64,
                                     size_t next_hops = 64,
                                     size_t domains = 1, size_t groups = 0,
                                     bool ipv6 = false) {
    Router::Config config{.ipv4 = bess::testing::TestRouteConfig()};
    config.ipv4->max_routes = 4096;
    config.ipv4->tbl8_groups = tbl8_groups;
    if (ipv6) {
      config.ipv6 = Ipv6RouteTable::Config{.max_routes = 4096, .tbl8_groups = 256};
    }
    auto router = Router::Create("rt", config, next_hops,
                                 bess::runtime::runtime().rcu(), domains, groups);
    EXPECT_TRUE(router.has_value());
    // The direct API still wants CreateDomain first (kUnknownDomain), so a test
    // that writes routes directly gets the domains it asked for; the resource
    // path creates a domain on demand (D-158), which
    // ATransactionCreatesTheDomainItFirstWritesInto covers.
    for (uint32_t d = 1; d < domains; d++) {
      EXPECT_TRUE((*router)->CreateDomain(RouteDomainId(d), config));
    }
    return std::move(router).value();
  }

  TransactionEngine::Result Apply(std::vector<Op> ops) {
    return engine_.Apply(ops);
  }

  void Settle() {
    while (engine_.ReclaimRetired() != 0) {
    }
    bess::runtime::runtime().rcu().Drain();
  }

  TransactionEngine engine_{bess::runtime::runtime().rcu()};
};

TEST_P(RouterTransactionTest, TwoRoutersShareAModulePrefixWithoutSharingState) {
  const RouterResourceNames inner_names{
      .next_hops = "upf/next_hops",
      .neighbors = "upf/neighbors",
      .next_hop_state = "upf/next_hop_state",
      .routes = "upf/routes",
      .groups = "upf/groups"};
  const RouterResourceNames outer_names{
      .next_hops = "upf/transport_next_hops",
      .neighbors = "upf/transport_neighbors",
      .next_hop_state = "upf/transport_next_hop_state",
      .routes = "upf/transport_routes",
      .groups = "upf/transport_groups"};
  Router::Config config{.ipv4 = bess::testing::TestRouteConfig()};
  auto inner = Router::Create("inner", config, 8,
                              bess::runtime::runtime().rcu(), 1, 0, inner_names);
  auto outer = Router::Create("outer", config, 8,
                              bess::runtime::runtime().rcu(), 1, 0, outer_names);
  ASSERT_TRUE(inner);
  ASSERT_TRUE(outer);
  ASSERT_TRUE((*inner)->Enroll(engine_.registry()));
  ASSERT_TRUE((*outer)->Enroll(engine_.registry()));
  const auto net = P(Ip(10, 0, 0, 0), 8);
  ASSERT_EQ(Apply({(*inner)->SetRouteOp(net, NextHopId(1)),
                   (*outer)->SetRouteOp(net, NextHopId(1)),
                   (*inner)->SetNextHopOp(NextHopId(1), Hop(11)),
                   (*outer)->SetNextHopOp(NextHopId(1), Hop(22))}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressOf(**inner, Ip(10, 0, 0, 1)), 11);
  EXPECT_EQ(EgressOf(**outer, Ip(10, 0, 0, 1)), 22);
  // Reference checks use each router's overridden names, not its build name
  // or the other router's identical next-hop id.
  EXPECT_EQ(Apply({(*outer)->RemoveNextHopOp(NextHopId(1))}).outcome,
            Outcome::kRejected);
  ASSERT_EQ(Apply({(*inner)->RemoveRouteOp(net),
                   (*inner)->RemoveNextHopOp(NextHopId(1))}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressOf(**inner, Ip(10, 0, 0, 1)), -1);
  EXPECT_EQ(EgressOf(**outer, Ip(10, 0, 0, 1)), 22);
}

TEST_P(RouterTransactionTest, EnrolledRoutersAreWrittenOnlyThroughTheEngine) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));  // before: fine
  ASSERT_TRUE(router->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  auto enrolled = router->Enroll(engine_.registry());
  ASSERT_FALSE(enrolled) << "enrolled with routes the ledger cannot see";
  EXPECT_NE(enrolled.error().find("before adding routes"), std::string::npos);
  ASSERT_TRUE(router->RemoveRoute(P(Ip(10, 0, 0, 0), 8)));

  ASSERT_TRUE(router->Enroll(engine_.registry()));
  EXPECT_TRUE(router->enrolled());
  EXPECT_EQ(router->next_hops_resource(), "rt/next_hops");
  EXPECT_EQ(router->routes_resource(), "rt/routes");
  EXPECT_FALSE(router->Enroll(engine_.registry()));
  EXPECT_EQ(router->SetNextHop(NextHopId(2), Hop(2)).error(),
            RouteError::kEnrolled);
  EXPECT_EQ(router->RemoveNextHop(NextHopId(1)).error(),
            RouteError::kEnrolled);
  EXPECT_EQ(router->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)).error(),
            RouteError::kEnrolled);
  EXPECT_EQ(router->RemoveRoute(P(Ip(10, 0, 0, 0), 8)).error(),
            RouteError::kEnrolled);

  // The next hop that existed before enrollment is usable.
  ASSERT_EQ(Apply({router->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressOf(*router, Ip(10, 9, 9, 9)), 1);
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 1u);
}

// D-158: a domain a transaction first writes a route into is created by that
// write, so `max_domains` bounds the ids without allocating every domain's FIB.
// The direct API keeps CreateDomain explicit, an out-of-range domain is refused
// with nothing created, and an erase in a missing domain is still "not found".
TEST_P(RouterTransactionTest, ATransactionCreatesTheDomainItFirstWritesInto) {
  LpmRouteTable::Config config = bess::testing::TestRouteConfig();
  config.max_routes = 4096;
  auto made = Router::Create("rtlazy", Router::Config{.ipv4 = config}, /*max_next_hops=*/8,
                             bess::runtime::runtime().rcu(), /*max_domains=*/4,
                             /*max_groups=*/0);
  ASSERT_TRUE(made.has_value());
  auto router = std::move(*made);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  // Only the default domain exists: the other three cost nothing yet.
  EXPECT_EQ(1u, router->domain_count());
  EXPECT_FALSE(router->HasDomain(RouteDomainId(2)));

  const Ipv4Prefix net = P(Ip(10, 0, 0, 0), 8);
  ASSERT_EQ(Apply({router->SetRouteOp(RouteDomainId(2), net, NextHopId(1)),
                   router->SetNextHopOp(NextHopId(1), Hop(2))})
                .outcome,
            Outcome::kApplied);
  EXPECT_TRUE(router->HasDomain(RouteDomainId(2)));
  EXPECT_EQ(2u, router->domain_count());
  const NextHop *hop = router->Resolve(RouteDomainId(2), Ip(10, 1, 2, 3));
  ASSERT_NE(nullptr, hop);
  EXPECT_EQ(2u, hop->egress.value());
  EXPECT_EQ(router->route_count(RouteDomainId(2)), 1u);
  EXPECT_EQ(router->LookupRoute(kDefaultRouteDomainId, Ip(10, 1, 2, 3)),
            kInvalidNextHopId)
      << "the default domain did not gain the route";

  // Beyond `max_domains`: refused, and no domain appears.
  auto out_of_range =
      Apply({router->SetRouteOp(RouteDomainId(9), net, NextHopId(1))});
  ASSERT_EQ(out_of_range.outcome, Outcome::kRejected);
  EXPECT_FALSE(router->HasDomain(RouteDomainId(9)));
  EXPECT_EQ(2u, router->domain_count());

  // An erase in a domain that does not exist creates nothing.
  auto erase = Apply({router->RemoveRouteOp(RouteDomainId(3), net)});
  ASSERT_EQ(erase.outcome, Outcome::kRejected);
  EXPECT_NE(erase.ops[0].error.find("not found"), std::string::npos);
  EXPECT_FALSE(router->HasDomain(RouteDomainId(3)));
  EXPECT_EQ(2u, router->domain_count());
}

// Groups made through the direct API stay out of the engine's ledger: a router
// that has them cannot enroll, and an enrolled router takes groups only
// through transactions.
TEST_P(RouterTransactionTest, DirectGroupsAndEnrollmentExcludeEachOther) {
  LpmRouteTable::Config config = bess::testing::TestRouteConfig();
  auto made = Router::Create("rtg", Router::Config{.ipv4 = config}, 8, bess::runtime::runtime().rcu(),
                             /*max_domains=*/1, /*max_groups=*/4);
  ASSERT_TRUE(made.has_value());
  auto router = std::move(made).value();
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  const NextHopId members[] = {NextHopId(1)};
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(1), members));
  auto enrolled = router->Enroll(engine_.registry());
  ASSERT_FALSE(enrolled);
  EXPECT_NE(enrolled.error().find("before adding next-hop groups"), std::string::npos);

  ASSERT_TRUE(router->RemoveNextHopGroup(NextHopGroupId(1)));
  Settle();
  EXPECT_EQ(0u, router->ReclaimRetired());
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  EXPECT_EQ(RouteError::kEnrolled,
            router->SetNextHopGroup(NextHopGroupId(1), members).error());
  EXPECT_EQ(RouteError::kEnrolled,
            router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                             NextHopGroupId(1))
                .error());
  EXPECT_EQ(RouteError::kEnrolled,
            router->RemoveNextHopGroup(NextHopGroupId(1)).error());
}

TEST_P(RouterTransactionTest, NextHopsAndRoutesChangeTogether) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  // Given in any order: the engine publishes next hops before routes.
  ASSERT_EQ(Apply({router->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
                   router->SetRouteOp(P(Ip(10, 1, 0, 0), 16), NextHopId(2)),
                   router->SetNextHopOp(NextHopId(1), Hop(11)),
                   router->SetNextHopOp(NextHopId(2), Hop(12))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressOf(*router, Ip(10, 0, 0, 1)), 11);
  EXPECT_EQ(EgressOf(*router, Ip(10, 1, 0, 1)), 12);
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 1u);

  // A route to a missing next hop, and removing a next hop in use: rejected,
  // nothing changes.
  auto r = Apply({router->SetRouteOp(P(Ip(11, 0, 0, 0), 8), NextHopId(3))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(EgressOf(*router, Ip(11, 0, 0, 1)), -1);
  EXPECT_EQ(router->route_count(), 2u);
  r = Apply({router->RemoveNextHopOp(NextHopId(2))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(EgressOf(*router, Ip(10, 1, 0, 1)), 12);

  // Re-point, then remove the route and its next hop together.
  ASSERT_EQ(Apply({router->SetRouteOp(P(Ip(10, 1, 0, 0), 16), NextHopId(1)),
                   router->RemoveNextHopOp(NextHopId(2))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressOf(*router, Ip(10, 1, 0, 1)), 11);
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 2u);
  ASSERT_EQ(Apply({router->RemoveRouteOp(P(Ip(10, 1, 0, 0), 16)),
                   router->RemoveRouteOp(P(Ip(10, 0, 0, 0), 8)),
                   router->RemoveNextHopOp(NextHopId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressOf(*router, Ip(10, 1, 0, 1)), -1);
  EXPECT_EQ(router->route_count(), 0u);
  Settle();
  EXPECT_EQ(router->next_hop_count(), 0u);
  // Destroyed enrolled: both resources leave with it.
  router.reset();
  const auto gone = Apply({Op::Erase("rt/routes", Router::RouteKey(
                                                     P(Ip(10, 0, 0, 0), 8)))});
  ASSERT_EQ(gone.outcome, Outcome::kRejected);
  EXPECT_NE(gone.ops[0].error.find("unknown resource"), std::string::npos);
}

// A new route is placed during prepare with what its addresses resolve to
// today; observed from inside the publication window, nothing has changed.
Router *g_router = nullptr;
std::vector<int> g_seen;
std::vector<uint32_t> g_probe;

void ResolveInWindow(bool entering) {
  if (entering) {
    g_seen.clear();
    for (uint32_t dst : g_probe) {
      g_seen.push_back(EgressOf(*g_router, dst));
    }
  }
}

TEST_P(RouterTransactionTest, RoutesBeingPlacedChangeNoLookup) {
  for (bool with_default : {false, true}) {
    SCOPED_TRACE(with_default ? "with a default route" : "no default route");
    auto router = MakeRouter();
    ASSERT_TRUE(router->Enroll(engine_.registry()));
    std::vector<Op> base = {router->SetNextHopOp(NextHopId(1), Hop(1)),
                            router->SetNextHopOp(NextHopId(2), Hop(2)),
                            router->SetNextHopOp(NextHopId(9), Hop(9)),
                            router->SetRouteOp(P(Ip(10, 0, 0, 0), 8),
                                               NextHopId(1))};
    if (with_default) {
      base.push_back(router->SetRouteOp(P(0, 0), NextHopId(9)));
    }
    ASSERT_EQ(Apply(base).outcome, Outcome::kApplied);
    g_probe = {Ip(10, 1, 5, 5), Ip(10, 1, 2, 5), Ip(10, 2, 0, 1),
               Ip(11, 0, 0, 1), Ip(12, 3, 4, 5)};
    const std::vector<int> before = {1, 1, 1, with_default ? 9 : -1,
                                     with_default ? 9 : -1};
    g_router = router.get();
    dataplane::internal::g_publish_window_hook = ResolveInWindow;
    // Nested new prefixes (the /24 inside the new /16), a /25 needing a
    // tbl8 group, and one with no covering route.
    const auto r = Apply({router->SetRouteOp(P(Ip(10, 1, 0, 0), 16),
                                             NextHopId(2)),
                          router->SetRouteOp(P(Ip(10, 1, 2, 0), 24),
                                             NextHopId(9)),
                          router->SetRouteOp(P(Ip(12, 3, 4, 0), 25),
                                             NextHopId(2)),
                          router->SetRouteOp(P(Ip(11, 0, 0, 0), 8),
                                             NextHopId(2))});
    dataplane::internal::g_publish_window_hook = nullptr;
    ASSERT_EQ(r.outcome, Outcome::kApplied);
    EXPECT_EQ(g_seen, before);
    std::vector<int> after;
    for (uint32_t dst : g_probe) {
      after.push_back(EgressOf(*router, dst));
    }
    EXPECT_EQ(after, (std::vector<int>{2, 9, 1, 2, 2}));
  }
}

TEST_P(RouterTransactionTest, RoutesThatDoNotFitRejectWithNothingVisible) {
  auto router = MakeRouter(/*tbl8_groups=*/2);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1))}).outcome,
            Outcome::kApplied);
  // Three /25s in three /24s need three tbl8 groups; there are two.
  std::vector<Op> ops;
  for (uint32_t i = 0; i < 3; i++) {
    ops.push_back(router->SetRouteOp(P(Ip(20, 0, i, 0), 25), NextHopId(1)));
  }
  const auto r = Apply(ops);
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[2].status, TransactionEngine::OpStatus::kFailed);
  EXPECT_NE(r.ops[2].error.find("full"), std::string::npos) << r.ops[2].error;
  EXPECT_EQ(router->route_count(), 0u);
  EXPECT_EQ(EgressOf(*router, Ip(20, 0, 0, 1)), -1);
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 0u);
  // The groups the placeholders took came back.
  ops.pop_back();
  ASSERT_EQ(Apply(ops).outcome, Outcome::kApplied);
  EXPECT_EQ(EgressOf(*router, Ip(20, 0, 1, 1)), 1);
}

// One routes resource covers every domain; the domain is part of the key. The
// default domain's key is the byte string the pre-domain router used.
TEST_P(RouterTransactionTest, RoutesInTwoDomainsInOneTransaction) {
  auto router = MakeRouter(/*tbl8_groups=*/64, /*next_hops=*/64, /*domains=*/3);
  const RouteDomainId d1(1), d2(2);
  ASSERT_TRUE(router->Enroll(engine_.registry()));

  const Ipv4Prefix net = P(Ip(10, 0, 0, 0), 8);
  EXPECT_EQ(Router::RouteKey(net),
            dataplane::EncodeKey(uint64_t{Ip(10, 0, 0, 0)} << 8 | 8))
      << "the default domain's key changed";
  EXPECT_EQ(Router::RouteKey(kDefaultRouteDomainId, net), Router::RouteKey(net));
  EXPECT_EQ(Router::RouteKey(d2, net),
            dataplane::EncodeKey(uint64_t{2} << 40 |
                                 uint64_t{Ip(10, 0, 0, 0)} << 8 | 8));
  EXPECT_EQ(router->SetRouteOp(d1, net, NextHopId(1)).key,
            Router::RouteKey(RouteKey{d1, net}));
  EXPECT_NE(Router::RouteKey(d1, net), Router::RouteKey(d2, net));

  ASSERT_EQ(Apply({router->SetRouteOp(d1, net, NextHopId(1)),
                   router->SetRouteOp(d2, net, NextHopId(2)),
                   router->SetRouteOp(net, NextHopId(1)),
                   router->SetRouteOp(d2, P(0, 0), NextHopId(3)),
                   router->SetNextHopOp(NextHopId(1), Hop(11)),
                   router->SetNextHopOp(NextHopId(2), Hop(12)),
                   router->SetNextHopOp(NextHopId(3), Hop(13))})
                .outcome,
            Outcome::kApplied);
  auto egress = [&](RouteDomainId d, uint32_t dst) {
    const NextHop *hop = router->Resolve(d, dst);
    return hop == nullptr ? -1 : static_cast<int>(hop->egress.value());
  };
  EXPECT_EQ(egress(d1, Ip(10, 1, 1, 1)), 11);
  EXPECT_EQ(egress(d2, Ip(10, 1, 1, 1)), 12);
  EXPECT_EQ(egress(kDefaultRouteDomainId, Ip(10, 1, 1, 1)), 11);
  EXPECT_EQ(egress(d2, Ip(99, 1, 1, 1)), 13) << "d2's default route";
  EXPECT_EQ(egress(d1, Ip(99, 1, 1, 1)), -1);
  EXPECT_EQ(router->route_count(), 4u);
  EXPECT_EQ(router->route_count(d2), 2u);
  // The reference ledger spans domains: next hop 1 is named in two.
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 2u);
  EXPECT_EQ(router->RouteReferences(NextHopId(2)), 1u);

  // The same prefix in another domain is another route: erasing a route that
  // exists only elsewhere is "not found", and a domain the router does not
  // have is refused.
  auto missing = Apply({router->RemoveRouteOp(d1, P(0, 0))});
  ASSERT_EQ(missing.outcome, Outcome::kRejected);
  EXPECT_NE(missing.ops[0].error.find("not found"), std::string::npos);
  auto unknown = Apply({router->SetRouteOp(RouteDomainId(7), net, NextHopId(1))});
  ASSERT_EQ(unknown.outcome, Outcome::kRejected);
  EXPECT_EQ(router->route_count(), 4u);

  // Erasing a referenced next hop is refused until every domain's route to it
  // goes; the cascade in one transaction then succeeds.
  ASSERT_EQ(Apply({router->RemoveNextHopOp(NextHopId(1))}).outcome,
            Outcome::kRejected);
  ASSERT_EQ(Apply({router->RemoveRouteOp(d1, net),
                   router->RemoveNextHopOp(NextHopId(1))})
                .outcome,
            Outcome::kRejected)
      << "the default domain's route still names it";
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 2u);
  ASSERT_EQ(Apply({router->RemoveRouteOp(d1, net),
                   router->RemoveRouteOp(net),
                   router->RemoveNextHopOp(NextHopId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 0u);
  EXPECT_EQ(egress(d1, Ip(10, 1, 1, 1)), -1);
  EXPECT_EQ(egress(d2, Ip(10, 1, 1, 1)), 12) << "d2 is untouched";
  EXPECT_EQ(router->route_count(), 2u);

  // The set of domains and wholesale replacement are not transactional.
  EXPECT_EQ(router->CreateDomain(RouteDomainId(1), {}).error(),
            RouteError::kEnrolled);
  EXPECT_EQ(router->RemoveDomain(d1).error(), RouteError::kEnrolled);
  EXPECT_EQ(router->ReplaceRouteSetAtomic(d1, {}).error(),
            RouteError::kEnrolled);
  Settle();
}

// A new route is placed per domain during prepare, and a domain's tbl8 pool
// is its own: filling d1's rejects the transaction without touching d2.
TEST_P(RouterTransactionTest, PlacementCapacityIsPerDomain) {
  auto router = MakeRouter(/*tbl8_groups=*/2, /*next_hops=*/8, /*domains=*/3);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1))}).outcome,
            Outcome::kApplied);
  std::vector<Op> ops;
  for (uint32_t i = 0; i < 2; i++) {
    ops.push_back(router->SetRouteOp(RouteDomainId(1), P(Ip(20, 0, i, 0), 25),
                                     NextHopId(1)));
    ops.push_back(router->SetRouteOp(RouteDomainId(2), P(Ip(20, 0, i, 0), 25),
                                     NextHopId(1)));
  }
  ASSERT_EQ(Apply(ops).outcome, Outcome::kApplied) << "two groups each fit";
  ops = {router->SetRouteOp(RouteDomainId(1), P(Ip(21, 0, 0, 0), 25),
                            NextHopId(1)),
         router->SetRouteOp(RouteDomainId(2), P(Ip(22, 0, 0, 0), 24),
                            NextHopId(1))};
  const auto r = Apply(ops);
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[0].status, TransactionEngine::OpStatus::kFailed);
  EXPECT_EQ(router->route_count(RouteDomainId(1)), 2u);
  EXPECT_EQ(router->route_count(RouteDomainId(2)), 2u);
  EXPECT_EQ(router->Resolve(RouteDomainId(2), Ip(22, 0, 0, 1)), nullptr)
      << "the rejected transaction's placeholder was removed";
}

// Another resource referencing the router's next hops: an action naming the
// next hop it forwards to, set up with the route in one transaction.
struct ActionTag;
using ActionId = dataplane::StrongId<ActionTag, uint32_t>;
struct Action {
  NextHopId via;
};

TEST_P(RouterTransactionTest, OtherResourcesReferenceNextHops) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  dataplane::SlotTable<ActionId, Action> actions(16);
  const std::string hops = router->next_hops_resource();
  dataplane::SlotResource<ActionId, Action> actions_res(
      "actions", actions,
      [hops](const Action &a) {
        return std::vector<dataplane::Reference>{
            {hops, dataplane::EncodeKey(a.via)}};
      },
      {hops});
  ASSERT_TRUE(engine_.Register(&actions_res));
  ASSERT_EQ(Apply({Op::Upsert("actions", dataplane::EncodeKey(ActionId(1)),
                              std::any(Action{NextHopId(4)})),
                   router->SetNextHopOp(NextHopId(4), Hop(4)),
                   router->SetRouteOp(P(Ip(30, 0, 0, 0), 8), NextHopId(4))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(router->RouteReferences(NextHopId(4)), 1u)
      << "routes only: the action's reference is the engine's, not a route";
  EXPECT_EQ(engine_.ReferenceCount(hops, dataplane::EncodeKey(NextHopId(4))), 2u)
      << "the engine counts the action too, and that is what protects the hop";

  // The router's resources cannot leave while "actions" may reference them.
  const std::string names[] = {router->routes_resource(), hops};
  EXPECT_FALSE(engine_.Unregister(names));
  ASSERT_EQ(Apply({Op::Erase("actions", dataplane::EncodeKey(ActionId(1)))})
                .outcome,
            Outcome::kApplied);
  Settle();
  ASSERT_TRUE(engine_.Unregister("actions"));
}

// A resource that declares only "<rt>/next_hops" may name a bound id too
// (D-187): the reference is held by "<rt>/neighbors", as a route's is, so the
// binding cannot go while it is named -- and a static id stays protected in
// "<rt>/next_hops". An id unbound and named in one transaction is missing.
TEST_P(RouterTransactionTest, OtherResourcesReferenceBoundNextHopsThroughNextHops) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->SetNeighborBindingPolicy(
      {.capacity = 8,
       .validate = [](const NeighborKey &) -> std::expected<void, std::string> { return {}; }}));
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  dataplane::SlotTable<ActionId, Action> actions(16);
  const std::string hops = router->next_hops_resource();
  const std::string neighbors = router->neighbors_resource();
  dataplane::SlotResource<ActionId, Action> actions_res(
      "actions", actions,
      [hops](const Action &a) {
        return std::vector<dataplane::Reference>{
            {hops, dataplane::EncodeKey(a.via)}};
      },
      {hops});
  ASSERT_TRUE(engine_.Register(&actions_res));
  // A failed assertion returns early: "actions" must not outlive this scope in
  // the engine, or the router's teardown refuses (it may reference the router).
  struct ReleaseActions {
    TransactionEngine &engine;
    ~ReleaseActions() {
      while (engine.ReclaimRetired() != 0) {
      }
      bess::runtime::runtime().rcu().Drain();
      EXPECT_TRUE(engine.ReleaseForTeardown(std::vector<std::string>{"actions"}));
    }
  } release_actions{engine_};
  const auto use = [](uint32_t action, uint32_t hop) {
    return Op::Upsert("actions", dataplane::EncodeKey(ActionId(action)),
                      std::any(Action{NextHopId(hop)}));
  };
  const auto drop = [](uint32_t action) {
    return Op::Erase("actions", dataplane::EncodeKey(ActionId(action)));
  };
  const auto key = [](uint32_t hop) { return dataplane::EncodeKey(NextHopId(hop)); };
  const NeighborKey neighbor(dataplane::InterfaceId(7), Ip(192, 0, 2, 7));

  // The referrers first: the engine reserves the hop and the binding first.
  auto r = Apply({use(1, 4), use(2, 5), router->SetNextHopOp(NextHopId(4), Hop(4)),
                  router->BindNeighborOp(NextHopId(5), neighbor)});
  ASSERT_EQ(r.outcome, Outcome::kApplied) << r.ops[0].error << "|" << r.ops[1].error;
  EXPECT_EQ(engine_.ReferenceCount(hops, key(4)), 1u);
  EXPECT_EQ(engine_.ReferenceCount(neighbors, key(5)), 1u) << "held by the binding";
  EXPECT_EQ(engine_.ReferenceCount(hops, key(5)), 0u);

  r = Apply({router->UnbindNeighborOp(NextHopId(5))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("still referenced"), std::string::npos) << r.ops[0].error;
  EXPECT_TRUE(router->IsNeighborBound(NextHopId(5)));
  r = Apply({router->RemoveNextHopOp(NextHopId(4))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("still referenced"), std::string::npos) << r.ops[0].error;

  // Unbound in the transaction that names it: missing.
  r = Apply({drop(2), router->UnbindNeighborOp(NextHopId(5)), use(3, 5)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[2].error.find("references missing " + hops), std::string::npos)
      << r.ops[2].error;
  EXPECT_TRUE(router->IsNeighborBound(NextHopId(5)));
  EXPECT_EQ(engine_.ReferenceCount(neighbors, key(5)), 1u) << "nothing of it applied";

  // The referrer and the binding go together; an id that changes kind while
  // nothing names it is counted where it is held now.
  ASSERT_EQ(Apply({drop(2), router->UnbindNeighborOp(NextHopId(5))}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.ReferenceCount(neighbors, key(5)), 0u);
  ASSERT_EQ(Apply({drop(1), router->RemoveNextHopOp(NextHopId(4))}).outcome,
            Outcome::kApplied);
  Settle();
  ASSERT_EQ(Apply({router->BindNeighborOp(NextHopId(4), neighbor), use(1, 4)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(engine_.ReferenceCount(neighbors, key(4)), 1u);
  EXPECT_EQ(engine_.ReferenceCount(hops, key(4)), 0u);
  EXPECT_NE(Apply({router->UnbindNeighborOp(NextHopId(4))}).outcome, Outcome::kApplied);
  ASSERT_EQ(Apply({drop(1), router->UnbindNeighborOp(NextHopId(4))}).outcome,
            Outcome::kApplied);
  Settle();
  ASSERT_TRUE(engine_.Unregister("actions"));
}

// Readers resolve through the router while transactions add a next hop and a
// route per session, and remove an older session's route and next hop. A
// resolved address only ever reaches its own session's next hop.
TEST_P(RouterTransactionTest, ResolvesWhileTransactionsRun) {
  constexpr uint32_t kSessions = 2000;  // next hop ids 1..kSessions
  constexpr uint32_t kLive = 300;
  constexpr uint32_t kSteps = 6000;
  auto router = MakeRouter(/*tbl8_groups=*/64, /*next_hops=*/kSessions);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  // Session s: 40.x.y.0/24 with x.y = s, via next hop s + 1, egress s % 4000.
  auto prefix = [](uint32_t s) {
    return P(Ip(40, (s >> 8) & 0xff, s & 0xff, 0), 24);
  };
  auto establish = [&](uint32_t s) {
    return std::vector<Op>{
        router->SetNextHopOp(NextHopId(s + 1),
                             Hop(static_cast<uint32_t>(s % 4000))),
        router->SetRouteOp(prefix(s), NextHopId(s + 1))};
  };
  auto release = [&](uint32_t s) {
    return std::vector<Op>{router->RemoveRouteOp(prefix(s)),
                           router->RemoveNextHopOp(NextHopId(s + 1))};
  };

  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  constexpr rcu::ReaderId kReader = 25;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> hits{0}, wrong{0};
  std::thread reader([&] {
    domain.Online(kReader);
    std::mt19937 rng(3);
    uint32_t dst[32];
    uint32_t session[32];
    const NextHop *resolved[32];
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 32; i++) {
        session[i] = rng() % kSessions;
        dst[i] = Ip(40, (session[i] >> 8) & 0xff, session[i] & 0xff, 7);
      }
      const uint64_t mask = router->ResolveBatch(dst, resolved);
      for (uint64_t m = mask; m != 0; m &= m - 1) {
        const int i = __builtin_ctzll(m);
        if (resolved[i]->egress.value() == session[i] % 4000) {
          hits++;
        } else {
          wrong++;
        }
      }
      domain.Quiescent(kReader);
    }
    domain.Offline(kReader);
  });
  uint64_t retries = 0;
  for (uint32_t step = 0; step < kSteps; step++) {
    // An id reused while its previous removal still waits for the reader is
    // refused: retry once the reader has moved on.
    while (Apply(establish(step % kSessions)).outcome != Outcome::kApplied) {
      retries++;
      engine_.ReclaimRetired();
    }
    if (step >= kLive) {
      ASSERT_EQ(Apply(release((step - kLive) % kSessions)).outcome,
                Outcome::kApplied);
    }
  }
  stop = true;
  reader.join();
  domain.Unregister(kReader);
  EXPECT_EQ(wrong.load(), 0u);
  EXPECT_GT(hits.load(), 0u);
  EXPECT_EQ(router->route_count(), kLive);
  std::printf("[router] %llu resolved, %llu retries\n",
              static_cast<unsigned long long>(hits.load()),
              static_cast<unsigned long long>(retries));
  Settle();
}

// Every allocation a mixed router transaction makes, failed in turn: the
// routes, next hops, lookups and ledger stay exactly as they were. A refusal
// the engine absorbs (std::stable_sort's nothrow buffer) leaves the
// transaction applied exactly as without it.
TEST_P(RouterTransactionTest, FailureAtEveryAllocationLeavesNoTrace) {
  struct Run {
    TransactionEngine engine{bess::runtime::runtime().rcu()};
    std::unique_ptr<Router> router;
    std::vector<Op> ops;
    std::string State() const {
      std::string s = std::to_string(router->route_count()) + "/" +
                      std::to_string(router->next_hop_count()) + "/g" +
                      std::to_string(engine.generation());
      for (uint32_t dst : {Ip(10, 0, 0, 1), Ip(10, 1, 0, 1), Ip(10, 1, 2, 200),
                           Ip(10, 9, 0, 1)}) {
        s += " " + std::to_string(EgressOf(*router, dst));
      }
      for (uint32_t id = 1; id <= 3; id++) {
        s += " r" + std::to_string(router->RouteReferences(NextHopId(id)));
      }
      return s;
    }
  };
  const auto prepare = [&](Run &run) {
    run.router = MakeRouter();
    Router &router = *run.router;
    ASSERT_TRUE(router.Enroll(run.engine.registry()));
    ASSERT_EQ(run.engine
                  .Apply(std::vector<Op>{
                      router.SetNextHopOp(NextHopId(1), Hop(1)),
                      router.SetNextHopOp(NextHopId(2), Hop(2)),
                      router.SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
                      router.SetRouteOp(P(Ip(10, 9, 0, 0), 16),
                                        NextHopId(2))})
                  .outcome,
              Outcome::kApplied);
    run.ops = {router.SetNextHopOp(NextHopId(3), Hop(3)),
               router.SetRouteOp(P(Ip(10, 1, 0, 0), 16), NextHopId(3)),
               router.SetRouteOp(P(Ip(10, 1, 2, 128), 25), NextHopId(3)),
               router.SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(2)),
               router.RemoveRouteOp(P(Ip(10, 9, 0, 0), 16)),
               router.SetNextHopOp(NextHopId(1), Hop(7))};
  };
  std::string applied;
  {
    Run reference;
    prepare(reference);
    ASSERT_EQ(reference.engine.Apply(reference.ops).outcome, Outcome::kApplied);
    applied = reference.State();
  }
  size_t threw_count = 0;
  const size_t points = fault_injection::ForEachFailurePoint([&](size_t k) {
    SCOPED_TRACE(::testing::Message() << "failing allocation " << k);
    Run run;
    prepare(run);
    ASSERT_FALSE(::testing::Test::HasFailure());
    const std::string before = run.State();
    bool threw = false;
    TransactionEngine::Result result;
    {
      const fault_injection::AllocationFaults window(k);
      try {
        result = run.engine.Apply(run.ops);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
    }
    if (!threw) {
      // No allocation was refused, or the refusal was absorbed.
      ASSERT_EQ(result.outcome, Outcome::kApplied);
      ASSERT_EQ(applied, run.State()) << "an absorbed refusal changed the outcome";
      return;
    }
    threw_count++;
    ASSERT_EQ(run.State(), before) << "a failed allocation left a trace";
    ASSERT_EQ(run.engine.Apply(run.ops).outcome, Outcome::kApplied);
    ASSERT_EQ(applied, run.State());
  });
  std::printf("[router] %zu failure points, %zu of them thrown\n", points, threw_count);
  EXPECT_GT(points, 10u);
}

// -- live (non-transactional) updates: domains, next hops, groups -----------------

// Bytes allocated from the DPDK heap (the FIBs live there).
size_t DpdkHeapBytes() {
  size_t total = 0;
  for (int socket = 0; socket < RTE_MAX_NUMA_NODES; socket++) {
    rte_malloc_socket_stats stats{};
    if (rte_malloc_get_socket_stats(socket, &stats) == 0) {
      total += stats.heap_allocsz_bytes;
    }
  }
  return total;
}

// What readers and the control side can observe of a router, read without
// changing it: counts, domains, every lookup of a fixed set of destinations
// (both group members), and the reference counts that gate removals.
std::string LiveState(const Router &r) {
  std::string s = std::to_string(r.route_count()) + "/" + std::to_string(r.next_hop_count()) +
                  "/" + std::to_string(r.next_hop_group_count()) + "/" +
                  std::to_string(r.domain_count());
  for (uint32_t d = 0; d < 4; d++) {
    const RouteDomainId domain(d);
    if (!r.HasDomain(domain)) {
      s += " d" + std::to_string(d) + "-";
      continue;
    }
    s += " d" + std::to_string(d) + ":" + std::to_string(r.route_count(domain));
    for (uint32_t dst : {Ip(10, 0, 0, 1), Ip(10, 1, 0, 1), Ip(10, 5, 0, 1), Ip(10, 9, 0, 1),
                         Ip(20, 0, 0, 1)}) {
      for (uint32_t hash : {0u, 1u, 2u}) {
        const NextHop *hop = r.Resolve(domain, dst, hash);
        s += " " + (hop == nullptr ? std::string("-") : std::to_string(hop->egress.value()));
      }
    }
  }
  for (uint32_t id = 1; id <= 6; id++) {
    s += " n" + std::to_string(r.RouteReferences(NextHopId(id))) + "," +
         std::to_string(r.GroupMemberships(NextHopId(id)));
  }
  for (uint32_t g = 1; g <= 3; g++) {
    const NextHopGroup *group = r.LookupNextHopGroup(NextHopGroupId(g));
    s += " g" + std::to_string(r.GroupReferences(NextHopGroupId(g))) + "," +
         (group == nullptr ? std::string("-")
                           : std::to_string(group->size) + "+" +
                                 std::to_string(group->standby_size) + ":" +
                                 std::to_string(group->eligible_size));
  }
  r.ForEachNextHopState([&](NextHopId id, const NextHopState &state) {
    s += " s" + std::to_string(id.value()) + (state.down ? "d" : "u");
  });
  return s;
}

class RouterLiveUpdateFaultTest : public bess::testing::RouteBackendTest {};

// A router with two extra domains, four next hops, two groups and routes in
// both domains, every retirement completed.
std::unique_ptr<Router> LiveRouter() {
  const Router::Config config{
      .ipv4 = LpmRouteTable::Config{.max_routes = 64,
                                    .tbl8_groups = 16,
                                    .backend = bess::testing::TestRouteBackend()}};
  auto made = Router::Create("live", config, 16, bess::runtime::runtime().rcu(), 4, 4);
  EXPECT_TRUE(made.has_value());
  Router &r = **made;
  for (uint32_t id = 1; id <= 4; id++) {
    EXPECT_TRUE(r.SetNextHop(NextHopId(id), Hop(id)));
  }
  EXPECT_TRUE(r.CreateDomain(RouteDomainId(1), config));
  EXPECT_TRUE(r.CreateDomain(RouteDomainId(3), config));
  const NextHopId g1[] = {NextHopId(1), NextHopId(2)};
  const NextHopId g3[] = {NextHopId(3)};
  EXPECT_TRUE(r.SetNextHopGroup(NextHopGroupId(1), g1));
  EXPECT_TRUE(r.SetNextHopGroup(NextHopGroupId(3), g3));
  EXPECT_TRUE(r.SetRoute(RouteDomainId(0), P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  EXPECT_TRUE(r.SetRoute(RouteDomainId(0), P(Ip(10, 9, 0, 0), 16), NextHopId(2)));
  EXPECT_TRUE(r.SetRoute(RouteDomainId(0), P(Ip(20, 0, 0, 0), 8), NextHopGroupId(1)));
  EXPECT_TRUE(r.SetRoute(RouteDomainId(1), P(Ip(10, 1, 0, 0), 16), NextHopId(3)));
  while (r.ReclaimRetired() != 0) {
    bess::runtime::runtime().rcu().Drain();
  }
  bess::runtime::runtime().rcu().Drain();
  return std::move(made).value();
}

// Each live update with each allocation it makes refused in turn: it fails
// (std::bad_alloc or an error), the router -- lookups, counts, references,
// domains, groups -- is exactly as it was, nothing is left in the DPDK heap,
// and the same update then succeeds and leaves the router as an undisturbed
// update would.
TEST_P(RouterLiveUpdateFaultTest, FailureAtEveryAllocationLeavesNoTrace) {
  const Router::Config config{
      .ipv4 = LpmRouteTable::Config{.max_routes = 64,
                                    .tbl8_groups = 16,
                                    .backend = bess::testing::TestRouteBackend()}};
  using Update = std::function<std::expected<void, RouteError>(Router &)>;
  const std::pair<const char *, Update> updates[] = {
      {"add a next hop", [](Router &r) { return r.SetNextHop(NextHopId(5), Hop(5)); }},
      {"replace a next hop", [](Router &r) { return r.SetNextHop(NextHopId(1), Hop(9)); }},
      {"remove a next hop", [](Router &r) { return r.RemoveNextHop(NextHopId(4)); }},
      {"add a group",
       [](Router &r) {
         const NextHopId m[] = {NextHopId(2), NextHopId(4)};
         return r.SetNextHopGroup(NextHopGroupId(2), m);
       }},
      {"replace a group",
       [](Router &r) {
         const NextHopId m[] = {NextHopId(2), NextHopId(3), NextHopId(3)};
         return r.SetNextHopGroup(NextHopGroupId(1), m);
       }},
      {"add a group with a standby",
       [](Router &r) {
         const NextHopId m[] = {NextHopId(2), NextHopId(4)};
         const NextHopId s[] = {NextHopId(3)};
         return r.SetNextHopGroup(NextHopGroupId(2), m, s);
       }},
      {"mark a group member down",
       [](Router &r) { return r.SetNextHopDown(NextHopId(1), true); }},
      {"mark a next hop in no group down",
       [](Router &r) { return r.SetNextHopDown(NextHopId(4), true); }},
      {"remove a group", [](Router &r) { return r.RemoveNextHopGroup(NextHopGroupId(3)); }},
      {"create a domain", [&](Router &r) { return r.CreateDomain(RouteDomainId(2), config); }},
      {"remove a domain", [](Router &r) { return r.RemoveDomain(RouteDomainId(3)); }},
      {"add a route",
       [](Router &r) { return r.SetRoute(RouteDomainId(1), P(Ip(10, 5, 0, 0), 16), NextHopId(2)); }},
      {"repoint a route at a group",
       [](Router &r) {
         return r.SetRoute(RouteDomainId(0), P(Ip(10, 9, 0, 0), 16), NextHopGroupId(3));
       }},
      {"remove a route",
       [](Router &r) { return r.RemoveRoute(RouteDomainId(0), P(Ip(10, 9, 0, 0), 16)); }},
      {"replace a domain's routes",
       [](Router &r) {
         return r.ReplaceRouteSetAtomic(
             RouteDomainId(1), RouteSet{{P(Ip(10, 1, 0, 0), 16), NextHopId(4)},
                                        {P(Ip(10, 5, 0, 0), 16), NextHopId(1)},
                                        {P(Ip(20, 0, 0, 0), 8), NextHopId(2)}});
       }},
  };
  for (const auto &[name, update] : updates) {
    SCOPED_TRACE(name);
    std::string expected;
    {
      auto reference = LiveRouter();
      ASSERT_TRUE(update(*reference));
      expected = LiveState(*reference);
    }
    const size_t points = fault_injection::ForEachFailurePoint([&](size_t k) {
      SCOPED_TRACE(::testing::Message() << "failing allocation " << k);
      auto router = LiveRouter();
      const std::string before = LiveState(*router);
      const size_t heap = DpdkHeapBytes();
      bool threw = false, injected = false;
      std::expected<void, RouteError> result;
      {
        const fault_injection::AllocationFaults window(k);
        try {
          result = update(*router);
        } catch (const std::bad_alloc &) {
          threw = true;
        }
        injected = window.injected();
      }
      if (!injected) {
        ASSERT_FALSE(threw);
        ASSERT_TRUE(result);
        ASSERT_EQ(expected, LiveState(*router));
        return;
      }
      ASSERT_TRUE(threw || !result) << "a refused allocation was swallowed";
      ASSERT_EQ(before, LiveState(*router)) << "a failed update left a trace";
      EXPECT_EQ(heap, DpdkHeapBytes()) << "a failed update left memory in the DPDK heap";
      ASSERT_TRUE(update(*router)) << "the update cannot be retried";
      ASSERT_EQ(expected, LiveState(*router));
    });
    // Some updates allocate nothing once their room is reserved up front
    // (removing a domain or a route, repointing one): zero points is a pass.
    std::printf("[router live] %s: %zu allocation sites failed in turn\n", name, points);
  }
}


// -- next-hop groups through transactions (user decision on D-065) ----------------

// The egress a destination resolves to with `hash`, -1 for a miss.
int EgressOfHashed(const Router &router, uint32_t dst, uint32_t hash) {
  const NextHop *hop = router.Resolve(kDefaultRouteDomainId, dst, hash);
  return hop == nullptr ? -1 : static_cast<int>(hop->egress.value());
}

// The set of egresses a group route spreads over, by sampling hashes.
std::set<int> EgressesOf(const Router &router, uint32_t dst) {
  std::set<int> seen;
  for (uint32_t h = 0; h < 4096; h++) {
    seen.insert(EgressOfHashed(router, dst, h * 0x9E3779B9u));
  }
  return seen;
}

TEST_P(RouterTransactionTest, GroupsNextHopsAndRoutesChangeInOneTransaction) {
  auto router = MakeRouter(64, 64, 1, /*groups=*/8);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  EXPECT_EQ(router->groups_resource(), "rt/groups");
  ASSERT_NE(router->groups_resource_object(), nullptr);
  const NextHopId abc[] = {NextHopId(1), NextHopId(2), NextHopId(3)};
  // Next hops, a group over them and a route to the group: one transaction.
  auto r = Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                  router->SetNextHopOp(NextHopId(2), Hop(2)),
                  router->SetNextHopOp(NextHopId(3), Hop(3)),
                  router->SetNextHopGroupOp(NextHopGroupId(1), abc),
                  router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                     NextHopGroupId(1))});
  ASSERT_EQ(r.outcome, Outcome::kApplied);
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{1, 2, 3}));
  EXPECT_EQ(router->GroupReferences(NextHopGroupId(1)), 1u);
  EXPECT_EQ(router->GroupMemberships(NextHopId(2)), 1u);
  EXPECT_EQ(router->RouteReferences(NextHopId(2)), 0u) << "a member is not a route";

  // Re-pointing the group's members and adding a next hop, atomically.
  const NextHopId cd[] = {NextHopId(3), NextHopId(4)};
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(4), Hop(4)),
                   router->SetNextHopGroupOp(NextHopGroupId(1), cd)})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{3, 4}));
  Settle();
  // Next hops no group or route names any more can go.
  ASSERT_EQ(Apply({router->RemoveNextHopOp(NextHopId(1)), router->RemoveNextHopOp(NextHopId(2))})
                .outcome,
            Outcome::kApplied);

  // The route moves to a plain next hop and the group goes, in one
  // transaction, while a reader is online and has not passed a quiescent
  // state: it may still hold a FIB value naming the group.
  rcu::RcuDomain &rcu = bess::runtime::runtime().rcu();
  const rcu::ReaderId reader = 62;
  ASSERT_TRUE(rcu.Register(reader).has_value());
  rcu.Online(reader);
  ASSERT_EQ(Apply({router->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(3)),
                   router->RemoveNextHopGroupOp(NextHopGroupId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressOf(*router, Ip(10, 1, 1, 1)), 3);
  // So the removed id stays retiring: re-adding it is refused.
  const auto again = Apply({router->SetNextHopGroupOp(NextHopGroupId(1), cd)});
  rcu.Offline(reader);
  rcu.Unregister(reader);
  ASSERT_NE(again.outcome, Outcome::kApplied);
  EXPECT_NE(again.ops[0].error.find("retiring"), std::string::npos) << again.ops[0].error;
  // Once the reader is gone the grace period ends and the id is free again.
  Settle();
  EXPECT_EQ(router->next_hop_group_count(), 0u);
  ASSERT_EQ(Apply({router->SetNextHopGroupOp(NextHopGroupId(1), cd)}).outcome, Outcome::kApplied);
}

TEST_P(RouterTransactionTest, GroupReferencesAreChecked) {
  auto router = MakeRouter(64, 64, 1, /*groups=*/4);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  const NextHopId ab[] = {NextHopId(1), NextHopId(2), NextHopId(1)};  // 1 weighted twice
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                   router->SetNextHopOp(NextHopId(2), Hop(2)),
                   router->SetNextHopGroupOp(NextHopGroupId(2), ab),
                   router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                      NextHopGroupId(2))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(router->GroupMemberships(NextHopId(1)), 1u) << "a weight is not a second membership";
  const auto gen = engine_.generation();
  const auto rejected = [&](std::vector<Op> ops, const char *why) {
    const auto res = Apply(std::move(ops));
    EXPECT_NE(res.outcome, Outcome::kApplied) << why;
    EXPECT_EQ(engine_.generation(), gen) << why << ": nothing applied";
    EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{1, 2})) << why;
  };
  rejected({router->RemoveNextHopGroupOp(NextHopGroupId(2))}, "a group a route names");
  rejected({router->RemoveNextHopOp(NextHopId(2))}, "a group's member");
  const NextHopId missing[] = {NextHopId(1), NextHopId(9)};
  rejected({router->SetNextHopGroupOp(NextHopGroupId(3), missing)}, "an unknown member");
  rejected({router->SetRouteOp(kDefaultRouteDomainId, P(Ip(20, 0, 0, 0), 8), NextHopGroupId(3))},
           "a route to an unknown group");
  rejected({router->SetNextHopGroupOp(NextHopGroupId(5), ab)}, "a group id past max_groups");
  // max_next_hops (64) + this id wraps to next hop 1's FIB value.
  rejected({router->SetRouteOp(kDefaultRouteDomainId, P(Ip(20, 0, 0, 0), 8),
                               NextHopGroupId(0xFFFFFFFFu - 62))},
           "a group id that wraps the FIB value");
  EXPECT_EQ(EgressOf(*router, Ip(20, 0, 0, 1)), -1);
  rejected({router->SetNextHopGroupOp(NextHopGroupId(3), std::span<const NextHopId>())},
           "an empty group");
  std::vector<NextHopId> many(NextHopGroup::kMaxMembers + 1, NextHopId(1));
  rejected({router->SetNextHopGroupOp(NextHopGroupId(3), many)}, "too many members");
  // A good op with a bad one: the whole transaction is refused.
  rejected({router->SetNextHopOp(NextHopId(3), Hop(3)),
            router->SetNextHopGroupOp(NextHopGroupId(3), missing)},
           "mixed transaction");
  EXPECT_EQ(EgressOf(*router, Ip(30, 0, 0, 1)), -1);

  // The direct setters belong to an unenrolled router.
  EXPECT_EQ(router->SetNextHopGroup(NextHopGroupId(3), ab).error(), RouteError::kEnrolled);
  EXPECT_EQ(router->RemoveNextHopGroup(NextHopGroupId(2)).error(), RouteError::kEnrolled);
  EXPECT_EQ(router->SetRoute(kDefaultRouteDomainId, P(Ip(20, 0, 0, 0), 8), NextHopGroupId(2))
                .error(),
            RouteError::kEnrolled);
}

// Without max_groups the groups resource is still registered -- a module
// class registers every table it declares (D-156) -- but it holds nothing and
// every id is invalid, so group ops and routes that name a group are refused
// and nothing resolves through one.
TEST_P(RouterTransactionTest, RoutersWithoutGroupsRegisterAnEmptyGroupsResource) {
  auto router = MakeRouter();  // max_groups 0
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  ASSERT_NE(router->groups_resource_object(), nullptr);
  EXPECT_EQ(router->next_hop_group_count(), 0u);
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1))}).outcome, Outcome::kApplied);
  const NextHopId one[] = {NextHopId(1)};
  const auto group = Apply({router->SetNextHopGroupOp(NextHopGroupId(1), one)});
  EXPECT_NE(group.outcome, Outcome::kApplied);
  EXPECT_NE(group.ops[0].error.find("configured capacity"), std::string::npos)
      << group.ops[0].error;
  const auto route = Apply({router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                               NextHopGroupId(1))});
  EXPECT_NE(route.outcome, Outcome::kApplied);
  EXPECT_NE(route.ops[0].error.find("configured capacity"), std::string::npos)
      << route.ops[0].error;
  EXPECT_EQ(EgressOf(*router, Ip(10, 0, 0, 1)), -1) << "nothing resolved through a group";
  EXPECT_EQ(Apply({router->SetNextHopStateOp(NextHopId(1), NextHopState{true})}).outcome,
            Outcome::kApplied)
      << "availability is per next hop: it needs no groups";
}

// A reader resolving through groups while transactions replace members, move
// routes between groups and next hops, and retire groups: every answer is one
// of the next hops some committed state allowed for that destination.
TEST_P(RouterTransactionTest, ReadersResolveThroughGroupsWhileTransactionsRun) {
  auto router = MakeRouter(64, 64, 1, /*groups=*/4);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  std::vector<Op> setup;
  for (uint32_t i = 1; i <= 6; i++) {
    setup.push_back(router->SetNextHopOp(NextHopId(i), Hop(i)));
  }
  const NextHopId g1[] = {NextHopId(1), NextHopId(2)};
  setup.push_back(router->SetNextHopGroupOp(NextHopGroupId(1), g1));
  setup.push_back(router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                     NextHopGroupId(1)));
  ASSERT_EQ(Apply(setup).outcome, Outcome::kApplied);

  rcu::RcuDomain &rcu = bess::runtime::runtime().rcu();
  const rcu::ReaderId reader_id = 61;
  ASSERT_TRUE(rcu.Register(reader_id).has_value());
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> bad{0}, reads{0};
  std::thread reader([&] {
    rcu.Online(reader_id);
    uint32_t h = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const int e = EgressOfHashed(*router, Ip(10, 2, 3, 4), h++ * 0x9E3779B9u);
      // Every next hop 1..6 is a legal answer at some point; a miss is not
      // (the route always names a group or a next hop that exists).
      if (e < 1 || e > 6) {
        bad++;
      }
      reads++;
      rcu.Quiescent(reader_id);
    }
    rcu.Offline(reader_id);
  });
  std::mt19937 rng(65);
  size_t applied = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  const auto hard = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  uint32_t current = 1;  // the group the route names; 0: a next hop
  while ((std::chrono::steady_clock::now() < deadline || applied < 200) &&
         std::chrono::steady_clock::now() < hard) {
    std::vector<Op> ops;
    const uint32_t other = current == 1 ? 2 : 1;
    std::vector<NextHopId> members;
    for (uint32_t m = 0, n = 1 + rng() % 4; m < n; m++) {
      members.push_back(NextHopId(1 + rng() % 6));
    }
    switch (rng() % 3) {
      case 0:  // new members for the current group
        if (current != 0) {
          ops.push_back(router->SetNextHopGroupOp(NextHopGroupId(current), members));
        }
        break;
      case 1:  // the route moves to the other group, the old group goes
        ops.push_back(router->SetNextHopGroupOp(NextHopGroupId(other), members));
        ops.push_back(router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                         NextHopGroupId(other)));
        if (current != 0) {
          ops.push_back(router->RemoveNextHopGroupOp(NextHopGroupId(current)));
        }
        current = other;
        break;
      default:  // the route moves to a plain next hop, the group goes
        ops.push_back(router->SetRouteOp(P(Ip(10, 0, 0, 0), 8), members[0]));
        if (current != 0) {
          ops.push_back(router->RemoveNextHopGroupOp(NextHopGroupId(current)));
        }
        current = 0;
        break;
    }
    if (ops.empty()) {
      continue;
    }
    const auto r = Apply(ops);
    if (r.outcome == Outcome::kApplied) {
      applied++;
    } else {
      // Only a group id still retiring may refuse: undo the bookkeeping.
      bool retiring = r.outcome == Outcome::kBusy;
      for (const auto &op : r.ops) {
        retiring |= op.error.find("retiring") != std::string::npos;
      }
      if (!retiring) {
        ADD_FAILURE() << "unexpected rejection";
        break;
      }
      current = router->GroupReferences(NextHopGroupId(1)) != 0   ? 1
                : router->GroupReferences(NextHopGroupId(2)) != 0 ? 2
                                                                  : 0;
      engine_.ReclaimRetired();
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
  stop = true;
  reader.join();
  rcu.Unregister(reader_id);
  EXPECT_EQ(bad.load(), 0u) << "of " << reads.load();
  EXPECT_GE(applied, 200u);
  EXPECT_GT(reads.load(), 0u);
}

TEST_P(RouterTransactionTest, GroupTransactionFailureAtEveryAllocationLeavesNoTrace) {
  struct Run {
    TransactionEngine engine{bess::runtime::runtime().rcu()};
    std::unique_ptr<Router> router;
    std::vector<Op> ops;
    std::string State() const {
      std::string s = std::to_string(router->route_count()) + "/" +
                      std::to_string(router->next_hop_count()) + "/" +
                      std::to_string(router->next_hop_group_count()) + "/g" +
                      std::to_string(engine.generation());
      for (uint32_t h = 0; h < 16; h++) {
        s += " " + std::to_string(EgressOfHashed(*router, Ip(10, 0, 0, 1), h * 0x9E3779B9u));
      }
      for (uint32_t g = 1; g <= 2; g++) {
        s += " g" + std::to_string(router->GroupReferences(NextHopGroupId(g)));
      }
      return s;
    }
  };
  std::vector<NextHopId> ab = {NextHopId(1), NextHopId(2)};
  std::vector<NextHopId> bc = {NextHopId(2), NextHopId(3)};
  const auto prepare = [&](Run &run, MakeRouterFn make) {
    run.router = make();
    Router &router = *run.router;
    ASSERT_TRUE(router.Enroll(run.engine.registry()));
    ASSERT_EQ(run.engine
                  .Apply(std::vector<Op>{router.SetNextHopOp(NextHopId(1), Hop(1)),
                                         router.SetNextHopOp(NextHopId(2), Hop(2)),
                                         router.SetNextHopGroupOp(NextHopGroupId(1), ab),
                                         router.SetRouteOp(kDefaultRouteDomainId,
                                                           P(Ip(10, 0, 0, 0), 8),
                                                           NextHopGroupId(1))})
                  .outcome,
              Outcome::kApplied);
    run.ops = {router.SetNextHopOp(NextHopId(3), Hop(3)),
               router.SetNextHopGroupOp(NextHopGroupId(2), bc),
               router.SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8), NextHopGroupId(2)),
               router.RemoveNextHopGroupOp(NextHopGroupId(1))};
  };
  const MakeRouterFn make = [&] { return MakeRouter(64, 64, 1, 4); };
  std::string applied;
  {
    Run reference;
    prepare(reference, make);
    ASSERT_EQ(reference.engine.Apply(reference.ops).outcome, Outcome::kApplied);
    applied = reference.State();
  }
  const size_t points = fault_injection::ForEachFailurePoint([&](size_t k) {
    SCOPED_TRACE(::testing::Message() << "failing allocation " << k);
    Run run;
    prepare(run, make);
    ASSERT_FALSE(::testing::Test::HasFailure());
    const std::string before = run.State();
    bool threw = false;
    TransactionEngine::Result result;
    {
      const fault_injection::AllocationFaults window(k);
      try {
        result = run.engine.Apply(run.ops);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
    }
    if (!threw) {
      ASSERT_EQ(result.outcome, Outcome::kApplied);
      ASSERT_EQ(applied, run.State());
      return;
    }
    ASSERT_EQ(run.State(), before) << "a failed allocation left a trace";
    ASSERT_EQ(run.engine.Apply(run.ops).outcome, Outcome::kApplied);
    ASSERT_EQ(applied, run.State());
  });
  std::printf("[router groups] %zu failure points\n", points);
  EXPECT_GT(points, 10u);
}

Ipv6Prefix P6(const char *text, uint8_t length) {
  return Ipv6Prefix::Make(bess::utils::ParseIpv6Address(text).value(), length)
      .value();
}

// The egress an IPv6 `dst` resolves to in `domain`, or -1.
int Egress6Of(const Router &router, RouteDomainId domain, const char *dst) {
  const NextHop *hop =
      router.Resolve(domain, bess::utils::ParseIpv6Address(dst).value(), 0);
  return hop == nullptr ? -1 : static_cast<int>(hop->egress.value());
}
int Egress6Of(const Router &router, const char *dst) {
  return Egress6Of(router, kDefaultRouteDomainId, dst);
}

// Several IPv6 operations on one table in one transaction compose: each edits
// the same unpublished spare, so all of them are visible after the commit,
// whatever mix of upserts and erases they are, and the footprint the stage
// declares is exact (the engine's Retirer is fatal on excess).
TEST_P(RouterTransactionTest, Ipv6OperationsInOneTransactionAllTakeEffect) {
  auto router = MakeRouter(64, 64, 1, 0, /*ipv6=*/true);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                   router->SetNextHopOp(NextHopId(2), Hop(2)),
                   router->SetRouteOp(kDefaultRouteDomainId,
                                      P6("2001:db8:1::", 48), NextHopId(1)),
                   router->SetRouteOp(kDefaultRouteDomainId,
                                      P6("2001:db8:2::", 48), NextHopId(2))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:1::1"), 1);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:2::1"), 2);
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 1u);
  EXPECT_EQ(router->RouteReferences(NextHopId(2)), 1u);

  // An erase, a re-point, a new prefix and the default together, in an order
  // that puts the erase first: the engine publishes erases after upserts, and
  // the table's single commit carries all of them.
  for (int round = 0; round < 3; round++) {
    SCOPED_TRACE(round);
    const bool even = round % 2 == 0;
    ASSERT_EQ(Apply({even ? router->RemoveRouteOp(kDefaultRouteDomainId,
                                                  P6("2001:db8:1::", 48))
                          : router->SetRouteOp(kDefaultRouteDomainId,
                                               P6("2001:db8:1::", 48), NextHopId(1)),
                     router->SetRouteOp(kDefaultRouteDomainId,
                                        P6("2001:db8:2::", 48),
                                        NextHopId(even ? 1 : 2)),
                     even ? router->SetRouteOp(kDefaultRouteDomainId,
                                               P6("2001:db8:3::", 48), NextHopId(2))
                          : router->RemoveRouteOp(kDefaultRouteDomainId,
                                                  P6("2001:db8:3::", 48)),
                     even ? router->SetRouteOp(kDefaultRouteDomainId, P6("::", 0),
                                               NextHopId(2))
                          : router->RemoveRouteOp(kDefaultRouteDomainId, P6("::", 0))})
                  .outcome,
              Outcome::kApplied);
    EXPECT_EQ(Egress6Of(*router, "2001:db8:1::1"), even ? 2 : 1);
    EXPECT_EQ(Egress6Of(*router, "2001:db8:2::1"), even ? 1 : 2);
    EXPECT_EQ(Egress6Of(*router, "2001:db8:3::1"), even ? 2 : -1);
    EXPECT_EQ(Egress6Of(*router, "3000::1"), even ? 2 : -1);
    EXPECT_EQ(router->route_count(), even ? 3u : 2u);
  }
  Settle();
}

// One transaction writes IPv6 routes into two domains -- one of them created
// by the write (D-158) -- and IPv4 routes beside them: each table's stage is
// committed once, every domain sees exactly its own routes, and the next
// transaction on the same tables works.
TEST_P(RouterTransactionTest, Ipv6OperationsAcrossDomainsInOneTransaction) {
  auto router = MakeRouter(64, 64, /*domains=*/2, 0, /*ipv6=*/true);
  const RouteDomainId d0 = kDefaultRouteDomainId;
  const RouteDomainId d1(1);
  ASSERT_TRUE(router->RemoveDomain(d1));  // so the transaction creates it
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                   router->SetNextHopOp(NextHopId(2), Hop(2)),
                   router->SetRouteOp(d0, P6("2001:db8:1::", 48), NextHopId(1)),
                   router->SetRouteOp(d1, P6("2001:db8:1::", 48), NextHopId(2)),
                   router->SetRouteOp(d0, P6("2001:db8:2::", 48), NextHopId(2)),
                   router->SetRouteOp(d1, P6("2001:db8::", 32), NextHopId(1)),
                   router->SetRouteOp(d0, P(Ip(10, 0, 0, 0), 8), NextHopId(1)),
                   router->SetRouteOp(d1, P(Ip(10, 0, 0, 0), 8), NextHopId(2))})
                .outcome,
            Outcome::kApplied);
  EXPECT_TRUE(router->HasDomain(d1));
  EXPECT_EQ(Egress6Of(*router, d0, "2001:db8:1::1"), 1);
  EXPECT_EQ(Egress6Of(*router, d1, "2001:db8:1::1"), 2);
  EXPECT_EQ(Egress6Of(*router, d0, "2001:db8:2::1"), 2);
  EXPECT_EQ(Egress6Of(*router, d1, "2001:db8:2::1"), 1) << "d1's /32";
  EXPECT_EQ(Egress6Of(*router, d0, "2001:db8:9::1"), -1);
  EXPECT_EQ(router->Resolve(d1, Ip(10, 1, 1, 1))->egress.value(), 2);
  EXPECT_EQ(router->route_count(), 6u);
  EXPECT_EQ(router->route_count(d1), 3u);

  ASSERT_EQ(Apply({router->RemoveRouteOp(d1, P6("2001:db8:1::", 48)),
                   router->SetRouteOp(d0, P6("2001:db8:1::", 48), NextHopId(2)),
                   router->RemoveRouteOp(d0, P6("2001:db8:2::", 48)),
                   router->SetRouteOp(d1, P6("2001:db8:3::", 48), NextHopId(2))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(Egress6Of(*router, d0, "2001:db8:1::1"), 2);
  EXPECT_EQ(Egress6Of(*router, d1, "2001:db8:1::1"), 1) << "back to d1's /32";
  EXPECT_EQ(Egress6Of(*router, d0, "2001:db8:2::1"), -1);
  EXPECT_EQ(Egress6Of(*router, d1, "2001:db8:3::1"), 2);
  EXPECT_EQ(router->RouteReferences(NextHopId(1)), 2u);  // d0 10/8, d1 /32
  EXPECT_EQ(router->RouteReferences(NextHopId(2)), 3u);
  Settle();
}

// A transaction refused after its IPv6 operations were staged leaves every
// table as it was: readers never saw the spares, and each stage is aborted
// once -- whether the refusal is a later operation of the same table or of
// another resource.
TEST_P(RouterTransactionTest, AnAbortedIpv6TransactionLeavesTheTableUnchanged) {
  auto router = MakeRouter(64, 64, 1, 0, /*ipv6=*/true);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                   router->SetRouteOp(kDefaultRouteDomainId,
                                      P6("2001:db8:1::", 48), NextHopId(1))})
                .outcome,
            Outcome::kApplied);
  const auto generation = engine_.generation();
  auto unchanged = [&] {
    EXPECT_EQ(engine_.generation(), generation);
    EXPECT_EQ(Egress6Of(*router, "2001:db8:1::1"), 1);
    EXPECT_EQ(Egress6Of(*router, "2001:db8:2::1"), -1);
    EXPECT_EQ(Egress6Of(*router, "2001:db8:3::1"), -1);
    EXPECT_EQ(router->route_count(), 1u);
    EXPECT_EQ(router->RouteReferences(NextHopId(1)), 1u);
  };
  // The last operation names a next hop that does not exist.
  auto r = Apply({router->SetRouteOp(kDefaultRouteDomainId,
                                     P6("2001:db8:2::", 48), NextHopId(1)),
                  router->RemoveRouteOp(kDefaultRouteDomainId,
                                        P6("2001:db8:1::", 48)),
                  router->SetRouteOp(kDefaultRouteDomainId,
                                     P6("2001:db8:3::", 48), NextHopId(9))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  unchanged();
  // An operation of another resource is refused after the routes.
  r = Apply({router->SetRouteOp(kDefaultRouteDomainId, P6("2001:db8:2::", 48),
                                NextHopId(1)),
             router->RemoveNextHopOp(NextHopId(7))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  unchanged();

  // And the same operations without the bad one still apply.
  ASSERT_EQ(Apply({router->SetRouteOp(kDefaultRouteDomainId,
                                      P6("2001:db8:2::", 48), NextHopId(1)),
                   router->RemoveRouteOp(kDefaultRouteDomainId,
                                         P6("2001:db8:1::", 48))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:1::1"), -1);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:2::1"), 1);
  Settle();
}

// A domain's IPv6 routes are bounded by max_routes, as its IPv4 rules are: a
// transaction that would pass it is refused whole, including when the
// refused edit is the one that began the table's stage.
TEST_P(RouterTransactionTest, Ipv6RoutesAreBoundedByMaxRoutes) {
  const Router::Config config{
      .ipv6 = Ipv6RouteTable::Config{.max_routes = 2, .tbl8_groups = 64}};
  auto made = Router::Create("rt", config, 8, bess::runtime::runtime().rcu());
  ASSERT_TRUE(made);
  auto router = std::move(*made);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1))}).outcome,
            Outcome::kApplied);
  auto r = Apply({router->SetRouteOp(kDefaultRouteDomainId,
                                     P6("2001:db8:1::", 48), NextHopId(1)),
                  router->SetRouteOp(kDefaultRouteDomainId,
                                     P6("2001:db8:2::", 48), NextHopId(1)),
                  router->SetRouteOp(kDefaultRouteDomainId,
                                     P6("2001:db8:3::", 48), NextHopId(1))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[2].status, TransactionEngine::OpStatus::kFailed);
  EXPECT_EQ(router->route_count(), 0u);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:1::1"), -1);
  ASSERT_EQ(Apply({router->SetRouteOp(kDefaultRouteDomainId,
                                      P6("2001:db8:1::", 48), NextHopId(1)),
                   router->SetRouteOp(kDefaultRouteDomainId,
                                      P6("2001:db8:2::", 48), NextHopId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(router->route_count(), 2u);
  // Full: the first (and only) IPv6 edit of the next transaction is refused.
  r = Apply({router->SetRouteOp(kDefaultRouteDomainId, P6("2001:db8:3::", 48),
                                NextHopId(1))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:3::1"), -1);
  ASSERT_EQ(Apply({router->RemoveRouteOp(kDefaultRouteDomainId,
                                         P6("2001:db8:2::", 48)),
                   router->SetRouteOp(kDefaultRouteDomainId,
                                      P6("2001:db8:3::", 48), NextHopId(1))})
                .outcome,
            Outcome::kApplied)
      << "an erase makes room within the same transaction";
  EXPECT_EQ(Egress6Of(*router, "2001:db8:3::1"), 1);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:2::1"), -1);
  Settle();
}

std::optional<fault_injection::AllocationFaults> g_ipv6_publish_window;
size_t g_ipv6_publish_allocations = 0;

void CountIpv6PublishAllocations(bool entering) {
  if (entering) {
    g_ipv6_publish_window.emplace();
  } else if (g_ipv6_publish_window) {
    g_ipv6_publish_allocations += g_ipv6_publish_window->allocations();
    g_ipv6_publish_window.reset();
  }
}

// The StagedOp contract (resource.h): an IPv6 route's publication allocates
// nothing -- the spare was edited in Reserve(), Commit() is a pointer
// exchange, and the rules' nodes were made in the stage. Multi-operation
// transactions on two domains' tables, and IPv4 beside them.
TEST_P(RouterTransactionTest, Ipv6PublicationDoesNotAllocate) {
  auto router = MakeRouter(64, 64, /*domains=*/2, 0, /*ipv6=*/true);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  const RouteDomainId d1(1);
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                   router->SetNextHopOp(NextHopId(2), Hop(2))})
                .outcome,
            Outcome::kApplied);
  g_ipv6_publish_allocations = 0;
  dataplane::internal::g_publish_window_hook = CountIpv6PublishAllocations;
  const auto added = Apply({router->SetRouteOp(kDefaultRouteDomainId,
                                               P6("2001:db8:1::", 48), NextHopId(1)),
                            router->SetRouteOp(kDefaultRouteDomainId,
                                               P6("2001:db8:2::", 48), NextHopId(2)),
                            router->SetRouteOp(d1, P6("2001:db8:1::", 48), NextHopId(2)),
                            router->SetRouteOp(d1, P6("::", 0), NextHopId(1)),
                            router->SetRouteOp(P(Ip(10, 0, 0, 0), 8), NextHopId(1))});
  const auto changed = Apply({router->SetRouteOp(kDefaultRouteDomainId,
                                                 P6("2001:db8:1::", 48), NextHopId(2)),
                              router->RemoveRouteOp(kDefaultRouteDomainId,
                                                    P6("2001:db8:2::", 48)),
                              router->SetRouteOp(kDefaultRouteDomainId,
                                                 P6("2001:db8:3::", 48), NextHopId(1)),
                              router->RemoveRouteOp(d1, P6("::", 0))});
  dataplane::internal::g_publish_window_hook = nullptr;
  ASSERT_EQ(added.outcome, Outcome::kApplied);
  ASSERT_EQ(changed.outcome, Outcome::kApplied);
  EXPECT_EQ(g_ipv6_publish_allocations, 0u) << "the publication phase allocated";
  EXPECT_EQ(Egress6Of(*router, "2001:db8:1::1"), 2);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:2::1"), -1);
  EXPECT_EQ(Egress6Of(*router, "2001:db8:3::1"), 1);
  EXPECT_EQ(Egress6Of(*router, d1, "2001:db8:1::1"), 2);
  EXPECT_EQ(Egress6Of(*router, d1, "3000::1"), -1);
  Settle();
}

// -- address families ----------------------------------------------------------

// IPv4-only, IPv6-only and dual-stack routers: a family is served iff its
// config is set, and a disabled family has no FIB in any domain -- the
// default one, one CreateDomain() makes, or one a transaction creates -- its
// writes are refused (kFamilyDisabled) directly and through the engine, and
// its readers miss.
class RouterFamilyTest : public ::testing::Test {
 protected:
  static constexpr size_t kDomains = 3;

  static Router::Config Families(bool ipv4, bool ipv6) {
    Router::Config config;
    if (ipv4) {
      config.ipv4 = LpmRouteTable::Config{.max_routes = 64, .tbl8_groups = 16};
    }
    if (ipv6) {
      config.ipv6 = Ipv6RouteTable::Config{.max_routes = 64, .tbl8_groups = 64};
    }
    return config;
  }

  void TearDown() override { bess::runtime::runtime().rcu().Drain(); }

  // Creates a router with the families and checks every rule above.
  void Check(bool ipv4, bool ipv6) {
    SCOPED_TRACE(std::string(ipv4 ? "ipv4 " : "") + (ipv6 ? "ipv6" : ""));
    // Outlives the router enrolled in it (declared first).
    TransactionEngine engine{bess::runtime::runtime().rcu()};
    const Router::Config config = Families(ipv4, ipv6);
    auto made = Router::Create("fam", config, 8, bess::runtime::runtime().rcu(), kDomains);
    ASSERT_TRUE(made) << RouteErrorName(made.error());
    Router &router = **made;
    EXPECT_EQ(router.has_ipv4(), ipv4);
    EXPECT_EQ(router.has_ipv6(), ipv6);
    auto expect_fibs = [&](size_t domains) {
      const Router::FibCounts counts = router.fib_counts();
      EXPECT_EQ(counts.ipv4, ipv4 ? domains : 0u);
      EXPECT_EQ(counts.ipv6, ipv6 ? domains : 0u);
    };
    expect_fibs(1);

    // A domain must name exactly the router's families.
    EXPECT_EQ(RouteError::kFamilyDisabled,
              router.CreateDomain(RouteDomainId(1), Families(!ipv4, ipv6)).error());
    EXPECT_EQ(RouteError::kFamilyDisabled,
              router.CreateDomain(RouteDomainId(1), Families(ipv4, !ipv6)).error());
    EXPECT_EQ(RouteError::kFamilyDisabled, router.CreateDomain(RouteDomainId(1), {}).error());
    EXPECT_FALSE(router.HasDomain(RouteDomainId(1)));
    ASSERT_TRUE(router.CreateDomain(RouteDomainId(1), config));
    expect_fibs(2);

    // Direct writes.
    ASSERT_TRUE(router.SetNextHop(NextHopId(1), Hop(1)));
    const auto v4 = router.SetRoute(RouteDomainId(1), P(Ip(10, 0, 0, 0), 8), NextHopId(1));
    const auto v6 = router.SetRoute(RouteDomainId(1), P6("2001:db8::", 32), NextHopId(1));
    EXPECT_EQ(v4.has_value(), ipv4);
    EXPECT_EQ(v6.has_value(), ipv6);
    if (!ipv4) {
      EXPECT_EQ(RouteError::kFamilyDisabled, v4.error());
      EXPECT_EQ(RouteError::kFamilyDisabled,
                router.RemoveRoute(RouteDomainId(1), P(Ip(10, 0, 0, 0), 8)).error());
      EXPECT_EQ(RouteError::kFamilyDisabled,
                router.ReplaceRouteSetAtomic(RouteDomainId(1), {}).error());
    }
    if (!ipv6) {
      EXPECT_EQ(RouteError::kFamilyDisabled, v6.error());
      EXPECT_EQ(RouteError::kFamilyDisabled,
                router.RemoveRoute(RouteDomainId(1), P6("2001:db8::", 32)).error());
    }
    // Readers: the served family resolves, the other misses (no table to
    // read, not a crash), on every overload.
    const auto v6_dst = bess::utils::ParseIpv6Address("2001:db8::1").value();
    EXPECT_EQ(router.Resolve(RouteDomainId(1), Ip(10, 1, 2, 3)) != nullptr, ipv4);
    EXPECT_EQ(router.LookupRoute(RouteDomainId(1), Ip(10, 1, 2, 3)).value(), ipv4 ? 1u : 0u);
    EXPECT_EQ(router.Resolve(RouteDomainId(1), v6_dst) != nullptr, ipv6);
    EXPECT_EQ(router.Resolve(Ip(10, 1, 2, 3)), nullptr) << "nothing in the default domain";
    const uint32_t dst4[] = {Ip(10, 1, 2, 3)};
    const uint32_t hashes[] = {0};
    const NextHop *hops[1] = {};
    EXPECT_EQ(router.ResolveBatch(RouteDomainId(1), dst4, hashes, hops), ipv4 ? 1u : 0u);
    EXPECT_EQ(router.ResolveBatch(dst4, hops), 0u);
    const bess::utils::Ipv6Address dst6[] = {v6_dst};
    EXPECT_EQ(router.ResolveBatch6(RouteDomainId(1), dst6, hashes, hops), ipv6 ? 1u : 0u);
    EXPECT_EQ(router.route_count(), size_t{ipv4} + size_t{ipv6});
    if (ipv4) {
      ASSERT_TRUE(router.RemoveRoute(RouteDomainId(1), P(Ip(10, 0, 0, 0), 8)));
    }
    if (ipv6) {
      ASSERT_TRUE(router.RemoveRoute(RouteDomainId(1), P6("2001:db8::", 32)));
    }

    // Transactions: a disabled family's key is refused at Reserve; a served
    // one creates its domain with the router's families only.
    ASSERT_TRUE(router.Enroll(engine.registry()));
    const RouteDomainId d2(2);
    for (const bool family6 : {false, true}) {
      const Op op = family6 ? router.SetRouteOp(d2, P6("2001:db8::", 32), NextHopId(1))
                            : router.SetRouteOp(d2, P(Ip(10, 0, 0, 0), 8), NextHopId(1));
      const auto r = engine.Apply(std::span(&op, 1));
      const bool served = family6 ? ipv6 : ipv4;
      ASSERT_EQ(r.outcome, served ? Outcome::kApplied : Outcome::kRejected);
      if (!served) {
        EXPECT_NE(r.ops[0].error.find(RouteErrorName(RouteError::kFamilyDisabled)),
                  std::string::npos)
            << r.ops[0].error;
      }
    }
    EXPECT_TRUE(router.HasDomain(d2)) << "created by the served family's write";
    expect_fibs(3);
    std::vector<Op> erase;
    if (ipv4) {
      erase.push_back(router.RemoveRouteOp(d2, P(Ip(10, 0, 0, 0), 8)));
    }
    if (ipv6) {
      erase.push_back(router.RemoveRouteOp(d2, P6("2001:db8::", 32)));
    }
    ASSERT_EQ(engine.Apply(erase).outcome, Outcome::kApplied);
    while (engine.ReclaimRetired() != 0) {
    }
  }
};

TEST_F(RouterFamilyTest, Ipv4Only) { Check(true, false); }
TEST_F(RouterFamilyTest, Ipv6Only) { Check(false, true); }
TEST_F(RouterFamilyTest, DualStack) { Check(true, true); }

// A router with neither family is a next-hop-only router: no FIB in any
// domain, next hops resolved by id, and every route write refused.
TEST_F(RouterFamilyTest, ARouterWithNoFamilyHoldsNextHopsAndNoFib) {
  TransactionEngine engine{bess::runtime::runtime().rcu()};
  auto made = Router::Create("none", {}, 8, bess::runtime::runtime().rcu());
  ASSERT_TRUE(made) << RouteErrorName(made.error());
  Router &router = **made;
  EXPECT_FALSE(router.has_ipv4());
  EXPECT_FALSE(router.has_ipv6());
  EXPECT_EQ(0u, router.fib_counts().ipv4);
  EXPECT_EQ(0u, router.fib_counts().ipv6);
  ASSERT_TRUE(router.Enroll(engine.registry()));
  const Op hop = router.SetNextHopOp(NextHopId(1), Hop(1));
  ASSERT_EQ(engine.Apply(std::span(&hop, 1)).outcome, Outcome::kApplied);
  EXPECT_NE(nullptr, router.LookupNextHop(NextHopId(1)));
  for (const Op &route : {router.SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                            NextHopId(1)),
                          router.SetRouteOp(kDefaultRouteDomainId, P6("2001:db8::", 32),
                                            NextHopId(1))}) {
    const auto r = engine.Apply(std::span(&route, 1));
    ASSERT_EQ(r.outcome, Outcome::kRejected);
    EXPECT_NE(r.ops[0].error.find("no FIB"), std::string::npos) << r.ops[0].error;
  }
  EXPECT_EQ(0u, router.fib_counts().ipv4 + router.fib_counts().ipv6);
  EXPECT_EQ(router.Resolve(Ip(10, 1, 2, 3)), nullptr);
  const Op erase = router.RemoveNextHopOp(NextHopId(1));
  ASSERT_EQ(engine.Apply(std::span(&erase, 1)).outcome, Outcome::kApplied);
  while (engine.ReclaimRetired() != 0) {
  }
}

// The router sizes its IPv6 entries from its own capacity: a small router's
// largest route value (a group's) works whatever max_value the caller set.
TEST_F(RouterFamilyTest, Ipv6EntriesFollowTheRouterCapacity) {
  Router::Config config = Families(false, true);
  config.ipv6->max_value = 1;  // overridden by the router: 8 + 4
  auto made = Router::Create("fam6", config, 8, bess::runtime::runtime().rcu(), 1, 4);
  ASSERT_TRUE(made);
  Router &router = **made;
  ASSERT_TRUE(router.SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE(router.SetNextHop(NextHopId(2), Hop(2)));
  const NextHopId members[] = {NextHopId(2)};
  ASSERT_TRUE(router.SetNextHopGroup(NextHopGroupId(4), members));
  ASSERT_TRUE(router.SetRoute(kDefaultRouteDomainId, P6("2001:db8::", 32), NextHopGroupId(4)));
  EXPECT_EQ(Egress6Of(router, "2001:db8::1"), 2) << "the largest value, 8 + 4, fits";
  ASSERT_TRUE(router.RemoveRoute(kDefaultRouteDomainId, P6("2001:db8::", 32)));
}

// -- path health through transactions (D-184) ---------------------------------

// The next hops with a state entry, and whether each says down.
std::vector<std::pair<uint32_t, bool>> StatesOf(const Router &router) {
  std::vector<std::pair<uint32_t, bool>> states;
  router.ForEachNextHopState(
      [&](NextHopId id, const NextHopState &state) { states.emplace_back(id.value(), state.down); });
  return states;
}

NextHopState Down(bool down = true) { return NextHopState{down}; }

// "<rt>/next_hop_state" is desired state that references its next hop: state
// for a next hop that does not exist is refused, a next hop with an entry
// cannot leave alone, and an entry reads back as written -- one that says
// available too. A change of availability fails the group over.
TEST_P(RouterTransactionTest, NextHopStateIsAResourceThatReferencesItsNextHop) {
  auto router = MakeRouter(64, 64, 1, /*groups=*/4);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  EXPECT_EQ(router->next_hop_state_resource(), "rt/next_hop_state");
  ASSERT_NE(router->next_hop_state_resource_object(), nullptr);
  const NextHopId ab[] = {NextHopId(1), NextHopId(2)};
  const NextHopId c[] = {NextHopId(3)};
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                   router->SetNextHopOp(NextHopId(2), Hop(2)),
                   router->SetNextHopOp(NextHopId(3), Hop(3)),
                   router->SetNextHopGroupOp(NextHopGroupId(1), ab, c),
                   router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                      NextHopGroupId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(router->GroupMemberships(NextHopId(3)), 1u) << "a standby is a member";
  const auto rejected = [&](std::vector<Op> ops, const std::string &error, const char *why) {
    const auto gen = engine_.generation();
    const auto res = Apply(std::move(ops));
    EXPECT_NE(res.outcome, Outcome::kApplied) << why;
    EXPECT_EQ(engine_.generation(), gen) << why << ": nothing applied";
    bool named = false;
    for (const auto &op : res.ops) {
      named |= op.error.find(error) != std::string::npos;
    }
    EXPECT_TRUE(named) << why << ": want '" << error << "'";
    EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{1, 2})) << why;
  };
  rejected({router->SetNextHopStateOp(NextHopId(9), Down())}, "references missing",
           "state for a next hop that does not exist");
  rejected({router->SetNextHopStateOp(NextHopId(0), Down())}, "configured capacity",
           "next hop id 0");
  rejected({router->SetNextHopStateOp(NextHopId(65), Down())}, "configured capacity",
           "an id past max_next_hops");
  rejected({router->RemoveNextHopStateOp(NextHopId(1))}, "not found", "no entry to erase");
  rejected({Op::Upsert(router->next_hop_state_resource(), dataplane::EncodeKey(NextHopId(1)),
                       std::any(true))},
           "wrong value type", "a value that is not a NextHopState");
  const NextHopId both[] = {NextHopId(3)};
  rejected({router->SetNextHopGroupOp(NextHopGroupId(2), both, both)},
           "both an active and a standby", "a next hop in both lists");

  // A next hop and its state arrive together; the hop cannot leave without
  // its entry, and both leave together.
  ASSERT_EQ(Apply({router->SetNextHopStateOp(NextHopId(4), Down()),
                   router->SetNextHopOp(NextHopId(4), Hop(4))})
                .outcome,
            Outcome::kApplied);
  rejected({router->RemoveNextHopOp(NextHopId(4))}, "still referenced",
           "a next hop with a state entry");
  ASSERT_EQ(Apply({router->RemoveNextHopOp(NextHopId(4)), router->RemoveNextHopStateOp(NextHopId(4))})
                .outcome,
            Outcome::kApplied);
  EXPECT_TRUE(StatesOf(*router).empty());

  ASSERT_EQ(Apply({router->SetNextHopStateOp(NextHopId(1), Down()),
                   router->SetNextHopStateOp(NextHopId(2), Down(false))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(StatesOf(*router), (std::vector<std::pair<uint32_t, bool>>{{1, true}, {2, false}}));
  EXPECT_EQ(router->next_hop_state_resource_object()->LiveCount(), 2u);
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{2}));
  ASSERT_EQ(Apply({router->SetNextHopStateOp(NextHopId(2), Down())}).outcome, Outcome::kApplied);
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{3})) << "the standby";
  ASSERT_EQ(Apply({router->RemoveNextHopStateOp(NextHopId(1)),
                   router->RemoveNextHopStateOp(NextHopId(2))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{1, 2})) << "no entries";
  EXPECT_EQ(router->SetNextHopDown(NextHopId(1), true).error(), RouteError::kEnrolled);

  // State set directly cannot be carried into a ledger: enroll first.
  TransactionEngine other{bess::runtime::runtime().rcu()};
  const Router::Config config{.ipv4 = bess::testing::TestRouteConfig()};
  auto direct = Router::Create("rt_direct", config, 8, bess::runtime::runtime().rcu());
  ASSERT_TRUE(direct.has_value());
  ASSERT_TRUE((*direct)->SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE((*direct)->SetNextHopDown(NextHopId(1), true));
  const auto enrolled = (*direct)->Enroll(other.registry());
  ASSERT_FALSE(enrolled);
  EXPECT_NE(enrolled.error().find("marking next hops down"), std::string::npos);
  ASSERT_TRUE((*direct)->SetNextHopDown(NextHopId(1), false));
  EXPECT_TRUE((*direct)->Enroll(other.registry()));
}

// One transaction that changes groups and member state together publishes
// every group from the staged state of both, whatever the request order: a
// group it writes computes its eligible set with the transaction's state
// changes, a group it does not write is republished for them, and a group it
// erases is never republished (its id is retiring, and publishing over it is
// fatal).
TEST_P(RouterTransactionTest, GroupsAndMemberStateChangeInOneTransaction) {
  for (const bool state_first : {true, false}) {
    SCOPED_TRACE(state_first ? "state ops first" : "group ops first");
    auto router = MakeRouter(64, 64, 1, /*groups=*/4);
    ASSERT_TRUE(router->Enroll(engine_.registry()));
    std::vector<Op> setup;
    for (uint32_t i = 1; i <= 6; i++) {
      setup.push_back(router->SetNextHopOp(NextHopId(i), Hop(i)));
    }
    const NextHopId g1[] = {NextHopId(1), NextHopId(2)};
    const NextHopId g1_standby[] = {NextHopId(3)};
    const NextHopId g2[] = {NextHopId(2), NextHopId(4)};
    const NextHopId g3[] = {NextHopId(2)};
    const NextHopId g3_standby[] = {NextHopId(5)};
    setup.push_back(router->SetNextHopGroupOp(NextHopGroupId(1), g1, g1_standby));
    setup.push_back(router->SetNextHopGroupOp(NextHopGroupId(2), g2));
    setup.push_back(router->SetNextHopGroupOp(NextHopGroupId(3), g3, g3_standby));
    setup.push_back(
        router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8), NextHopGroupId(1)));
    setup.push_back(
        router->SetRouteOp(kDefaultRouteDomainId, P(Ip(20, 0, 0, 0), 8), NextHopGroupId(2)));
    setup.push_back(
        router->SetRouteOp(kDefaultRouteDomainId, P(Ip(30, 0, 0, 0), 8), NextHopGroupId(3)));
    ASSERT_EQ(Apply(setup).outcome, Outcome::kApplied);
    const auto apply = [&](std::vector<Op> state, std::vector<Op> groups) {
      std::vector<Op> ops = state_first ? state : groups;
      for (Op &op : state_first ? groups : state) {
        ops.push_back(std::move(op));
      }
      return Apply(std::move(ops)).outcome;
    };

    // Next hop 2 goes down while group 1 gains next hop 6 and group 3 goes.
    const NextHopId g1_wider[] = {NextHopId(1), NextHopId(2), NextHopId(6)};
    ASSERT_EQ(apply({router->SetNextHopStateOp(NextHopId(2), Down())},
                    {router->SetNextHopGroupOp(NextHopGroupId(1), g1_wider, g1_standby),
                     router->SetRouteOp(P(Ip(30, 0, 0, 0), 8), NextHopId(5)),
                     router->RemoveNextHopGroupOp(NextHopGroupId(3))}),
              Outcome::kApplied);
    EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{1, 6}))
        << "the written group is computed with the staged state";
    EXPECT_EQ(EgressesOf(*router, Ip(20, 1, 1, 1)), (std::set<int>{4}))
        << "the group not written is republished for the state change";
    EXPECT_EQ(EgressOf(*router, Ip(30, 1, 1, 1)), 5);
    Settle();

    // Next hop 2 comes back (its entry is erased), 1 and 6 go down, group 2
    // goes and group 4 arrives with its actives down.
    const NextHopId g4[] = {NextHopId(1), NextHopId(6)};
    const NextHopId g4_standby[] = {NextHopId(2)};
    ASSERT_EQ(apply({router->RemoveNextHopStateOp(NextHopId(2)),
                     router->SetNextHopStateOp(NextHopId(1), Down()),
                     router->SetNextHopStateOp(NextHopId(6), Down())},
                    {router->SetRouteOp(P(Ip(20, 0, 0, 0), 8), NextHopId(4)),
                     router->RemoveNextHopGroupOp(NextHopGroupId(2)),
                     router->SetNextHopGroupOp(NextHopGroupId(4), g4, g4_standby),
                     router->SetRouteOp(kDefaultRouteDomainId, P(Ip(40, 0, 0, 0), 8),
                                        NextHopGroupId(4))}),
              Outcome::kApplied);
    EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{2}));
    EXPECT_EQ(EgressOf(*router, Ip(20, 1, 1, 1)), 4);
    EXPECT_EQ(EgressesOf(*router, Ip(40, 1, 1, 1)), (std::set<int>{2})) << "the new group's standby";
    EXPECT_EQ(StatesOf(*router), (std::vector<std::pair<uint32_t, bool>>{{1, true}, {6, true}}));
    Settle();
  }
}

// Publishing state changes, the group republications they cause, and group
// changes beside them allocates nothing: every object and every reverse index
// slot was made before.
std::optional<fault_injection::AllocationFaults> g_publish_window;
size_t g_publish_allocations = 0;

TEST_P(RouterTransactionTest, PathHealthPublicationDoesNotAllocate) {
  auto router = MakeRouter(64, 64, 1, /*groups=*/4);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  g_publish_allocations = 0;
  dataplane::internal::g_publish_window_hook = [](bool entering) {
    if (entering) {
      g_publish_window.emplace();
    } else if (g_publish_window) {
      g_publish_allocations += g_publish_window->allocations();
      g_publish_window.reset();
    }
  };
  const NextHopId ab[] = {NextHopId(1), NextHopId(2)};
  const NextHopId c[] = {NextHopId(3)};
  const NextHopId bd[] = {NextHopId(2), NextHopId(4)};
  std::vector<Op> setup;
  for (uint32_t i = 1; i <= 5; i++) {
    setup.push_back(router->SetNextHopOp(NextHopId(i), Hop(i)));
  }
  setup.push_back(router->SetNextHopStateOp(NextHopId(1), Down()));
  setup.push_back(router->SetNextHopGroupOp(NextHopGroupId(1), ab, c));
  setup.push_back(router->SetNextHopGroupOp(NextHopGroupId(2), bd));
  EXPECT_EQ(Apply(setup).outcome, Outcome::kApplied);
  // A state change republishing two groups; an entry for a hop in no group.
  EXPECT_EQ(Apply({router->SetNextHopStateOp(NextHopId(2), Down()),
                   router->SetNextHopStateOp(NextHopId(5), Down())})
                .outcome,
            Outcome::kApplied);
  // A group rewritten (members joining and leaving) with a state change.
  const NextHopId ae[] = {NextHopId(1), NextHopId(5)};
  EXPECT_EQ(Apply({router->SetNextHopGroupOp(NextHopGroupId(1), ae, c),
                   router->RemoveNextHopStateOp(NextHopId(1))})
                .outcome,
            Outcome::kApplied);
  // A group erased with its member's entry erased.
  EXPECT_EQ(Apply({router->RemoveNextHopGroupOp(NextHopGroupId(2)),
                   router->RemoveNextHopStateOp(NextHopId(2))})
                .outcome,
            Outcome::kApplied);
  dataplane::internal::g_publish_window_hook = nullptr;
  EXPECT_EQ(g_publish_allocations, 0u) << "the publication phase allocated";
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{-1})) << "no route yet";
  ASSERT_EQ(Apply({router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                      NextHopGroupId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{1})) << "5 is down";
}

// A reader resolving through active/standby groups while transactions flip
// member state, sometimes with a group rewrite in the same transaction: every
// answer is a configured member, and every group object it sees is whole --
// its eligible set is drawn from one tier of its own configuration.
TEST_P(RouterTransactionTest, ReadersResolveThroughGroupsWhileStateFlips) {
  auto router = MakeRouter(64, 64, 1, /*groups=*/4);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  std::vector<Op> setup;
  for (uint32_t i = 1; i <= 7; i++) {
    setup.push_back(router->SetNextHopOp(NextHopId(i), Hop(i)));
  }
  const NextHopId g1[] = {NextHopId(1), NextHopId(2), NextHopId(3)};
  const NextHopId g1_standby[] = {NextHopId(4)};
  const NextHopId g2[] = {NextHopId(3), NextHopId(5)};
  const NextHopId g2_standby[] = {NextHopId(6)};
  setup.push_back(router->SetNextHopGroupOp(NextHopGroupId(1), g1, g1_standby));
  setup.push_back(router->SetNextHopGroupOp(NextHopGroupId(2), g2, g2_standby));
  setup.push_back(router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                     NextHopGroupId(1)));
  setup.push_back(router->SetRouteOp(kDefaultRouteDomainId, P(Ip(20, 0, 0, 0), 8),
                                     NextHopGroupId(2)));
  ASSERT_EQ(Apply(setup).outcome, Outcome::kApplied);

  // Whether `g`'s eligible set is drawn from one tier of its configuration.
  const auto whole = [](const NextHopGroup &g) {
    if (g.eligible_size == 0 || g.eligible_size > NextHopGroup::kMaxMembers) {
      return false;
    }
    const auto in = [&](std::span<const NextHopId> tier) {
      for (uint32_t s = 0; s < g.eligible_size; s++) {
        if (std::find(tier.begin(), tier.end(), g.eligible[s]) == tier.end()) {
          return false;
        }
      }
      return true;
    };
    return in(g.actives()) || in(g.standbys());
  };
  rcu::RcuDomain &rcu = bess::runtime::runtime().rcu();
  const rcu::ReaderId reader_id = 60;
  ASSERT_TRUE(rcu.Register(reader_id).has_value());
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> bad{0}, torn{0}, reads{0};
  std::thread reader([&] {
    rcu.Online(reader_id);
    uint32_t h = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const uint32_t hash = h++ * 0x9E3779B9u;
      const int one = EgressOfHashed(*router, Ip(10, 2, 3, 4), hash);
      const int two = EgressOfHashed(*router, Ip(20, 2, 3, 4), hash);
      if (one < 1 || one > 4 || (two != 3 && two != 5 && two != 6)) {
        bad++;
      }
      for (uint32_t g : {1u, 2u}) {
        const NextHopGroup *group = router->LookupNextHopGroup(NextHopGroupId(g));
        if (group == nullptr || !whole(*group)) {
          torn++;
        }
      }
      reads++;
      rcu.Quiescent(reader_id);
    }
    rcu.Offline(reader_id);
  });
  uint32_t draws = 183;  // xorshift32: a fixed, repeatable sequence of changes
  const auto draw = [&draws] {
    draws ^= draws << 13u;
    draws ^= draws >> 17u;
    draws ^= draws << 5u;
    return draws;
  };
  size_t applied = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  const auto hard = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::set<uint32_t> with_entry;  // the next hops that have a state entry
  while ((std::chrono::steady_clock::now() < deadline || applied < 200) &&
         std::chrono::steady_clock::now() < hard) {
    std::vector<Op> ops;
    std::set<uint32_t> next = with_entry;
    for (uint32_t n = 1 + draw() % 3, k = 0; k < n; k++) {
      const uint32_t hop = 1 + draw() % 6;
      if (std::any_of(ops.begin(), ops.end(), [&](const Op &op) {
            return op.key == dataplane::EncodeKey(NextHopId(hop));
          })) {
        continue;
      }
      if (next.contains(hop) && draw() % 2 == 0) {
        ops.push_back(router->RemoveNextHopStateOp(NextHopId(hop)));
        next.erase(hop);
      } else {
        ops.push_back(router->SetNextHopStateOp(NextHopId(hop), Down(draw() % 3 != 0)));
        next.insert(hop);
      }
    }
    if (draw() % 4 == 0) {  // group 1's actives in another order, or fewer
      std::vector<NextHopId> actives = {NextHopId(1), NextHopId(2), NextHopId(3)};
      std::rotate(actives.begin(), actives.begin() + draw() % 3, actives.end());
      actives.resize(1 + draw() % 3);
      ops.push_back(router->SetNextHopGroupOp(NextHopGroupId(1), actives, g1_standby));
    }
    const auto r = Apply(ops);
    if (r.outcome == Outcome::kApplied) {
      applied++;
      with_entry = std::move(next);
    } else {
      ASSERT_EQ(r.outcome, Outcome::kBusy) << "only backpressure may refuse";
      engine_.ReclaimRetired();
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
  stop = true;
  reader.join();
  rcu.Unregister(reader_id);
  EXPECT_EQ(bad.load(), 0u) << "of " << reads.load();
  EXPECT_EQ(torn.load(), 0u) << "of " << reads.load();
  EXPECT_GE(applied, 200u);
  EXPECT_GT(reads.load(), 0u);
}

// Every allocation a transaction that changes state and groups together makes,
// failed in turn: lookups, state, references and the reverse index stay as
// they were (a later state change behaves as on a router that never saw the
// failure), and the transaction then applies as without the failure.
TEST_P(RouterTransactionTest, PathHealthFailureAtEveryAllocationLeavesNoTrace) {
  struct Run {
    TransactionEngine engine{bess::runtime::runtime().rcu()};
    std::unique_ptr<Router> router;
    std::vector<Op> ops;
    std::vector<Op> later;  // a state change that reads the reverse index
    std::string State() const {
      std::string s = std::to_string(router->next_hop_count()) + "/" +
                      std::to_string(router->next_hop_group_count()) + "/g" +
                      std::to_string(engine.generation());
      for (uint32_t dst : {Ip(10, 0, 0, 1), Ip(20, 0, 0, 1), Ip(30, 0, 0, 1)}) {
        for (uint32_t h = 0; h < 16; h++) {
          s += " " + std::to_string(EgressOfHashed(*router, dst, h * 0x9E3779B9u));
        }
      }
      for (const auto &[id, down] : StatesOf(*router)) {
        s += " s" + std::to_string(id) + (down ? "d" : "u");
      }
      for (uint32_t id = 1; id <= 5; id++) {
        s += " m" + std::to_string(router->GroupMemberships(NextHopId(id)));
      }
      return s;
    }
  };
  const std::vector<NextHopId> ab = {NextHopId(1), NextHopId(2)};
  const std::vector<NextHopId> c = {NextHopId(3)};
  const std::vector<NextHopId> bd = {NextHopId(2), NextHopId(4)};
  const std::vector<NextHopId> abd = {NextHopId(1), NextHopId(2), NextHopId(4)};
  const std::vector<NextHopId> e = {NextHopId(5)};
  const auto prepare = [&](Run &run) {
    run.router = MakeRouter(64, 64, 1, /*groups=*/4);
    Router &router = *run.router;
    ASSERT_TRUE(router.Enroll(run.engine.registry()));
    std::vector<Op> setup;
    for (uint32_t i = 1; i <= 4; i++) {
      setup.push_back(router.SetNextHopOp(NextHopId(i), Hop(i)));
    }
    setup.push_back(router.SetNextHopGroupOp(NextHopGroupId(1), ab, c));
    setup.push_back(router.SetNextHopGroupOp(NextHopGroupId(2), bd));
    setup.push_back(router.SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                      NextHopGroupId(1)));
    setup.push_back(router.SetRouteOp(kDefaultRouteDomainId, P(Ip(20, 0, 0, 0), 8),
                                      NextHopGroupId(2)));
    setup.push_back(router.SetNextHopStateOp(NextHopId(4), Down()));
    ASSERT_EQ(run.engine.Apply(setup).outcome, Outcome::kApplied);
    run.ops = {router.SetNextHopStateOp(NextHopId(2), Down()),
               router.RemoveNextHopStateOp(NextHopId(4)),
               router.SetNextHopGroupOp(NextHopGroupId(1), abd, c),
               router.SetNextHopOp(NextHopId(5), Hop(5)),
               router.SetNextHopGroupOp(NextHopGroupId(3), e, ab),
               router.SetRouteOp(kDefaultRouteDomainId, P(Ip(30, 0, 0, 0), 8),
                                 NextHopGroupId(3))};
    run.later = {router.SetNextHopStateOp(NextHopId(5), Down()),
                 router.SetNextHopStateOp(NextHopId(1), Down())};
  };
  std::string applied;
  {
    Run reference;
    prepare(reference);
    ASSERT_EQ(reference.engine.Apply(reference.ops).outcome, Outcome::kApplied);
    ASSERT_EQ(reference.engine.Apply(reference.later).outcome, Outcome::kApplied);
    applied = reference.State();
  }
  const size_t points = fault_injection::ForEachFailurePoint([&](size_t k) {
    SCOPED_TRACE(::testing::Message() << "failing allocation " << k);
    Run run;
    prepare(run);
    ASSERT_FALSE(::testing::Test::HasFailure());
    const std::string before = run.State();
    bool threw = false;
    TransactionEngine::Result result;
    {
      const fault_injection::AllocationFaults window(k);
      try {
        result = run.engine.Apply(run.ops);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
    }
    if (!threw) {
      ASSERT_EQ(result.outcome, Outcome::kApplied);
    } else {
      ASSERT_EQ(run.State(), before) << "a failed allocation left a trace";
      ASSERT_EQ(run.engine.Apply(run.ops).outcome, Outcome::kApplied);
    }
    ASSERT_EQ(run.engine.Apply(run.later).outcome, Outcome::kApplied);
    ASSERT_EQ(applied, run.State());
  });
  std::printf("[router path health] %zu failure points\n", points);
  EXPECT_GT(points, 10u);
}

// The egresses an IPv6 group route spreads over, by sampling hashes.
std::set<int> Egresses6Of(const Router &router, const char *dst) {
  std::set<int> seen;
  const auto address = bess::utils::ParseIpv6Address(dst);
  if (!address) {
    ADD_FAILURE() << "not an IPv6 address: " << dst;
    return seen;
  }
  for (uint32_t h = 0; h < 4096; h++) {
    const NextHop *hop = router.Resolve(kDefaultRouteDomainId, *address, h * 0x9E3779B9u);
    seen.insert(hop == nullptr ? -1 : static_cast<int>(hop->egress.value()));
  }
  return seen;
}

// Every lookup that picks a group member picks from the eligible set: an IPv6
// group route fails over with the IPv4 one, through the same group object.
TEST_P(RouterTransactionTest, PathHealthAppliesToIpv6GroupRoutes) {
  auto router = MakeRouter(64, 64, 1, /*groups=*/4, /*ipv6=*/true);
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  const NextHopId ab[] = {NextHopId(1), NextHopId(2)};
  const NextHopId c[] = {NextHopId(3)};
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                   router->SetNextHopOp(NextHopId(2), Hop(2)),
                   router->SetNextHopOp(NextHopId(3), Hop(3)),
                   router->SetNextHopGroupOp(NextHopGroupId(1), ab, c),
                   router->SetRouteOp(kDefaultRouteDomainId, P6("2001:db8::", 32),
                                      NextHopGroupId(1)),
                   router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                      NextHopGroupId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(Egresses6Of(*router, "2001:db8::1"), (std::set<int>{1, 2}));
  ASSERT_EQ(Apply({router->SetNextHopStateOp(NextHopId(1), Down())}).outcome, Outcome::kApplied);
  EXPECT_EQ(Egresses6Of(*router, "2001:db8::1"), (std::set<int>{2}));
  ASSERT_EQ(Apply({router->SetNextHopStateOp(NextHopId(2), Down())}).outcome, Outcome::kApplied);
  EXPECT_EQ(Egresses6Of(*router, "2001:db8::1"), (std::set<int>{3})) << "the standby";
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{3})) << "IPv4 alike";
  ASSERT_EQ(Apply({router->RemoveNextHopStateOp(NextHopId(1)),
                   router->RemoveNextHopStateOp(NextHopId(2))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(Egresses6Of(*router, "2001:db8::1"), (std::set<int>{1, 2}));
  Settle();
}

// A next hop bound to a neighbor (D-183) has availability like a static one:
// its entry references "<rt>/neighbors", so the binding cannot be removed
// while the entry names it, and marking it down fails its groups over.
TEST_P(RouterTransactionTest, StateOfABoundNextHopReferencesItsBinding) {
  auto router = MakeRouter(64, 64, 1, /*groups=*/4);
  ASSERT_TRUE(router->SetNeighborBindingPolicy(
      {.capacity = 8,
       .validate = [](const NeighborKey &) -> std::expected<void, std::string> { return {}; }}));
  ASSERT_TRUE(router->Enroll(engine_.registry()));
  const NeighborKey neighbor(dataplane::InterfaceId(7), Ip(192, 0, 2, 7));
  const NextHopId members[] = {NextHopId(1), NextHopId(5)};
  ASSERT_EQ(Apply({router->SetNextHopOp(NextHopId(1), Hop(1)),
                   router->BindNeighborOp(NextHopId(5), neighbor),
                   router->SetNextHopStateOp(NextHopId(5), Down()),
                   router->SetNextHopGroupOp(NextHopGroupId(1), members),
                   router->SetRouteOp(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                                      NextHopGroupId(1))})
                .outcome,
            Outcome::kApplied);
  EXPECT_EQ(EgressesOf(*router, Ip(10, 1, 1, 1)), (std::set<int>{1})) << "the bound member is down";
  EXPECT_EQ(engine_.ReferenceCount(router->neighbors_resource(), dataplane::EncodeKey(NextHopId(5))),
            2u)
      << "the group and the state entry";
  ASSERT_EQ(Apply({router->RemoveRouteOp(P(Ip(10, 0, 0, 0), 8)),
                   router->RemoveNextHopGroupOp(NextHopGroupId(1))})
                .outcome,
            Outcome::kApplied);
  const auto unbind = Apply({router->UnbindNeighborOp(NextHopId(5))});
  EXPECT_NE(unbind.outcome, Outcome::kApplied) << "the state entry names the binding";
  ASSERT_EQ(Apply({router->UnbindNeighborOp(NextHopId(5)),
                   router->RemoveNextHopStateOp(NextHopId(5))})
                .outcome,
            Outcome::kApplied);
  EXPECT_TRUE(StatesOf(*router).empty());
  Settle();
}

BESS_INSTANTIATE_ROUTE_BACKENDS(RouterTransactionTest);
BESS_INSTANTIATE_ROUTE_BACKENDS(RouterLiveUpdateFaultTest);

}  // namespace
}  // namespace bess::route
