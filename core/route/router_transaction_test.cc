// SPDX-License-Identifier: BSD-3-Clause

// A Router enrolled in the transaction engine (Decision D-023): next hops and
// routes as two resources, changed together or not at all, with routes
// placed invisibly during prepare, readers resolving throughout, and every
// allocation of a transaction failed in turn.

#include <gtest/gtest.h>
#include <rte_malloc.h>

#include <any>
#include <atomic>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <new>
#include <random>
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

class RouterTransactionTest : public ::testing::Test {
 protected:
  std::unique_ptr<Router> MakeRouter(uint32_t tbl8_groups = 64,
                                     size_t next_hops = 64,
                                     size_t domains = 1) {
    LpmRouteTable::Config config;
    config.max_routes = 4096;
    config.tbl8_groups = tbl8_groups;
    auto router = Router::Create("rt", config, next_hops,
                                 bess::runtime::runtime().rcu(), domains);
    EXPECT_TRUE(router.has_value());
    // Domains are structural: they exist before the router is enrolled.
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

TEST_F(RouterTransactionTest, EnrolledRoutersAreWrittenOnlyThroughTheEngine) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));  // before: fine
  ASSERT_TRUE(router->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  auto enrolled = router->Enroll(engine_);
  ASSERT_FALSE(enrolled) << "enrolled with routes the ledger cannot see";
  EXPECT_NE(enrolled.error().find("before adding routes"), std::string::npos);
  ASSERT_TRUE(router->RemoveRoute(P(Ip(10, 0, 0, 0), 8)));

  ASSERT_TRUE(router->Enroll(engine_));
  EXPECT_TRUE(router->enrolled());
  EXPECT_EQ(router->next_hops_resource(), "rt/next_hops");
  EXPECT_EQ(router->routes_resource(), "rt/routes");
  EXPECT_FALSE(router->Enroll(engine_));
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

// Next-hop groups are not transactional (D-065): a router with groups cannot
// enroll, and an enrolled router refuses them.
TEST_F(RouterTransactionTest, GroupsAndEnrollmentExcludeEachOther) {
  LpmRouteTable::Config config;
  auto made = Router::Create("rtg", config, 8, bess::runtime::runtime().rcu(),
                             /*max_domains=*/1, /*max_groups=*/4);
  ASSERT_TRUE(made.has_value());
  auto router = std::move(made).value();
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  const NextHopId members[] = {NextHopId(1)};
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(1), members));
  auto enrolled = router->Enroll(engine_);
  ASSERT_FALSE(enrolled);
  EXPECT_NE(enrolled.error().find("not transactional"), std::string::npos);

  ASSERT_TRUE(router->RemoveNextHopGroup(NextHopGroupId(1)));
  Settle();
  EXPECT_EQ(0u, router->ReclaimRetired());
  ASSERT_TRUE(router->Enroll(engine_));
  EXPECT_EQ(RouteError::kEnrolled,
            router->SetNextHopGroup(NextHopGroupId(1), members).error());
  EXPECT_EQ(RouteError::kEnrolled,
            router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                             NextHopGroupId(1))
                .error());
  EXPECT_EQ(RouteError::kEnrolled,
            router->RemoveNextHopGroup(NextHopGroupId(1)).error());
}

TEST_F(RouterTransactionTest, NextHopsAndRoutesChangeTogether) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_));
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

TEST_F(RouterTransactionTest, RoutesBeingPlacedChangeNoLookup) {
  for (bool with_default : {false, true}) {
    SCOPED_TRACE(with_default ? "with a default route" : "no default route");
    auto router = MakeRouter();
    ASSERT_TRUE(router->Enroll(engine_));
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

TEST_F(RouterTransactionTest, RoutesThatDoNotFitRejectWithNothingVisible) {
  auto router = MakeRouter(/*tbl8_groups=*/2);
  ASSERT_TRUE(router->Enroll(engine_));
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
TEST_F(RouterTransactionTest, RoutesInTwoDomainsInOneTransaction) {
  auto router = MakeRouter(/*tbl8_groups=*/64, /*next_hops=*/64, /*domains=*/3);
  const RouteDomainId d1(1), d2(2);
  ASSERT_TRUE(router->Enroll(engine_));

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
TEST_F(RouterTransactionTest, PlacementCapacityIsPerDomain) {
  auto router = MakeRouter(/*tbl8_groups=*/2, /*next_hops=*/8, /*domains=*/3);
  ASSERT_TRUE(router->Enroll(engine_));
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

TEST_F(RouterTransactionTest, OtherResourcesReferenceNextHops) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->Enroll(engine_));
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
  EXPECT_EQ(router->RouteReferences(NextHopId(4)), 2u);  // route + action

  // The router's resources cannot leave while "actions" may reference them.
  const std::string names[] = {router->routes_resource(), hops};
  EXPECT_FALSE(engine_.Unregister(names));
  ASSERT_EQ(Apply({Op::Erase("actions", dataplane::EncodeKey(ActionId(1)))})
                .outcome,
            Outcome::kApplied);
  Settle();
  ASSERT_TRUE(engine_.Unregister("actions"));
}

// Readers resolve through the router while transactions add a next hop and a
// route per session, and remove an older session's route and next hop. A
// resolved address only ever reaches its own session's next hop.
TEST_F(RouterTransactionTest, ResolvesWhileTransactionsRun) {
  constexpr uint32_t kSessions = 2000;  // next hop ids 1..kSessions
  constexpr uint32_t kLive = 300;
  constexpr uint32_t kSteps = 6000;
  auto router = MakeRouter(/*tbl8_groups=*/64, /*next_hops=*/kSessions);
  ASSERT_TRUE(router->Enroll(engine_));
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
TEST_F(RouterTransactionTest, FailureAtEveryAllocationLeavesNoTrace) {
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
    ASSERT_TRUE(router.Enroll(run.engine));
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
  size_t faults = 0;
  fault_injection::ForEachFailurePoint([&](size_t k) {
    SCOPED_TRACE(testing::Message() << "failing allocation " << k);
    Run run;
    prepare(run);
    ASSERT_FALSE(testing::Test::HasFailure());
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
    faults++;
    ASSERT_EQ(run.State(), before) << "a failed allocation left a trace";
    ASSERT_EQ(run.engine.Apply(run.ops).outcome, Outcome::kApplied);
    ASSERT_EQ(applied, run.State());
  });
  std::printf("[router] %zu allocation sites failed in turn\n", faults);
  EXPECT_GT(faults, 10u);
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
         (group == nullptr ? std::string("-") : std::to_string(group->size));
  }
  return s;
}

// A router with two extra domains, four next hops, two groups and routes in
// both domains, every retirement completed.
std::unique_ptr<Router> LiveRouter() {
  LpmRouteTable::Config config;
  config.max_routes = 64;
  config.tbl8_groups = 16;
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
TEST(RouterLiveUpdateFaultTest, FailureAtEveryAllocationLeavesNoTrace) {
  LpmRouteTable::Config config;
  config.max_routes = 64;
  config.tbl8_groups = 16;
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
      SCOPED_TRACE(testing::Message() << "failing allocation " << k);
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
    EXPECT_GE(points, 1u);
  }
}

}  // namespace
}  // namespace bess::route
