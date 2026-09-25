// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

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

#include "control/runtime_state.h"
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
                                         control::runtime().rcu());
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
  rcu::RcuDomain &domain = control::runtime().rcu();
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

  std::mt19937_64 rng(0x6b3a);
  uint64_t adds = 0, full = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto &[p, v] = churn[rng() % churn.size()];
    if (table->Find(p)) {
      ASSERT_TRUE(table->Erase(p));
    } else {
      auto added = table->Upsert(p, Value(v));
      // Adds can transiently find no free group while freed ones wait out a
      // reader grace period; that is the defer queue working.
      ASSERT_TRUE(added || added.error() == RouteError::kTableFull);
      added ? adds++ : full++;
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
  EXPECT_GT(adds, 1000u);
}

// -- Router -----------------------------------------------------------------------

NextHop Hop(gate_idx_t egress, uint8_t mac_tail,
            NeighborState state = NeighborState::kResolved) {
  NextHop hop;
  hop.egress = egress;
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
                               control::runtime().rcu());
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
  EXPECT_EQ(4, hop->egress);
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
  EXPECT_EQ(1, hops[0]->egress);
  EXPECT_EQ(nullptr, hops[1]);
  EXPECT_EQ(2, hops[2]->egress);
  EXPECT_EQ(2, hops[4]->egress);
}

// Readers resolving continuously while the writer adds next hops, points
// routes at them, removes the routes, and removes the next hops: every key is
// under the stable background route, so every batch must resolve every key --
// to the background hop or to whichever churn hop its more-specific route
// named. A reader that found a route whose next hop had already been removed
// would come back with the bit cleared; RemoveNextHop's grace period is what
// prevents that.
TEST(RouterTest, ConcurrentChurnNeverLosesANextHop) {
  rcu::RcuDomain &domain = control::runtime().rcu();
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

  uint64_t rounds = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    const uint32_t x = static_cast<uint32_t>(rounds++ % 32);
    const NextHopId id(2 + x);
    const Ipv4Prefix p = P(Ip(10, x, 0, 0), 16);
    ASSERT_TRUE(router->SetNextHop(id, Hop(static_cast<gate_idx_t>(2 + x), 2)));
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
                             << rounds << " rounds";
  EXPECT_GT(rounds, 100u);
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
