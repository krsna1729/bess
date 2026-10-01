// SPDX-License-Identifier: BSD-3-Clause

// M6: route domains inside the one Router. Every domain shares the router's
// next hops; each has its own FIB. These tests pin the domain behaviour --
// independence, fail-closed lookups, concurrent creation, strict
// replacement -- the transaction path is covered in router_transaction_test.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "rcu/rcu_domain.h"
#include "route/route_domain.h"
#include "route/router.h"
#include "runtime/runtime_state.h"

namespace bess::route {
namespace {

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

// RCU reader threads of a concurrency test. They are registered here and, on
// every exit -- including a failed ASSERT that returns early -- stopped,
// joined and unregistered, so a failure is reported instead of terminating
// the process with joinable threads.
class ReaderGroup {
 public:
  ReaderGroup(rcu::RcuDomain &rcu, uint32_t first_id)
      : rcu_(rcu), next_id_(first_id) {}
  ReaderGroup(const ReaderGroup &) = delete;
  ReaderGroup &operator=(const ReaderGroup &) = delete;
  ~ReaderGroup() { Finish(); }

  std::atomic<bool> &stop() { return stop_; }

  // Registers the next reader id and runs `body(id)` on a thread. False if
  // the id could not be registered.
  bool Start(std::function<void(uint32_t)> body) {
    const uint32_t id = next_id_++;
    if (!rcu_.Register(id).has_value()) {
      return false;
    }
    ids_.push_back(id);
    threads_.emplace_back([body = std::move(body), id] { body(id); });
    return true;
  }

  // Stops and joins every reader, then unregisters them. Idempotent.
  void Finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    stop_ = true;
    for (auto &thread : threads_) {
      thread.join();
    }
    for (uint32_t id : ids_) {
      rcu_.Unregister(id);
    }
  }

 private:
  rcu::RcuDomain &rcu_;
  uint32_t next_id_;
  std::atomic<bool> stop_{false};
  bool finished_ = false;
  std::vector<std::thread> threads_;
  std::vector<uint32_t> ids_;
};

LpmRouteTable::Config Cfg(uint32_t routes = 256, uint32_t tbl8 = 16) {
  LpmRouteTable::Config config;
  config.max_routes = routes;
  config.tbl8_groups = tbl8;
  return config;
}

// A router with `domains` domains (0..domains-1) all created, `next_hops` ids.
std::unique_ptr<Router> MakeRouter(size_t domains, size_t next_hops = 64,
                                   LpmRouteTable::Config config = Cfg()) {
  auto router = Router::Create("domain_test", config, next_hops,
                               bess::runtime::runtime().rcu(), domains);
  EXPECT_TRUE(router.has_value());
  if (!router) {
    return nullptr;
  }
  for (uint32_t d = 1; d < domains; d++) {
    EXPECT_TRUE((*router)->CreateDomain(RouteDomainId(d), config));
  }
  return std::move(router).value();
}

int EgressOf(const Router &router, RouteDomainId domain, uint32_t dst) {
  const NextHop *hop = router.Resolve(domain, dst);
  return hop == nullptr ? -1 : static_cast<int>(hop->egress.value());
}

TEST(RouteDomainTest, DefaultDomainExistsAndNothingElseDoes) {
  auto router = MakeRouter(1);
  EXPECT_TRUE(router->HasDomain(kDefaultRouteDomainId));
  EXPECT_EQ(router->domain_count(), 1u);
  EXPECT_EQ(router->max_domains(), 1u);
  EXPECT_FALSE(router->HasDomain(RouteDomainId(1)));

  EXPECT_EQ(Router::Create("x", Cfg(), 8, bess::runtime::runtime().rcu(), 0)
                .error(),
            RouteError::kValueOutOfRange);
  EXPECT_EQ(Router::Create("x", Cfg(), 8, bess::runtime::runtime().rcu(),
                           kMaxRouteDomains + 1)
                .error(),
            RouteError::kValueOutOfRange);
}

// An unknown, removed or out-of-range domain is a miss everywhere -- readers
// and writers -- never undefined behaviour.
TEST(RouteDomainTest, UnknownDomainFailsClosed) {
  auto router = MakeRouter(4);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE(router->SetRoute(P(0, 0), NextHopId(1)));  // default domain

  ASSERT_TRUE(router->RemoveDomain(RouteDomainId(3)));
  const std::array<uint32_t, 3> dst = {Ip(1, 2, 3, 4), Ip(5, 6, 7, 8),
                                       Ip(9, 9, 9, 9)};
  for (const RouteDomainId domain :
       {RouteDomainId(3), RouteDomainId(4), RouteDomainId(1u << 20),
        RouteDomainId(UINT32_MAX - 1), RouteDomainId(UINT32_MAX)}) {
    SCOPED_TRACE(domain.value());
    EXPECT_FALSE(router->HasDomain(domain));
    EXPECT_EQ(nullptr, router->Resolve(domain, dst[0]));
    EXPECT_EQ(kInvalidNextHopId, router->LookupRoute(domain, dst[0]));
    std::array<const NextHop *, 3> hops{};
    EXPECT_EQ(0u, router->ResolveBatch(domain, dst, hops));
    EXPECT_EQ(nullptr, hops[0]);
    EXPECT_EQ(RouteError::kUnknownDomain,
              router->SetRoute(domain, P(0, 0), NextHopId(1)).error());
    EXPECT_EQ(RouteError::kUnknownDomain,
              router->RemoveRoute(domain, P(0, 0)).error());
    EXPECT_EQ(RouteError::kUnknownDomain,
              router->ReplaceRouteSetAtomic(domain, {}).error());
    EXPECT_EQ(0u, router->route_count(domain));
  }
  // The default domain is unaffected.
  EXPECT_EQ(1, EgressOf(*router, kDefaultRouteDomainId, dst[0]));

  EXPECT_EQ(RouteError::kInvalidId,
            router->CreateDomain(RouteDomainId(4), Cfg()).error());
  EXPECT_EQ(RouteError::kDomainExists,
            router->CreateDomain(RouteDomainId(1), Cfg()).error());
  EXPECT_EQ(RouteError::kDomainInUse,
            router->RemoveDomain(kDefaultRouteDomainId).error());
  EXPECT_EQ(RouteError::kUnknownDomain,
            router->RemoveDomain(RouteDomainId(3)).error());
}

TEST(RouteDomainTest, OverlappingPrefixesResolveIndependently) {
  auto router = MakeRouter(3);
  const RouteDomainId d0 = kDefaultRouteDomainId, d1(1), d2(2);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(11)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(2), Hop(22)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(3), Hop(33)));

  // The same private 10/8 in every domain, each to its own next hop, and a
  // longer prefix in one domain only.
  ASSERT_TRUE(router->SetRoute(d0, P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(10, 0, 0, 0), 8), NextHopId(2)));
  ASSERT_TRUE(router->SetRoute(d2, P(Ip(10, 0, 0, 0), 8), NextHopId(3)));
  ASSERT_TRUE(router->SetRoute(d2, P(Ip(10, 1, 0, 0), 16), NextHopId(1)));

  EXPECT_EQ(11, EgressOf(*router, d0, Ip(10, 1, 2, 3)));
  EXPECT_EQ(22, EgressOf(*router, d1, Ip(10, 1, 2, 3)));
  EXPECT_EQ(33, EgressOf(*router, d2, Ip(10, 2, 2, 3)));
  EXPECT_EQ(11, EgressOf(*router, d2, Ip(10, 1, 2, 3))) << "longest prefix";
  EXPECT_EQ(22, EgressOf(*router, d1, Ip(10, 1, 2, 3))) << "not d2's /16";
  EXPECT_EQ(NextHopId(2), router->LookupRoute(d1, Ip(10, 9, 9, 9)));
  EXPECT_EQ(-1, EgressOf(*router, d1, Ip(11, 0, 0, 1)));

  // The single-domain overloads are the default domain's.
  EXPECT_EQ(bess::dataplane::InterfaceId(11), router->Resolve(Ip(10, 1, 2, 3))->egress);
  EXPECT_EQ(router->Resolve(d0, Ip(10, 1, 2, 3)), router->Resolve(Ip(10, 1, 2, 3)));

  // A batch is within one domain; other domains' answers do not leak in.
  const std::array<uint32_t, 4> dst = {Ip(10, 1, 0, 1), Ip(11, 0, 0, 1),
                                       Ip(10, 2, 0, 1), Ip(10, 1, 9, 9)};
  std::array<const NextHop *, 4> hops{};
  EXPECT_EQ(0b1101u, router->ResolveBatch(d2, dst, hops));
  EXPECT_EQ(bess::dataplane::InterfaceId(11), hops[0]->egress);
  EXPECT_EQ(bess::dataplane::InterfaceId(33), hops[2]->egress);
  EXPECT_EQ(bess::dataplane::InterfaceId(11), hops[3]->egress);
  hops = {};
  EXPECT_EQ(0b1101u, router->ResolveBatch(d1, dst, hops));
  EXPECT_EQ(bess::dataplane::InterfaceId(22), hops[0]->egress);
  EXPECT_EQ(bess::dataplane::InterfaceId(22), hops[2]->egress);

  // Removing a route in one domain leaves the same prefix in the others.
  ASSERT_TRUE(router->RemoveRoute(d1, P(Ip(10, 0, 0, 0), 8)));
  EXPECT_EQ(-1, EgressOf(*router, d1, Ip(10, 1, 2, 3)));
  EXPECT_EQ(11, EgressOf(*router, d0, Ip(10, 1, 2, 3)));
  EXPECT_EQ(33, EgressOf(*router, d2, Ip(10, 2, 2, 3)));
  EXPECT_EQ(RouteError::kNotFound,
            router->RemoveRoute(d1, P(Ip(10, 0, 0, 0), 8)).error());

  // Reference counts span the domains.
  EXPECT_EQ(2u, router->RouteReferences(NextHopId(1)));  // d0 /8 and d2 /16
  EXPECT_EQ(0u, router->RouteReferences(NextHopId(2)));
  EXPECT_EQ(RouteError::kNextHopInUse,
            router->RemoveNextHop(NextHopId(1)).error());
  EXPECT_EQ(3u, router->route_count());
  EXPECT_EQ(2u, router->route_count(d2));
}

TEST(RouteDomainTest, DefaultRoutePerDomain) {
  auto router = MakeRouter(3);
  const RouteDomainId d0 = kDefaultRouteDomainId, d1(1), d2(2);
  for (uint32_t i = 1; i <= 3; i++) {
    ASSERT_TRUE(router->SetNextHop(NextHopId(i), Hop(static_cast<uint32_t>(i))));
  }
  ASSERT_TRUE(router->SetRoute(d0, P(0, 0), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(0, 0), NextHopId(2)));
  // d2 has no default route.
  ASSERT_TRUE(router->SetRoute(d2, P(Ip(10, 0, 0, 0), 8), NextHopId(3)));

  EXPECT_EQ(1, EgressOf(*router, d0, Ip(8, 8, 8, 8)));
  EXPECT_EQ(2, EgressOf(*router, d1, Ip(8, 8, 8, 8)));
  EXPECT_EQ(-1, EgressOf(*router, d2, Ip(8, 8, 8, 8)));
  EXPECT_EQ(3, EgressOf(*router, d2, Ip(10, 8, 8, 8)));
  const std::array<uint32_t, 2> dst = {Ip(8, 8, 8, 8), Ip(10, 1, 1, 1)};
  std::array<const NextHop *, 2> hops{};
  EXPECT_EQ(0b11u, router->ResolveBatch(d1, dst, hops));
  EXPECT_EQ(0b10u, router->ResolveBatch(d2, dst, hops));

  ASSERT_TRUE(router->RemoveRoute(d1, P(0, 0)));
  EXPECT_EQ(-1, EgressOf(*router, d1, Ip(8, 8, 8, 8)));
  EXPECT_EQ(1, EgressOf(*router, d0, Ip(8, 8, 8, 8))) << "d0's default stays";
}

// A domain can leave and come back while the router lives; it comes back
// empty, and removing it needs it empty (the reference counts stay exact).
TEST(RouteDomainTest, DomainLifecycle) {
  auto router = MakeRouter(3);
  const RouteDomainId d1(1);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  EXPECT_EQ(RouteError::kDomainInUse, router->RemoveDomain(d1).error());
  EXPECT_TRUE(router->HasDomain(d1));

  ASSERT_TRUE(router->RemoveRoute(d1, P(Ip(10, 0, 0, 0), 8)));
  ASSERT_TRUE(router->RemoveDomain(d1));
  EXPECT_FALSE(router->HasDomain(d1));
  EXPECT_EQ(2u, router->domain_count());
  EXPECT_EQ(-1, EgressOf(*router, d1, Ip(10, 1, 1, 1)));

  ASSERT_TRUE(router->CreateDomain(d1, Cfg()));
  EXPECT_EQ(3u, router->domain_count());
  EXPECT_EQ(-1, EgressOf(*router, d1, Ip(10, 1, 1, 1))) << "comes back empty";
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  EXPECT_EQ(1, EgressOf(*router, d1, Ip(10, 1, 1, 1)));
  EXPECT_EQ(1u, router->RouteReferences(NextHopId(1)));
}

// A removed domain's FIB is not freed under a reader that already holds it.
TEST(RouteDomainTest, RemovedDomainOutlivesOnlineReaders) {
  rcu::RcuDomain &rcu = bess::runtime::runtime().rcu();
  auto router = MakeRouter(2);
  const RouteDomainId d1(1);
  constexpr uint32_t kReader = 40;
  ASSERT_TRUE(rcu.Register(kReader).has_value());
  rcu.Online(kReader);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(0, 0), NextHopId(1)));
  ASSERT_TRUE(router->RemoveRoute(d1, P(0, 0)));
  ASSERT_TRUE(router->RemoveDomain(d1));
  // The reader never passes a quiescent state; re-creating the id must not
  // disturb anything, and the router must stay usable and destructible.
  ASSERT_TRUE(router->CreateDomain(d1, Cfg()));
  ASSERT_TRUE(router->SetRoute(d1, P(0, 0), NextHopId(1)));
  EXPECT_EQ(1, EgressOf(*router, d1, Ip(1, 1, 1, 1)));
  rcu.Quiescent(kReader);
  rcu.Offline(kReader);
  rcu.Unregister(kReader);
}

TEST(RouteDomainTest, ReplaceRouteSetAtomicReplacesTheWholeSet) {
  auto router = MakeRouter(2);
  const RouteDomainId d0 = kDefaultRouteDomainId, d1(1);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(2), Hop(2)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(3), Hop(3)));
  ASSERT_TRUE(router->SetRoute(d0, P(Ip(30, 0, 0, 0), 8), NextHopId(3)));
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(11, 0, 0, 0), 8), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(0, 0), NextHopId(1)));
  EXPECT_EQ(3u, router->RouteReferences(NextHopId(1)));

  // 11/8 is gone, 10/8 moves, a /25 (a tbl8 group) and a new default appear;
  // 12/8 is named twice and takes its last entry.
  const RouteSet next = {
      {P(Ip(10, 0, 0, 0), 8), NextHopId(2)},
      {P(Ip(12, 0, 0, 0), 8), NextHopId(1)},
      {P(Ip(12, 0, 0, 0), 8), NextHopId(2)},
      {P(Ip(20, 0, 0, 128), 25), NextHopId(2)},
      {P(0, 0), NextHopId(3)},
  };
  ASSERT_TRUE(router->ReplaceRouteSetAtomic(d1, next));
  EXPECT_EQ(2, EgressOf(*router, d1, Ip(10, 1, 1, 1)));
  EXPECT_EQ(3, EgressOf(*router, d1, Ip(11, 1, 1, 1))) << "falls to the default";
  EXPECT_EQ(2, EgressOf(*router, d1, Ip(12, 1, 1, 1)));
  EXPECT_EQ(2, EgressOf(*router, d1, Ip(20, 0, 0, 200)));
  EXPECT_EQ(3, EgressOf(*router, d1, Ip(20, 0, 0, 1)));
  EXPECT_EQ(4u, router->route_count(d1));
  EXPECT_EQ(3, EgressOf(*router, d0, Ip(30, 1, 1, 1))) << "d0 untouched";
  EXPECT_EQ(1u, router->route_count(d0));

  // Counts follow the new set exactly: nothing names hop 1 any more.
  EXPECT_EQ(0u, router->RouteReferences(NextHopId(1)));
  EXPECT_EQ(3u, router->RouteReferences(NextHopId(2)));
  EXPECT_EQ(2u, router->RouteReferences(NextHopId(3)));  // d1 default + d0 /8
  ASSERT_TRUE(router->RemoveNextHop(NextHopId(1)));

  // The in-place setters keep working on the replaced table, and an empty
  // set empties the domain.
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(40, 0, 0, 0), 8), NextHopId(2)));
  EXPECT_EQ(2, EgressOf(*router, d1, Ip(40, 1, 1, 1)));
  ASSERT_TRUE(router->ReplaceRouteSetAtomic(d1, {}));
  EXPECT_EQ(0u, router->route_count(d1));
  EXPECT_EQ(-1, EgressOf(*router, d1, Ip(10, 1, 1, 1)));
  EXPECT_EQ(0u, router->RouteReferences(NextHopId(2)));
  EXPECT_EQ(1u, router->RouteReferences(NextHopId(3)));
}

// Every way a replacement can fail leaves the old generation visible and the
// reference counts unchanged.
TEST(RouteDomainTest, FailedReplacementLeavesTheOldGeneration) {
  // Few rules and tbl8 groups, so capacity is easy to exhaust.
  auto router = MakeRouter(2, 8, Cfg(/*routes=*/8, /*tbl8=*/2));
  const RouteDomainId d1(1);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(2), Hop(2)));
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(20, 0, 0, 0), 24), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(0, 0), NextHopId(1)));

  auto expect_old_generation = [&](const char *why) {
    SCOPED_TRACE(why);
    EXPECT_EQ(1, EgressOf(*router, d1, Ip(10, 1, 1, 1)));
    EXPECT_EQ(1, EgressOf(*router, d1, Ip(20, 0, 0, 5)));
    EXPECT_EQ(1, EgressOf(*router, d1, Ip(99, 1, 1, 1)));
    EXPECT_EQ(3u, router->route_count(d1));
    EXPECT_EQ(3u, router->RouteReferences(NextHopId(1)));
    EXPECT_EQ(0u, router->RouteReferences(NextHopId(2)));
    EXPECT_EQ(RouteError::kNextHopInUse,
              router->RemoveNextHop(NextHopId(1)).error());
  };

  // A missing next hop, and ids that are not next hops at all.
  EXPECT_EQ(RouteError::kUnknownNextHop,
            router->ReplaceRouteSetAtomic(
                    d1, {{P(Ip(10, 0, 0, 0), 8), NextHopId(2)},
                         {P(Ip(11, 0, 0, 0), 8), NextHopId(3)}})
                .error());
  expect_old_generation("missing next hop");
  EXPECT_EQ(RouteError::kInvalidId,
            router->ReplaceRouteSetAtomic(
                    d1, {{P(Ip(10, 0, 0, 0), 8), kInvalidNextHopId}})
                .error());
  EXPECT_EQ(RouteError::kInvalidId,
            router->ReplaceRouteSetAtomic(
                    d1, {{P(Ip(10, 0, 0, 0), 8), NextHopId(9)}})
                .error());
  expect_old_generation("invalid next hop id");

  // Three /25s in three /24s need three tbl8 groups; the pool has two.
  RouteSet too_many_groups;
  for (uint32_t i = 0; i < 3; i++) {
    too_many_groups.push_back({P(Ip(50, 0, i, 0), 25), NextHopId(2)});
  }
  EXPECT_EQ(RouteError::kTableFull,
            router->ReplaceRouteSetAtomic(d1, too_many_groups).error());
  expect_old_generation("tbl8 exhaustion");

  // More rules than the table holds.
  RouteSet too_many_rules;
  for (uint32_t i = 0; i < 9; i++) {
    too_many_rules.push_back({P(Ip(60, i, 0, 0), 16), NextHopId(2)});
  }
  EXPECT_EQ(RouteError::kTableFull,
            router->ReplaceRouteSetAtomic(d1, too_many_rules).error());
  expect_old_generation("rule exhaustion");

  // The same sets fit once trimmed: nothing was left half-built, and the
  // groups a failed build used came back.
  too_many_groups.pop_back();
  ASSERT_TRUE(router->ReplaceRouteSetAtomic(d1, too_many_groups));
  EXPECT_EQ(2, EgressOf(*router, d1, Ip(50, 0, 1, 7)));
  EXPECT_EQ(2u, router->RouteReferences(NextHopId(2)));
  EXPECT_EQ(0u, router->RouteReferences(NextHopId(1)));
}

// Readers batch-resolving a domain while it alternates between two route sets
// see one whole set per batch, never a mix: that is what one publication
// means.
TEST(RouteDomainTest, ReplacementIsOneVisibleStep) {
  rcu::RcuDomain &rcu = bess::runtime::runtime().rcu();
  auto router = MakeRouter(2, 8, Cfg(/*routes=*/256, /*tbl8=*/4));
  const RouteDomainId d1(1);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(2), Hop(2)));
  RouteSet sets[2];
  for (uint32_t x = 0; x < 32; x++) {
    sets[0].push_back({P(Ip(10, x, 0, 0), 16), NextHopId(1)});
    sets[1].push_back({P(Ip(10, x, 0, 0), 16), NextHopId(2)});
  }
  ASSERT_TRUE(router->ReplaceRouteSetAtomic(d1, sets[0]));
  std::vector<uint32_t> keys;
  for (uint32_t x = 0; x < 32; x++) {
    keys.push_back(Ip(10, x, 1, 1));
  }

  constexpr int kReaders = 2;
  ReaderGroup group(rcu, 41);
  std::atomic<bool> &stop = group.stop();
  std::atomic<uint64_t> mixed{0}, missed{0}, batches{0};
  for (int r = 0; r < kReaders; r++) {
    ASSERT_TRUE(group.Start([&](uint32_t id) {
      rcu.Online(id);
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        std::array<const NextHop *, 32> hops{};
        if (router->ResolveBatch(d1, keys, hops) != 0xffffffffu) {
          missed++;
        } else {
          for (const NextHop *hop : hops) {
            if (hop->egress != hops[0]->egress) {
              mixed++;
              break;
            }
          }
        }
        local++;
        rcu.Quiescent(id);
      }
      batches += local;
      rcu.Offline(id);
    }));
  }
  // Each replacement builds a 64 MB table and retires the old one, freed only
  // after every reader has passed a quiescent state. A writer that outruns
  // descheduled readers would exhaust the 512 MB test heap (it did, on CI's
  // four CPUs), so it drains after each swap.
  uint64_t swaps = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
  while (std::chrono::steady_clock::now() < deadline || swaps < 3) {
    const auto swapped = router->ReplaceRouteSetAtomic(d1, sets[++swaps % 2]);
    ASSERT_TRUE(swapped.has_value())
        << (swapped ? "" : RouteErrorName(swapped.error()));
    rcu.Drain();
  }
  group.Finish();
  EXPECT_EQ(0u, mixed.load()) << "of " << batches.load() << " batches";
  EXPECT_EQ(0u, missed.load());
  EXPECT_GE(swaps, 3u);
  EXPECT_EQ(32u, router->RouteReferences(NextHopId(1 + swaps % 2)));
}

// Same early-reuse rule as a single-domain router, with routes in domains: an
// id a removed route named is not handed out again while a reader may still
// hold it from any domain.
TEST(RouteDomainTest, NextHopIdIsNotReusedEarly) {
  rcu::RcuDomain &rcu = bess::runtime::runtime().rcu();
  auto router = MakeRouter(3, 8);
  const RouteDomainId d1(1), d2(2);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1)));
  ASSERT_TRUE(router->SetRoute(d1, P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(d2, P(Ip(10, 0, 0, 0), 8), NextHopId(1)));

  constexpr uint32_t kReader = 43;
  ASSERT_TRUE(rcu.Register(kReader).has_value());
  rcu.Online(kReader);
  ASSERT_TRUE(router->RemoveRoute(d1, P(Ip(10, 0, 0, 0), 8)));
  EXPECT_EQ(RouteError::kNextHopInUse,
            router->RemoveNextHop(NextHopId(1)).error())
      << "d2 still names it";
  ASSERT_TRUE(router->RemoveRoute(d2, P(Ip(10, 0, 0, 0), 8)));
  ASSERT_TRUE(router->RemoveNextHop(NextHopId(1)));

  EXPECT_EQ(RouteError::kNextHopRetiring,
            router->SetNextHop(NextHopId(1), Hop(9)).error());
  EXPECT_EQ(RouteError::kUnknownNextHop,
            router->SetRoute(d1, P(Ip(11, 0, 0, 0), 8), NextHopId(1)).error());
  EXPECT_EQ(RouteError::kUnknownNextHop,
            router->ReplaceRouteSetAtomic(
                    d2, {{P(Ip(11, 0, 0, 0), 8), NextHopId(1)}})
                .error());
  rcu.Quiescent(kReader);
  EXPECT_EQ(0u, router->ReclaimRetired());
  EXPECT_TRUE(router->SetNextHop(NextHopId(1), Hop(9)));
  rcu.Offline(kReader);
  rcu.Unregister(kReader);
}

// Readers resolve in every domain while the writer creates domains, updates
// their routes, replaces a route set and removes and re-creates domains. Each
// domain d only ever routes to next hop d + 1 (egress d + 1), so any answer is
// either a miss (the domain does not exist yet, or is empty) or that domain's
// own hop -- a reader that picked up another domain's FIB, or a freed one,
// would show a different egress.
TEST(RouteDomainTest, ConcurrentReadersWhileDomainsAreCreatedAndUpdated) {
  rcu::RcuDomain &rcu = bess::runtime::runtime().rcu();
  // Each domain's rte_lpm is a 64 MB tbl24 and tests run with a 512 MB EAL
  // heap: three domains, plus the replacements and retired tables in flight.
  constexpr uint32_t kDomains = 3;
  auto router = Router::Create("domain_race", Cfg(256, 4), 16, rcu, kDomains);
  ASSERT_TRUE(router.has_value());
  for (uint32_t d = 0; d < kDomains; d++) {
    ASSERT_TRUE((*router)->SetNextHop(NextHopId(d + 1),
                                      Hop(static_cast<uint32_t>(d + 1))));
  }

  constexpr int kReaders = 2;
  ReaderGroup group(rcu, 44);
  std::atomic<bool> &stop = group.stop();
  std::atomic<uint64_t> wrong{0}, hits{0}, lookups{0};
  for (int r = 0; r < kReaders; r++) {
    ASSERT_TRUE(group.Start([&](uint32_t id) {
      rcu.Online(id);
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        for (uint32_t d = 0; d < kDomains + 2; d++) {  // two never exist
          const RouteDomainId domain(d);
          const NextHop *hop = (*router)->Resolve(domain, Ip(10, 5, 1, 1));
          if (hop == nullptr) {
            if (d >= kDomains) {
              continue;
            }
          } else if (d < kDomains && hop->egress.value() == d + 1) {
            hits++;
          } else {
            wrong++;
          }
          std::array<const NextHop *, 2> hops{};
          const std::array<uint32_t, 2> dst = {Ip(10, 5, 1, 1),
                                                Ip(10, 6, 1, 1)};
          const uint64_t mask = (*router)->ResolveBatch(domain, dst, hops);
          for (uint64_t m = mask; m != 0; m &= m - 1) {
            const int i = __builtin_ctzll(m);
            if (d >= kDomains || hops[i]->egress.value() != d + 1) {
              wrong++;
            }
          }
          local++;
        }
        rcu.Quiescent(id);
      }
      lookups += local;
      rcu.Offline(id);
    }));
  }

  Router &router_ref = **router;
  // A removed or replaced FIB is a 64 MB table that is freed only after every
  // reader has passed a quiescent state. The writer loop below outruns readers
  // that are descheduled (a busy or small machine), so it drains after each
  // removal: that bounds the tables in flight to the live ones plus one being
  // built, which fits the 512 MB heap. Without it this test ran out of memory
  // on CI's four CPUs.
  auto ok = [](const std::expected<void, RouteError> &result) {
    return result.has_value();
  };
  uint64_t rounds = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
  while (std::chrono::steady_clock::now() < deadline || rounds < 3) {
    for (uint32_t d = 1; d < kDomains; d++) {
      const RouteDomainId domain(d);
      const auto created = router_ref.CreateDomain(domain, Cfg(256, 4));
      ASSERT_TRUE(ok(created))
          << (created ? "" : RouteErrorName(created.error()));
      ASSERT_TRUE(router_ref.SetRoute(domain, P(Ip(10, 0, 0, 0), 8),
                                      NextHopId(d + 1)));
      ASSERT_TRUE(router_ref.SetRoute(domain, P(Ip(10, 5, 0, 0), 16),
                                      NextHopId(d + 1)));
    }
    ASSERT_TRUE(router_ref.SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
    for (uint32_t d = 1; d < kDomains; d++) {
      const RouteDomainId domain(d);
      if (d % 2 == 0) {
        const auto replaced = router_ref.ReplaceRouteSetAtomic(
            domain, {{P(Ip(10, 0, 0, 0), 8), NextHopId(d + 1)},
                     {P(Ip(10, 6, 0, 0), 16), NextHopId(d + 1)}});
        ASSERT_TRUE(ok(replaced))
            << (replaced ? "" : RouteErrorName(replaced.error()));
        rcu.Drain();
      }
      const auto emptied = router_ref.ReplaceRouteSetAtomic(domain, {});
      ASSERT_TRUE(ok(emptied))
          << (emptied ? "" : RouteErrorName(emptied.error()));
      rcu.Drain();
      ASSERT_TRUE(router_ref.RemoveDomain(domain));
      rcu.Drain();
    }
    ASSERT_TRUE(router_ref.RemoveRoute(P(Ip(10, 0, 0, 0), 8)));
    rounds++;
  }
  group.Finish();
  EXPECT_EQ(0u, wrong.load()) << "of " << lookups.load() << " lookups";
  EXPECT_GT(hits.load(), 0u);
  EXPECT_GE(rounds, 3u);
  EXPECT_EQ(1u, router_ref.domain_count());
  EXPECT_EQ(0u, router_ref.RouteReferences(NextHopId(2)));
}

}  // namespace
}  // namespace bess::route
