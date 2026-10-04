// SPDX-License-Identifier: BSD-3-Clause

// K7: route tables and next hops.
//
// The LPM differential is the gate entry 34 established (it is what caught
// rte_fib): a large rule set inserted in arbitrary order -- a /24 after a /28
// under it -- must agree with an independent longest-prefix match on every
// key, and must still agree after delete/re-add churn. The concurrent tests
// then check what K7 adds on top of rte_lpm: that readers running during
// in-place updates only ever see answers some rule justifies, and that a
// resolved route always has its next hop.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "runtime/runtime_state.h"
#include "packet_pool.h"
#include "route/route_table.h"
#include "route/router.h"

namespace bess::route {
namespace {

struct ValueTag;
using Value = dataplane::StrongId<ValueTag, uint32_t>;

Ipv4Prefix P(uint32_t addr, uint8_t len) { return Ipv4Prefix::Make(addr, len).value(); }

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a << 24 | b << 16 | c << 8 | d;
}

std::unique_ptr<RouteTable<Value>> MakeTable(uint32_t routes = 1024,
                                             uint32_t tbl8 = 256) {
  LpmRouteTable::Config config;
  config.max_routes = routes;
  config.tbl8_groups = tbl8;
  auto table = RouteTable<Value>::Create("route_test", config,
                                         bess::runtime::runtime().rcu());
  EXPECT_TRUE(table.has_value()) << RouteErrorName(table.error());
  return std::move(table).value();
}

// An independent longest-prefix match: one hash map per prefix length.
class ReferenceLpm {
 public:
  void Set(Ipv4Prefix p, uint32_t v) { by_len_[p.length()][p.addr()] = v; }
  void Erase(Ipv4Prefix p) { by_len_[p.length()].erase(p.addr()); }
  std::optional<uint32_t> Lookup(uint32_t addr) const {
    for (int len = 32; len >= 0; len--) {
      const uint32_t mask = len == 0 ? 0 : ~uint32_t{0} << (32 - len);
      const auto &m = by_len_[len];
      if (auto it = m.find(addr & mask); it != m.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

 private:
  std::array<std::unordered_map<uint32_t, uint32_t>, 33> by_len_;
};

// Mostly /24s, some shorter, and longer prefixes nested under earlier /24s --
// the shape entry 34 used, in generator (arbitrary) order.
std::vector<std::pair<Ipv4Prefix, uint32_t>> RandomRoutes(size_t n,
                                                          uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<std::pair<Ipv4Prefix, uint32_t>> routes;
  std::set<Ipv4Prefix> seen;
  std::vector<uint32_t> slash24s;
  while (routes.size() < n) {
    const uint32_t r = static_cast<uint32_t>(rng());
    const int kind = static_cast<int>(rng() % 10);
    uint8_t len;
    uint32_t addr;
    if (kind < 6 || slash24s.empty()) {
      len = 24;
      addr = r & 0xffffff00;
    } else if (kind < 8) {
      len = static_cast<uint8_t>(8 + rng() % 16);  // /8../23
      addr = r & (~uint32_t{0} << (32 - len));
    } else {
      len = static_cast<uint8_t>(25 + rng() % 8);  // /25../32 under a /24
      const uint32_t base = slash24s[rng() % slash24s.size()];
      addr = (base | (r & 0xff)) & (~uint32_t{0} << (32 - len));
    }
    const Ipv4Prefix p = P(addr, len);
    if (!seen.insert(p).second) {
      continue;
    }
    if (len == 24) {
      slash24s.push_back(addr);
    }
    routes.emplace_back(p, 1 + static_cast<uint32_t>(rng() % 60000));
  }
  return routes;
}

// Keys inside every route (random host bits) plus uniformly random keys.
std::vector<uint32_t> ProbeKeys(
    const std::vector<std::pair<Ipv4Prefix, uint32_t>> &routes, size_t random,
    uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<uint32_t> keys;
  for (const auto &[p, v] : routes) {
    const uint32_t host = p.length() == 32
                              ? 0
                              : static_cast<uint32_t>(rng()) &
                                    (~uint32_t{0} >> p.length());
    keys.push_back(p.addr() | host);
  }
  for (size_t i = 0; i < random; i++) {
    keys.push_back(static_cast<uint32_t>(rng()));
  }
  return keys;
}

void ExpectAgrees(const RouteTable<Value> &table, const ReferenceLpm &ref,
                  const std::vector<uint32_t> &keys) {
  const auto view = table.Read();
  size_t mismatches = 0;
  for (size_t base = 0; base < keys.size(); base += 32) {
    const size_t n = std::min<size_t>(32, keys.size() - base);
    uint32_t values[32];
    const uint64_t hits = view.LookupBatch(std::span(&keys[base], n),
                                           std::span(values, n));
    for (size_t i = 0; i < n; i++) {
      const auto expected = ref.Lookup(keys[base + i]);
      const bool hit = (hits >> i) & 1;
      const auto single = view.Lookup(keys[base + i]);
      if (hit != expected.has_value() || (hit && values[i] != *expected) ||
          single.has_value() != expected.has_value() ||
          (single && single->value() != *expected)) {
        if (mismatches++ < 5) {
          ADD_FAILURE() << "key " << std::hex << keys[base + i] << std::dec
                        << ": expected "
                        << (expected ? std::to_string(*expected) : "miss")
                        << ", batch " << (hit ? std::to_string(values[i]) : "miss")
                        << ", single "
                        << (single ? std::to_string(single->value()) : "miss");
        }
      }
    }
  }
  EXPECT_EQ(0u, mismatches) << "of " << keys.size() << " keys";
}

// -- Ipv4Prefix / RouteTable basics -------------------------------------------

TEST(Ipv4PrefixTest, Validation) {
  EXPECT_TRUE(Ipv4Prefix::Make(Ip(10, 0, 0, 0), 8).has_value());
  EXPECT_TRUE(Ipv4Prefix::Make(0, 0).has_value());
  EXPECT_TRUE(Ipv4Prefix::Make(Ip(1, 2, 3, 4), 32).has_value());
  EXPECT_EQ(RouteError::kInvalidPrefixLength,
            Ipv4Prefix::Make(0, 33).error());
  EXPECT_EQ(RouteError::kHostBitsSet,
            Ipv4Prefix::Make(Ip(10, 0, 0, 1), 24).error());
  EXPECT_EQ(RouteError::kHostBitsSet, Ipv4Prefix::Make(1, 0).error());
}

TEST(RouteTableTest, LongestPrefixDefaultAndClear) {
  auto table = MakeTable();
  ASSERT_TRUE(table->Upsert(P(Ip(10, 0, 0, 0), 8), Value(1)));
  ASSERT_TRUE(table->Upsert(P(Ip(10, 1, 0, 0), 16), Value(2)));
  ASSERT_TRUE(table->Upsert(P(Ip(10, 1, 2, 0), 28), Value(3)));
  EXPECT_EQ(Value(1), table->Read().Lookup(Ip(10, 9, 9, 9)));
  EXPECT_EQ(Value(2), table->Read().Lookup(Ip(10, 1, 9, 9)));
  EXPECT_EQ(Value(3), table->Read().Lookup(Ip(10, 1, 2, 15)));
  EXPECT_EQ(Value(2), table->Read().Lookup(Ip(10, 1, 2, 16)));
  EXPECT_FALSE(table->Read().Lookup(Ip(11, 0, 0, 0)).has_value());

  ASSERT_TRUE(table->Upsert(P(0, 0), Value(9)));  // default
  EXPECT_EQ(Value(9), table->Read().Lookup(Ip(11, 0, 0, 0)));
  ASSERT_TRUE(table->Upsert(P(Ip(10, 1, 0, 0), 16), Value(5)));  // re-point
  EXPECT_EQ(Value(5), table->Read().Lookup(Ip(10, 1, 9, 9)));
  EXPECT_EQ(Value(5), table->Find(P(Ip(10, 1, 0, 0), 16)));
  EXPECT_EQ(4u, table->size());

  ASSERT_TRUE(table->Erase(P(Ip(10, 1, 2, 0), 28)));
  EXPECT_EQ(Value(5), table->Read().Lookup(Ip(10, 1, 2, 15)));
  EXPECT_EQ(RouteError::kNotFound, table->Erase(P(Ip(10, 1, 2, 0), 28)).error());

  ASSERT_TRUE(table->Clear());
  EXPECT_EQ(1u, table->size()) << "Clear keeps the default route";
  EXPECT_EQ(Value(9), table->Read().Lookup(Ip(10, 1, 2, 15)));
  ASSERT_TRUE(table->Erase(P(0, 0)));
  EXPECT_FALSE(table->Read().Lookup(Ip(10, 1, 2, 15)).has_value());
  EXPECT_EQ(RouteError::kNotFound, table->Erase(P(0, 0)).error());
}

TEST(RouteTableTest, LimitsAreErrors) {
  auto table = MakeTable(/*routes=*/4, /*tbl8=*/1);
  EXPECT_EQ(RouteError::kValueOutOfRange,
            table->Upsert(P(Ip(1, 0, 0, 0), 8), Value(1u << 24)).error());
  // Two /25s in different /24s need two tbl8 groups; there is one.
  ASSERT_TRUE(table->Upsert(P(Ip(1, 1, 1, 0), 25), Value(1)));
  EXPECT_EQ(RouteError::kTableFull,
            table->Upsert(P(Ip(1, 1, 2, 0), 25), Value(1)).error());
  EXPECT_FALSE(table->Find(P(Ip(1, 1, 2, 0), 25)).has_value());
}

TEST(RouteTableTest, BatchMaskWithoutDefault) {
  auto table = MakeTable();
  ASSERT_TRUE(table->Upsert(P(Ip(10, 0, 0, 0), 8), Value(7)));
  const std::array<uint32_t, 6> keys = {Ip(10, 0, 0, 1), Ip(11, 0, 0, 1),
                                        Ip(10, 2, 0, 1), Ip(12, 0, 0, 1),
                                        Ip(10, 3, 0, 1), Ip(13, 0, 0, 1)};
  std::array<uint32_t, 6> values;
  values.fill(12345);
  const uint64_t hits = table->Read().LookupBatch(keys, values);
  EXPECT_EQ(0b010101u, hits);
  EXPECT_EQ(7u, values[0]);
  EXPECT_EQ(7u, values[2]);
  EXPECT_EQ(7u, values[4]);
}

// -- the correctness gate -------------------------------------------------------

TEST(RouteTableTest, LargeArbitraryOrderMatchesReferenceThroughChurn) {
  constexpr size_t kRoutes = 32768;
  auto table = MakeTable(kRoutes * 2, 8192);
  ReferenceLpm ref;
  auto routes = RandomRoutes(kRoutes, 0x6b37);
  for (const auto &[p, v] : routes) {
    ASSERT_TRUE(table->Upsert(p, Value(v))) << "route " << p.addr();
    ref.Set(p, v);
  }
  const std::vector<uint32_t> keys = ProbeKeys(routes, 65536, 0x6b38);
  ExpectAgrees(*table, ref, keys);

  // Churn: delete a random third, re-point another third, re-add the deleted.
  std::mt19937_64 rng(0x6b39);
  std::shuffle(routes.begin(), routes.end(), rng);
  const size_t third = routes.size() / 3;
  for (size_t i = 0; i < third; i++) {
    ASSERT_TRUE(table->Erase(routes[i].first));
    ref.Erase(routes[i].first);
  }
  ExpectAgrees(*table, ref, keys);
  for (size_t i = third; i < 2 * third; i++) {
    routes[i].second = 1 + static_cast<uint32_t>(rng() % 60000);
    ASSERT_TRUE(table->Upsert(routes[i].first, Value(routes[i].second)));
    ref.Set(routes[i].first, routes[i].second);
  }
  for (size_t i = 0; i < third; i++) {
    ASSERT_TRUE(table->Upsert(routes[i].first, Value(routes[i].second)));
    ref.Set(routes[i].first, routes[i].second);
  }
  ExpectAgrees(*table, ref, keys);
  EXPECT_EQ(kRoutes, table->size());
}

// -- concurrent in-place updates ---------------------------------------------------

// DPDK contract (rte_lpm with QSBR in defer-queue mode), deterministically: a
// tbl8 group freed by a delete is not handed to another /24 while a reader
// that was online before the delete has not reported quiescence -- that
// reader may have loaded the old tbl24 entry and be about to read the group.
// Once the reader is quiescent, the next add reclaims the group. The pool
// holds one group, so the second /24's add can only succeed by reuse.
TEST(RouteTableTest, FreedTbl8GroupWaitsForOnlineReaders) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  auto table = MakeTable(64, /*tbl8=*/1);
  const Ipv4Prefix a = P(Ip(10, 1, 1, 64), 26);
  const Ipv4Prefix b = P(Ip(10, 2, 2, 64), 26);
  ASSERT_TRUE(table->Upsert(a, Value(1)));
  auto no_group = table->Upsert(b, Value(2));
  ASSERT_FALSE(no_group);
  ASSERT_EQ(RouteError::kTableFull, no_group.error()) << "pool is one group";

  constexpr uint32_t kReader = 22;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);
  ASSERT_TRUE(table->Erase(a));
  auto held = table->Upsert(b, Value(2));
  EXPECT_FALSE(held) << "a freed tbl8 group was reused mid-grace-period";

  domain.Quiescent(kReader);
  EXPECT_TRUE(table->Upsert(b, Value(2)))
      << "the group never came back after the reader passed quiescence";
  EXPECT_EQ(Value(2), table->Find(b));
  domain.Offline(kReader);
  domain.Unregister(kReader);
}

// A DPDK behaviour the transactional route resource depends on (D-023): a
// delete never fails, even with every tbl8 group freed at once while a
// reader stalls. rte_lpm's QSBR defer queue defaults to one entry per tbl8
// group, so it always has room; aborting placed routes and publishing
// erases rely on it.
TEST(RouteTableTest, DeletesNeverFailWithAStalledReader) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  constexpr uint32_t kGroups = 8;
  auto table = MakeTable(64, kGroups);
  for (uint32_t i = 0; i < kGroups; i++) {
    ASSERT_TRUE(table->Upsert(P(Ip(10, 0, i, 128), 25), Value(i + 1)));
  }
  constexpr uint32_t kReader = 23;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);  // and never quiescent until the end
  for (int round = 0; round < 2; round++) {
    for (uint32_t i = 0; i < kGroups; i++) {
      ASSERT_TRUE(table->Erase(P(Ip(10, 0, i, 128), 25)))
          << "delete " << i << " failed with the defer queue holding "
          << "every group";
    }
    // Nothing can be re-added while the reader holds the groups.
    EXPECT_FALSE(table->Upsert(P(Ip(10, 0, 0, 128), 25), Value(1)));
    domain.Quiescent(kReader);
    for (uint32_t i = 0; i < kGroups; i++) {
      ASSERT_TRUE(table->Upsert(P(Ip(10, 0, i, 128), 25), Value(i + 1)));
    }
  }
  domain.Offline(kReader);
  domain.Unregister(kReader);
}

// Readers (registered RCU readers reporting quiescence between batches, as
// workers do) look up a fixed key set while the writer churns /26 routes,
// each of which needs a tbl8 group. The pool is deliberately tiny, so nearly
// every add reuses a group another /24 just freed -- through the QSBR defer
// queue, which is what must keep a group from being refilled while a reader
// that loaded the old tbl24 entry is still about to read it. Every answer
// must be one some rule justifies: the covering background /16's value, or
// the /26's value for keys inside it. A group reused under a reader would
// surface as another /24's value.
TEST(RouteTableTest, ConcurrentReadersOnlySeeJustifiedAnswers) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  auto table = MakeTable(4096, /*tbl8=*/4);

  constexpr uint32_t kSubnets = 64;
  for (uint32_t x = 0; x < kSubnets; x++) {
    ASSERT_TRUE(table->Upsert(P(Ip(10, x, 0, 0), 16), Value(1000 + x)));
  }
  std::vector<std::pair<Ipv4Prefix, uint32_t>> churn;
  std::vector<uint32_t> keys;
  std::vector<std::array<uint32_t, 2>> allowed;
  for (uint32_t x = 0; x < kSubnets; x++) {
    churn.emplace_back(P(Ip(10, x, 9, 64), 26), 3000 + x);
    keys.push_back(Ip(10, x, 9, 70));  // inside the /26
    allowed.push_back({1000 + x, 3000 + x});
    keys.push_back(Ip(10, x, 9, 5));  // same /24, outside the /26
    allowed.push_back({1000 + x, 1000 + x});
  }

  constexpr int kReaders = 2;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> bad{0}, lookups{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    const uint32_t reader_id = 10 + r;
    ASSERT_TRUE(domain.Register(reader_id).has_value());
    readers.emplace_back([&, reader_id] {
      domain.Online(reader_id);
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const auto view = table->Read();
        for (size_t base = 0; base < keys.size(); base += 32) {
          uint32_t values[32];
          const uint64_t hits = view.LookupBatch(std::span(&keys[base], 32),
                                                 std::span(values, 32));
          for (size_t i = 0; i < 32; i++) {
            const auto &ok = allowed[base + i];
            if (!((hits >> i) & 1) ||
                (values[i] != ok[0] && values[i] != ok[1])) {
              bad++;
            }
          }
        }
        local += keys.size();
        domain.Quiescent(reader_id);
      }
      lookups += local;
      domain.Offline(reader_id);
    });
  }

  // At least 500 ms of churn under the readers, and at least kAdds adds
  // however many CPUs they share (on one CPU the readers take most of the
  // time, and adds wait for grace periods the readers must report). The hard
  // deadline only stops a hang.
  constexpr uint64_t kAdds = 1000;
  std::mt19937_64 rng(0x6b3a);
  uint64_t adds = 0, full = 0;
  const auto start = std::chrono::steady_clock::now();
  const auto window = start + std::chrono::milliseconds(500);
  const auto hard_deadline = start + std::chrono::seconds(20);
  while ((std::chrono::steady_clock::now() < window || adds < kAdds) &&
         std::chrono::steady_clock::now() < hard_deadline) {
    const auto &[p, v] = churn[rng() % churn.size()];
    if (table->Find(p)) {
      ASSERT_TRUE(table->Erase(p));
    } else {
      auto added = table->Upsert(p, Value(v));
      // Adds can transiently find no free group while freed ones wait out a
      // reader grace period; that is the defer queue working.
      ASSERT_TRUE(added || added.error() == RouteError::kTableFull);
      if (added) {
        adds++;
      } else {
        full++;
        // A short sleep lets every reader run and report quiescence, so
        // retired groups free (a yield gives the CPU to one thread only, and
        // retrying at once without one starves the readers on few CPUs).
        std::this_thread::sleep_for(std::chrono::microseconds(20));
      }
    }
  }
  stop = true;
  for (auto &t : readers) {
    t.join();
  }
  for (int r = 0; r < kReaders; r++) {
    domain.Unregister(10 + r);
  }

  EXPECT_EQ(0u, bad.load()) << "of " << lookups.load() << " lookups, "
                            << adds << " adds, " << full << " full";
  EXPECT_GE(adds, kAdds);
}

// -- Router -----------------------------------------------------------------------

NextHop Hop(uint32_t egress, uint8_t mac_tail,
            NeighborState state = NeighborState::kResolved) {
  NextHop hop;
  hop.egress = bess::dataplane::InterfaceId(egress);
  hop.neighbor = state;
  hop.dst_mac.bytes[5] = mac_tail;
  hop.src_mac.bytes[5] = 0xee;
  return hop;
}

std::unique_ptr<Router> MakeRouter(size_t next_hops = 64) {
  LpmRouteTable::Config config;
  config.max_routes = 4096;
  config.tbl8_groups = 256;
  auto router = Router::Create("router_test", config, next_hops,
                               bess::runtime::runtime().rcu());
  EXPECT_TRUE(router.has_value()) << RouteErrorName(router.error());
  return std::move(router).value();
}

TEST(RouterTest, RoutesNeedNextHopsAndPinThem) {
  auto router = MakeRouter();
  EXPECT_EQ(RouteError::kUnknownNextHop,
            router->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)).error());
  EXPECT_EQ(RouteError::kInvalidId,
            router->SetNextHop(kInvalidNextHopId, Hop(1, 1)).error());
  EXPECT_EQ(RouteError::kInvalidId,
            router->SetNextHop(NextHopId(65), Hop(1, 1)).error());

  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(3, 0xa1)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(2), Hop(4, 0xa2)));
  ASSERT_TRUE(router->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(P(Ip(10, 1, 0, 0), 16), NextHopId(1)));
  EXPECT_EQ(2u, router->RouteReferences(NextHopId(1)));
  EXPECT_EQ(RouteError::kNextHopInUse,
            router->RemoveNextHop(NextHopId(1)).error());

  ASSERT_TRUE(router->SetRoute(P(Ip(10, 1, 0, 0), 16), NextHopId(2)));
  EXPECT_EQ(1u, router->RouteReferences(NextHopId(1)));
  EXPECT_EQ(1u, router->RouteReferences(NextHopId(2)));

  const NextHop *hop = router->Resolve(Ip(10, 1, 5, 5));
  ASSERT_NE(nullptr, hop);
  EXPECT_EQ(bess::dataplane::InterfaceId(4), hop->egress);
  EXPECT_EQ(nullptr, router->Resolve(Ip(11, 0, 0, 0)));

  ASSERT_TRUE(router->RemoveRoute(P(Ip(10, 0, 0, 0), 8)));
  ASSERT_TRUE(router->RemoveNextHop(NextHopId(1)));
  EXPECT_EQ(1u, router->next_hop_count());
  EXPECT_EQ(RouteError::kUnknownNextHop,
            router->RemoveNextHop(NextHopId(1)).error());
  EXPECT_EQ(RouteError::kNotFound,
            router->RemoveRoute(P(Ip(10, 0, 0, 0), 8)).error());
}

TEST(RouterTest, NeighborUpdateNeedsNoRouteChange) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->SetNextHop(NextHopId(5),
                                 Hop(2, 0, NeighborState::kIncomplete)));
  ASSERT_TRUE(router->SetRoute(P(0, 0), NextHopId(5)));  // default
  EXPECT_EQ(NeighborState::kIncomplete,
            router->Resolve(Ip(8, 8, 8, 8))->neighbor);

  ASSERT_TRUE(router->SetNextHop(NextHopId(5), Hop(2, 0x55)));
  const NextHop *hop = router->Resolve(Ip(8, 8, 8, 8));
  EXPECT_EQ(NeighborState::kResolved, hop->neighbor);
  EXPECT_EQ(0x55, hop->dst_mac.bytes[5]);
  EXPECT_EQ(1u, router->route_count());
}

TEST(RouterTest, ResolveBatch) {
  auto router = MakeRouter();
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1, 1)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(2), Hop(2, 2)));
  ASSERT_TRUE(router->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)));
  ASSERT_TRUE(router->SetRoute(P(Ip(20, 0, 0, 0), 8), NextHopId(2)));
  const std::array<uint32_t, 5> dst = {Ip(10, 1, 1, 1), Ip(30, 0, 0, 1),
                                       Ip(20, 1, 1, 1), Ip(10, 2, 2, 2),
                                       Ip(20, 9, 9, 9)};
  std::array<const NextHop *, 5> hops{};
  EXPECT_EQ(0b11101u, router->ResolveBatch(dst, hops));
  EXPECT_EQ(bess::dataplane::InterfaceId(1), hops[0]->egress);
  EXPECT_EQ(nullptr, hops[1]);
  EXPECT_EQ(bess::dataplane::InterfaceId(2), hops[2]->egress);
  EXPECT_EQ(bess::dataplane::InterfaceId(2), hops[4]->egress);
}

// Readers resolving continuously while the writer adds next hops, points
// routes at them, removes the routes, and removes the next hops: every key is
// under the stable background route, so every batch must resolve every key --
// to the background hop or to whichever churn hop its more-specific route
// named. A reader that found a route whose next hop had already been removed
// would come back with the bit cleared; RemoveNextHop's grace period is what
// prevents that.
TEST(RouterTest, ConcurrentChurnNeverLosesANextHop) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  auto router = MakeRouter(/*next_hops=*/64);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1, 1)));
  ASSERT_TRUE(router->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)));

  std::vector<uint32_t> keys;
  for (uint32_t x = 0; x < 32; x++) {
    keys.push_back(Ip(10, x, 1, 1));
  }
  constexpr int kReaders = 2;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> lost{0}, batches{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; r++) {
    const uint32_t reader_id = 20 + r;
    ASSERT_TRUE(domain.Register(reader_id).has_value());
    readers.emplace_back([&, reader_id] {
      domain.Online(reader_id);
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        std::array<const NextHop *, 32> hops{};
        if (router->ResolveBatch(keys, hops) != 0xffffffffu) {
          lost++;
        }
        local++;
        domain.Quiescent(reader_id);
      }
      batches += local;
      domain.Offline(reader_id);
    });
  }

  uint64_t rounds = 0, retiring_refusals = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    const uint32_t x = static_cast<uint32_t>(rounds++ % 32);
    const NextHopId id(2 + x);
    const Ipv4Prefix p = P(Ip(10, x, 0, 0), 16);
    // An id removed 32 rounds ago may still be retiring (readers not yet
    // past its grace period); reuse is refused until then.
    if (auto set = router->SetNextHop(id, Hop(static_cast<uint32_t>(2 + x), 2));
        !set) {
      ASSERT_EQ(RouteError::kNextHopRetiring, set.error());
      retiring_refusals++;
      continue;
    }
    ASSERT_TRUE(router->SetRoute(p, id));
    ASSERT_TRUE(router->RemoveRoute(p));
    ASSERT_TRUE(router->RemoveNextHop(id));
  }
  stop = true;
  for (auto &t : readers) {
    t.join();
  }
  for (int r = 0; r < kReaders; r++) {
    domain.Unregister(20 + r);
  }
  EXPECT_EQ(0u, lost.load()) << "of " << batches.load() << " batches, "
                             << rounds << " rounds, " << retiring_refusals
                             << " reuse refusals";
  EXPECT_GT(rounds, 100u);
}

// Removing a next hop does not wait for readers. With a reader online that
// never reports quiescence, RemoveNextHop() returns at once (the former
// blocking implementation would hang here). Until the reader passes a
// quiescent state the id is retiring: routes cannot name it and it cannot be
// reused, so a reader still holding it from a removed route can never reach a
// different next hop. Afterwards the next control call drops it.
TEST(RouterTest, NextHopRemovalIsDeferredNotBlocking) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  auto router = MakeRouter(/*next_hops=*/8);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1, 1)));
  ASSERT_TRUE(router->SetNextHop(NextHopId(2), Hop(2, 2)));
  ASSERT_TRUE(router->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1)));

  constexpr uint32_t kReader = 24;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);
  ASSERT_TRUE(router->RemoveRoute(P(Ip(10, 0, 0, 0), 8)));
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(router->RemoveNextHop(NextHopId(1)));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1))
      << "removal must not wait for readers";

  EXPECT_EQ(1u, router->next_hop_count()) << "retiring hops are not live";
  EXPECT_EQ(1u, router->ReclaimRetired()) << "still waiting for the reader";
  const auto reuse = router->SetNextHop(NextHopId(1), Hop(3, 3));
  ASSERT_FALSE(reuse.has_value()) << "a retiring id was reused";
  EXPECT_EQ(RouteError::kNextHopRetiring, reuse.error());
  const auto route = router->SetRoute(P(Ip(11, 0, 0, 0), 8), NextHopId(1));
  ASSERT_FALSE(route.has_value()) << "a route named a retiring id";
  EXPECT_EQ(RouteError::kUnknownNextHop, route.error());
  const auto again = router->RemoveNextHop(NextHopId(1));
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(RouteError::kUnknownNextHop, again.error());

  domain.Quiescent(kReader);
  EXPECT_EQ(0u, router->ReclaimRetired());
  EXPECT_TRUE(router->SetNextHop(NextHopId(1), Hop(3, 3)))
      << "reusable once readers have passed";
  EXPECT_EQ(2u, router->next_hop_count());
  domain.Offline(kReader);
  domain.Unregister(kReader);
}

std::unique_ptr<Router> MakeGroupRouter(size_t next_hops = 16, size_t groups = 4) {
  LpmRouteTable::Config config;
  config.max_routes = 1024;
  config.tbl8_groups = 64;
  auto router = Router::Create("router_groups", config, next_hops,
                               bess::runtime::runtime().rcu(), 1, groups);
  EXPECT_TRUE(router.has_value()) << RouteErrorName(router.error());
  return std::move(router).value();
}

// A group route spreads flows over its members by hash, keeps a flow on one
// member, and pins the members (D-065).
TEST(RouterGroupTest, FlowsSpreadOverMembersAndStay) {
  auto router = MakeGroupRouter();
  for (uint32_t i = 1; i <= 4; i++) {
    ASSERT_TRUE(router->SetNextHop(NextHopId(i), Hop(i, static_cast<uint8_t>(i))));
  }
  const NextHopId members[] = {NextHopId(1), NextHopId(2), NextHopId(3)};
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(1), members));
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                               NextHopGroupId(1)));
  ASSERT_TRUE(router->SetRoute(P(Ip(20, 0, 0, 0), 8), NextHopId(4)));
  EXPECT_EQ(1u, router->GroupReferences(NextHopGroupId(1)));
  EXPECT_EQ(1u, router->GroupMemberships(NextHopId(2)));
  EXPECT_EQ(0u, router->RouteReferences(NextHopId(2)));

  std::array<int, 5> count{};
  std::mt19937 rng(15);
  for (int i = 0; i < 30000; i++) {
    const uint32_t hash = rng();
    const NextHop *hop = router->Resolve(kDefaultRouteDomainId, Ip(10, 1, 2, 3), hash);
    ASSERT_NE(nullptr, hop);
    count[hop->egress.value()]++;
    // The same flow, the same member; LookupRoute agrees.
    ASSERT_EQ(hop, router->Resolve(kDefaultRouteDomainId, Ip(10, 9, 9, 9), hash));
    ASSERT_EQ(hop->egress.value(),
              router->LookupRoute(kDefaultRouteDomainId, Ip(10, 1, 2, 3), hash).value());
  }
  for (int m = 1; m <= 3; m++) {
    EXPECT_NEAR(10000, count[m], 600) << "member " << m;
  }
  EXPECT_EQ(0, count[4]);
  // Without a hash: the first member. A plain next-hop route ignores the hash.
  EXPECT_EQ(bess::dataplane::InterfaceId(1), router->Resolve(Ip(10, 1, 2, 3))->egress);
  EXPECT_EQ(bess::dataplane::InterfaceId(4),
            router->Resolve(kDefaultRouteDomainId, Ip(20, 1, 1, 1), 0xdeadbeef)->egress);

  // Pinned: a member and a group in use cannot go.
  EXPECT_EQ(RouteError::kNextHopInUse, router->RemoveNextHop(NextHopId(2)).error());
  EXPECT_EQ(RouteError::kNextHopInUse,
            router->RemoveNextHopGroup(NextHopGroupId(1)).error());
}

// Membership and neighbor changes are object updates: no route changes.
TEST(RouterGroupTest, MembershipAndNeighborChangesNeedNoRouteChange) {
  auto router = MakeGroupRouter();
  for (uint32_t i = 1; i <= 3; i++) {
    ASSERT_TRUE(router->SetNextHop(NextHopId(i), Hop(i, static_cast<uint8_t>(i))));
  }
  const NextHopId three[] = {NextHopId(1), NextHopId(2), NextHopId(3)};
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(2), three));
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P(0, 0), NextHopGroupId(2)));
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8),
                               NextHopGroupId(2)));

  const NextHopId two[] = {NextHopId(1), NextHopId(2)};
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(2), two));
  EXPECT_EQ(2u, router->route_count());
  EXPECT_EQ(0u, router->GroupMemberships(NextHopId(3)));
  for (uint32_t h = 0; h < 4096; h++) {
    const uint32_t hash = h * 0x9e3779b9u;
    ASSERT_NE(bess::dataplane::InterfaceId(3),
              router->Resolve(kDefaultRouteDomainId, Ip(10, 0, 0, 1), hash)->egress);
  }
  ASSERT_TRUE(router->RemoveNextHop(NextHopId(3))) << "no longer a member";

  // A neighbor update of a member reaches every group route at once.
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1, 0x77)));
  EXPECT_EQ(0x77, router->Resolve(Ip(10, 0, 0, 1))->dst_mac.bytes[5]);
  EXPECT_EQ(0x77, router->Resolve(Ip(99, 0, 0, 1))->dst_mac.bytes[5]);

  // Replacing a domain's routes releases the group references they held.
  EXPECT_EQ(2u, router->GroupReferences(NextHopGroupId(2)));
  ASSERT_TRUE(router->ReplaceRouteSetAtomic(kDefaultRouteDomainId,
                                            {{P(Ip(30, 0, 0, 0), 8), NextHopId(2)}}));
  EXPECT_EQ(0u, router->GroupReferences(NextHopGroupId(2)));
  EXPECT_EQ(nullptr, router->Resolve(Ip(10, 0, 0, 1)));
  // Re-pointing a route from a group to a next hop releases the group too.
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P(Ip(40, 0, 0, 0), 8),
                               NextHopGroupId(2)));
  ASSERT_TRUE(router->SetRoute(P(Ip(40, 0, 0, 0), 8), NextHopId(1)));
  EXPECT_EQ(0u, router->GroupReferences(NextHopGroupId(2)));
  ASSERT_TRUE(router->RemoveNextHopGroup(NextHopGroupId(2)));
}

TEST(RouterGroupTest, BatchWithHashesMatchesScalar) {
  auto router = MakeGroupRouter();
  for (uint32_t i = 1; i <= 8; i++) {
    ASSERT_TRUE(router->SetNextHop(NextHopId(i), Hop(i, static_cast<uint8_t>(i))));
  }
  const NextHopId a[] = {NextHopId(1), NextHopId(2), NextHopId(3), NextHopId(4)};
  const NextHopId b[] = {NextHopId(5), NextHopId(6)};
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(1), a));
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(4), b));
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8), NextHopGroupId(1)));
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P(Ip(20, 0, 0, 0), 8), NextHopGroupId(4)));
  ASSERT_TRUE(router->SetRoute(P(Ip(30, 0, 0, 0), 8), NextHopId(8)));
  std::mt19937 rng(16);
  for (int round = 0; round < 50; round++) {
    std::array<uint32_t, 32> dst{}, hashes{};
    for (size_t i = 0; i < dst.size(); i++) {
      dst[i] = Ip(10 + 10 * (rng() % 4), rng() % 256, 1, 1);  // 40/8 misses
      hashes[i] = rng();
    }
    std::array<const NextHop *, 32> hops{};
    const uint64_t mask = router->ResolveBatch(kDefaultRouteDomainId, dst, hashes, hops);
    for (size_t i = 0; i < dst.size(); i++) {
      const NextHop *want = router->Resolve(kDefaultRouteDomainId, dst[i], hashes[i]);
      ASSERT_EQ(want != nullptr, (mask >> i & 1) != 0);
      if (want != nullptr) {
        ASSERT_EQ(want, hops[i]);
      }
    }
  }
}

TEST(RouterGroupTest, InvalidGroupsAreRefused) {
  auto router = MakeGroupRouter(/*next_hops=*/8, /*groups=*/2);
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1, 1)));
  const NextHopId one[] = {NextHopId(1)};
  const NextHopId missing[] = {NextHopId(1), NextHopId(2)};
  const NextHopId out_of_range[] = {NextHopId(9)};
  std::vector<NextHopId> too_many(NextHopGroup::kMaxMembers + 1, NextHopId(1));
  EXPECT_EQ(RouteError::kInvalidId, router->SetNextHopGroup(NextHopGroupId(0), one).error());
  EXPECT_EQ(RouteError::kInvalidId, router->SetNextHopGroup(NextHopGroupId(3), one).error());
  EXPECT_EQ(RouteError::kInvalidId, router->SetNextHopGroup(NextHopGroupId(1), {}).error());
  EXPECT_EQ(RouteError::kInvalidId,
            router->SetNextHopGroup(NextHopGroupId(1), too_many).error());
  EXPECT_EQ(RouteError::kUnknownNextHop,
            router->SetNextHopGroup(NextHopGroupId(1), missing).error());
  EXPECT_EQ(RouteError::kInvalidId,
            router->SetNextHopGroup(NextHopGroupId(1), out_of_range).error());
  EXPECT_EQ(0u, router->GroupMemberships(NextHopId(1))) << "a refused group pins nothing";
  EXPECT_EQ(RouteError::kUnknownNextHop,
            router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8), NextHopGroupId(1))
                .error());
  EXPECT_EQ(RouteError::kInvalidId,
            router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8), NextHopGroupId(3))
                .error());
  // A router without groups refuses them; route values must fit 24 bits.
  auto plain = MakeRouter(8);
  EXPECT_EQ(RouteError::kInvalidId, plain->SetNextHopGroup(NextHopGroupId(1), one).error());
  LpmRouteTable::Config config;
  EXPECT_EQ(RouteError::kValueOutOfRange,
            Router::Create("too_wide", config, LpmRouteTable::kMaxValue - 1,
                           bess::runtime::runtime().rcu(), 1, 2)
                .error());
  // Kept: an exactly fitting router.
  EXPECT_TRUE(Router::Create("fits", config, LpmRouteTable::kMaxValue - 2,
                             bess::runtime::runtime().rcu(), 1, 2)
                  .has_value());
}

// Removing a group does not wait for readers; the id retires (cannot be
// reused or named by a route) and its members stay pinned until readers pass.
TEST(RouterGroupTest, GroupRemovalIsDeferredAndPinsMembers) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  auto router = MakeGroupRouter();
  ASSERT_TRUE(router->SetNextHop(NextHopId(1), Hop(1, 1)));
  const NextHopId one[] = {NextHopId(1)};
  ASSERT_TRUE(router->SetNextHopGroup(NextHopGroupId(1), one));
  ASSERT_TRUE(router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8), NextHopGroupId(1)));

  constexpr uint32_t kReader = 25;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);
  ASSERT_TRUE(router->RemoveRoute(P(Ip(10, 0, 0, 0), 8)));
  ASSERT_TRUE(router->RemoveNextHopGroup(NextHopGroupId(1)));
  EXPECT_EQ(0u, router->next_hop_group_count());
  EXPECT_EQ(1u, router->ReclaimRetired());
  EXPECT_NE(nullptr, router->LookupNextHopGroup(NextHopGroupId(1))) << "still readable";
  EXPECT_EQ(RouteError::kNextHopRetiring,
            router->SetNextHopGroup(NextHopGroupId(1), one).error());
  EXPECT_EQ(RouteError::kUnknownNextHop,
            router->SetRoute(kDefaultRouteDomainId, P(Ip(10, 0, 0, 0), 8), NextHopGroupId(1))
                .error());
  EXPECT_EQ(RouteError::kNextHopInUse, router->RemoveNextHop(NextHopId(1)).error())
      << "a retiring group's members stay pinned";

  domain.Quiescent(kReader);
  EXPECT_EQ(0u, router->ReclaimRetired());
  EXPECT_EQ(nullptr, router->LookupNextHopGroup(NextHopGroupId(1)));
  EXPECT_EQ(0u, router->GroupMemberships(NextHopId(1)));
  EXPECT_TRUE(router->RemoveNextHop(NextHopId(1)));
  domain.Offline(kReader);
  domain.Unregister(kReader);
}

TEST(RouterTest, RewriteL2) {
  PlainPacketPool pool(8, -1, 128);
  PacketHandle pkt = pool.Alloc(64);
  ASSERT_NE(nullptr, pkt);
  PacketRef ref(pkt);
  NextHop hop = Hop(1, 0x42);
  hop.dst_mac.bytes[0] = 0x02;
  ASSERT_TRUE(RewriteL2(ref, hop));
  const auto *eth = ref.head_data<utils::Ethernet *>();
  EXPECT_EQ(hop.dst_mac, eth->dst_addr);
  EXPECT_EQ(hop.src_mac, eth->src_addr);

  rte_mbuf_refcnt_update(pkt, 1);  // now shared
  EXPECT_EQ(packet::MutationError::kSharedStorage, RewriteL2(ref, hop).error());
  rte_mbuf_refcnt_update(pkt, -1);
  PacketFree(pkt);
}

}  // namespace
}  // namespace bess::route
