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

// K7: route lookup, route update and next-hop update costs.
//
//   lookup  -- 32-address batches over 1K/16K/64K-route tables; second arg
//              0 = a 32K-key stream from inside the routes (plus 1/8 random),
//              1 = 4M uniformly random keys (DRAM-bound tbl24):
//                RawLpmX4       the pre-K7 IPLookup shape: SSE byte swap of
//                               packet-order addresses + rte_lpm_lookupx4
//                RouteTable     host-order gather + LookupBatch (what
//                               IPLookup now runs)
//                Router         LookupBatch + next-hop resolution
//   update  -- one route change applied in place (K7) versus rebuilding the
//              whole rte_lpm for it (the pre-K7 IPLookup behaviour), at each
//              table size.
//   next hop -- SetNextHop (a neighbor update: republish the next-hop table)
//              at 1K and 64K next hops.
//
// Route sets follow entry 34's shape: mostly /24, some /8-/23, and /25-/32
// nested under earlier /24s, in generator (arbitrary) order. Tables above 64K
// are not registered: rte_lpm's build is quadratic (~21 s at 512K) and the
// suite's smoke run builds every row.

#include <benchmark/benchmark.h>

#include <immintrin.h>

#include <map>
#include <memory>
#include <string>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "control/runtime_state.h"
#include "dpdk.h"
#include "route/route_table.h"
#include "route/router.h"

namespace {

using bess::route::Ipv4Prefix;
using bess::route::LpmRouteTable;
using bess::route::NextHop;
using bess::route::NextHopId;
using bess::route::NeighborState;
using bess::route::Router;

constexpr size_t kBatch = 32;
constexpr size_t kKeys = 32768;

struct Route {
  Ipv4Prefix prefix;
  uint32_t value;
};

std::vector<Route> Routes(size_t n) {
  std::mt19937_64 rng(0x6b37);
  std::vector<Route> routes;
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
      len = static_cast<uint8_t>(8 + rng() % 16);
      addr = r & (~uint32_t{0} << (32 - len));
    } else {
      len = static_cast<uint8_t>(25 + rng() % 8);
      const uint32_t base = slash24s[rng() % slash24s.size()];
      addr = (base | (r & 0xff)) & (~uint32_t{0} << (32 - len));
    }
    const Ipv4Prefix p = Ipv4Prefix::Make(addr, len).value();
    if (!seen.insert(p).second) {
      continue;
    }
    if (len == 24) {
      slash24s.push_back(addr);
    }
    // Values fit both a next-hop id space of 1024 and a gate.
    routes.push_back({p, 1 + static_cast<uint32_t>(rng() % 1023)});
  }
  return routes;
}

std::vector<uint32_t> Keys(const std::vector<Route> &routes) {
  std::mt19937_64 rng(0x6b38);
  std::vector<uint32_t> keys(kKeys);
  for (auto &k : keys) {
    if (rng() % 8 == 0) {
      k = static_cast<uint32_t>(rng());
      continue;
    }
    const Ipv4Prefix &p = routes[rng() % routes.size()].prefix;
    const uint32_t host = p.length() == 32 ? 0
                                           : static_cast<uint32_t>(rng()) &
                                                 (~uint32_t{0} >> p.length());
    k = p.addr() | host;
  }
  return keys;
}

LpmRouteTable::Config ConfigFor(size_t routes) {
  LpmRouteTable::Config config;
  config.max_routes = static_cast<uint32_t>(routes * 2);
  config.tbl8_groups = static_cast<uint32_t>(routes / 4 + 256);
  return config;
}

struct Fixture {
  std::unique_ptr<LpmRouteTable> table;
  std::vector<Route> routes;
  std::vector<uint32_t> keys;
};

// Built once per size and kept: rte_lpm builds are the expensive part.
Fixture &TableFor(size_t n) {
  static std::map<size_t, Fixture> fixtures;
  Fixture &f = fixtures[n];
  if (f.table == nullptr) {
    f.routes = Routes(n);
    f.keys = Keys(f.routes);
    f.table = LpmRouteTable::Create("route_bench", ConfigFor(n),
                                    bess::control::runtime().rcu())
                  .value();
    for (const Route &r : f.routes) {
      (void)f.table->Upsert(r.prefix, r.value);
    }
  }
  return f;
}

// Key stream for a row: route-derived (cache-resident tbl24 footprint) or 4M
// uniformly random addresses (DRAM-bound; see BM_LookupBody).
std::vector<uint32_t> StreamFor(const Fixture &f, bool uniform) {
  if (!uniform) {
    return f.keys;
  }
  std::mt19937_64 rng(0x6b3b);
  std::vector<uint32_t> keys(size_t{1} << 22);
  for (auto &k : keys) {
    k = static_cast<uint32_t>(rng());
  }
  return keys;
}

// -- lookup ------------------------------------------------------------------------

// The pre-K7 IPLookup inner loop, on the same table: addresses as they sit in
// packets (network order), swapped four at a time, rte_lpm_lookupx4.
void BM_LookupRawLpmX4(benchmark::State &state) {
  Fixture &f = TableFor(static_cast<size_t>(state.range(0)));
  const std::vector<uint32_t> keys = StreamFor(f, state.range(1) != 0);
  std::vector<uint32_t> wire(keys.size());
  for (size_t i = 0; i < wire.size(); i++) {
    wire[i] = __builtin_bswap32(keys[i]);
  }
  // A plain rte_lpm with the same rules, as IPLookup used to own.
  struct rte_lpm_config conf = {};
  conf.max_rules = ConfigFor(f.routes.size()).max_routes;
  conf.number_tbl8s = ConfigFor(f.routes.size()).tbl8_groups;
  static int n = 0;
  const std::string name = "raw_lpm_" + std::to_string(n++);
  struct rte_lpm *lpm = rte_lpm_create(name.c_str(), SOCKET_ID_ANY, &conf);
  for (const Route &r : f.routes) {
    rte_lpm_add(lpm, r.prefix.addr(), r.prefix.length(), r.value);
  }
  const __m128i bswap =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  size_t offset = 0;
  uint32_t hops[kBatch];
  for (auto _ : state) {
    for (size_t i = 0; i < kBatch; i += 4) {
      __m128i ips = _mm_loadu_si128(
          reinterpret_cast<const __m128i *>(&wire[offset + i]));
      ips = _mm_shuffle_epi8(ips, bswap);
      rte_lpm_lookupx4(lpm, ips, &hops[i], 0);
    }
    benchmark::DoNotOptimize(hops);
    offset = (offset + kBatch) % wire.size();
  }
  rte_lpm_free(lpm);
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_LookupRawLpmX4)->ArgsProduct({{1024, 16384, 65536}, {0, 1}});

void BM_LookupRouteTable(benchmark::State &state) {
  Fixture &f = TableFor(static_cast<size_t>(state.range(0)));
  const std::vector<uint32_t> keys = StreamFor(f, state.range(1) != 0);
  std::vector<uint32_t> wire(keys.size());
  for (size_t i = 0; i < wire.size(); i++) {
    wire[i] = __builtin_bswap32(keys[i]);
  }
  size_t offset = 0;
  uint32_t dst[kBatch];
  uint32_t values[kBatch];
  for (auto _ : state) {
    const auto view = f.table->Read();
    for (size_t i = 0; i < kBatch; i++) {
      dst[i] = __builtin_bswap32(wire[offset + i]);  // IPLookup's gather
    }
    benchmark::DoNotOptimize(view.LookupBatch(dst, values));
    benchmark::DoNotOptimize(values);
    offset = (offset + kBatch) % wire.size();
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_LookupRouteTable)->ArgsProduct({{1024, 16384, 65536}, {0, 1}});

std::unique_ptr<Router> RouterFor(size_t n, std::vector<uint32_t> *keys) {
  auto router = Router::Create("route_bench_rt", ConfigFor(n), 1024,
                               bess::control::runtime().rcu())
                    .value();
  for (uint32_t id = 1; id <= 1023; id++) {
    NextHop hop;
    hop.egress = static_cast<bess::gate_idx_t>(id % 8);
    hop.neighbor = NeighborState::kResolved;
    (void)router->SetNextHop(NextHopId(id), hop);
  }
  const std::vector<Route> routes = Routes(n);
  for (const Route &r : routes) {
    (void)router->SetRoute(r.prefix, NextHopId(r.value));
  }
  *keys = Keys(routes);
  return router;
}

void BM_LookupRouter(benchmark::State &state) {
  std::vector<uint32_t> keys;
  auto router = RouterFor(static_cast<size_t>(state.range(0)), &keys);
  size_t offset = 0;
  const NextHop *hops[kBatch];
  uint64_t egress = 0;
  for (auto _ : state) {
    const uint64_t mask =
        router->ResolveBatch(std::span(&keys[offset], kBatch), hops);
    for (uint64_t m = mask; m; m &= m - 1) {
      egress += hops[__builtin_ctzll(m)]->egress;
    }
    offset = (offset + kBatch) % keys.size();
  }
  benchmark::DoNotOptimize(egress);
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_LookupRouter)->Arg(1024)->Arg(16384)->Arg(65536);

// -- lookup body study (K4.5 method applied to rte_lpm) ------------------------------
//
// K4.5 compared batch bodies on one fixture: scalar vs ILP4 interleaving, and
// a prefetch pipeline. The rte_lpm analogues, on one raw table and one key
// stream, host-order addresses, 32 per batch:
//
//   0 x4           rte_lpm_lookupx4 (DPDK's 4-wide body; LookupBatch today)
//   1 scalar       rte_lpm_lookup per address
//   2 bulk         rte_lpm_lookup_bulk
//   3 pf+x4        prefetch every address's tbl24 line first, then x4
//   4 pf+scalar    prefetch every tbl24 line first, then scalar
//
// Key streams: 0 = a 32K-key stream inside the routes (7/8) plus random (1/8),
// whose tbl24 footprint is cache-resident; 1 = 4M uniformly random addresses,
// so nearly every lookup touches a tbl24 line not seen recently (a
// DRAM-bound stream over the 64 MiB tbl24, K4.5c's working-set point).

enum Body : int { kX4, kScalar, kBulk, kPrefetchX4, kPrefetchScalar };

void BM_LookupBody(benchmark::State &state) {
  const auto body = static_cast<Body>(state.range(0));
  const size_t n = static_cast<size_t>(state.range(1));
  const bool uniform = state.range(2) != 0;
  Fixture &f = TableFor(n);

  struct rte_lpm_config conf = {};
  conf.max_rules = ConfigFor(n).max_routes;
  conf.number_tbl8s = ConfigFor(n).tbl8_groups;
  static int seq = 0;
  const std::string name = "body_lpm_" + std::to_string(seq++);
  struct rte_lpm *lpm = rte_lpm_create(name.c_str(), SOCKET_ID_ANY, &conf);
  for (const Route &r : f.routes) {
    rte_lpm_add(lpm, r.prefix.addr(), r.prefix.length(), r.value);
  }
  std::vector<uint32_t> keys = f.keys;
  if (uniform) {
    // 4M keys: the stream itself is read sequentially (hardware prefetch
    // covers it), but it touches ~4M distinct tbl24 lines, far beyond L3.
    // A 32K-key stream would touch only ~2 MiB of tbl24 and stay cached --
    // the sampling mistake K2.6 and K4.5c warned about.
    std::mt19937_64 rng(0x6b3b);
    keys.resize(size_t{1} << 22);
    for (auto &k : keys) {
      k = static_cast<uint32_t>(rng());
    }
  }

  size_t offset = 0;
  uint32_t hops[kBatch];
  uint64_t sink = 0;
  for (auto _ : state) {
    const uint32_t *ips = &keys[offset];
    if (body == kPrefetchX4 || body == kPrefetchScalar) {
      for (size_t i = 0; i < kBatch; i++) {
        __builtin_prefetch(&lpm->tbl24[ips[i] >> 8], 0, 3);
      }
    }
    switch (body) {
      case kX4:
      case kPrefetchX4:
        for (size_t i = 0; i < kBatch; i += 4) {
          const __m128i v =
              _mm_loadu_si128(reinterpret_cast<const __m128i *>(&ips[i]));
          rte_lpm_lookupx4(lpm, v, &hops[i], UINT32_MAX);
        }
        break;
      case kScalar:
      case kPrefetchScalar:
        for (size_t i = 0; i < kBatch; i++) {
          if (rte_lpm_lookup(lpm, ips[i], &hops[i]) != 0) {
            hops[i] = UINT32_MAX;
          }
        }
        break;
      case kBulk:
        rte_lpm_lookup_bulk(lpm, ips, hops, kBatch);
        break;
    }
    sink += hops[0] ^ hops[31];
    offset = (offset + kBatch) % keys.size();
  }
  benchmark::DoNotOptimize(sink);
  rte_lpm_free(lpm);
  state.SetItemsProcessed(state.iterations() * kBatch);
  static const char *const kNames[] = {"x4", "scalar", "bulk", "pf+x4",
                                       "pf+scalar"};
  state.SetLabel(std::string(kNames[body]) + (uniform ? " uniform" : " routes"));
}
BENCHMARK(BM_LookupBody)
    ->ArgsProduct({{kX4, kScalar, kBulk, kPrefetchX4, kPrefetchScalar},
                   {1024, 65536},
                   {0, 1}});

// -- update ------------------------------------------------------------------------

// One route change in place: add a /28 and delete it again (two changes per
// iteration, reported per change).
void BM_UpdateInPlace(benchmark::State &state) {
  Fixture &f = TableFor(static_cast<size_t>(state.range(0)));
  const Ipv4Prefix p = Ipv4Prefix::Make(0xc6336400, 28).value();  // 198.51.100/28
  for (auto _ : state) {
    (void)f.table->Upsert(p, 7);
    (void)f.table->Erase(p);
  }
  bess::control::runtime().rcu().ReclaimReady();
  state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_UpdateInPlace)->Arg(1024)->Arg(16384)->Arg(65536);

// What one route change cost before K7: a whole new rte_lpm with every rule.
void BM_UpdateByRebuild(benchmark::State &state) {
  if (!bess::IsDpdkInitialized()) {
    bess::InitDpdk();  // this row calls rte_lpm directly
  }
  const std::vector<Route> routes = Routes(static_cast<size_t>(state.range(0)));
  const LpmRouteTable::Config config = ConfigFor(routes.size());
  uint64_t n = 0;
  for (auto _ : state) {
    char name[32];
    snprintf(name, sizeof(name), "rebuild_%lu", static_cast<unsigned long>(n++));
    struct rte_lpm_config conf = {};
    conf.max_rules = config.max_routes;
    conf.number_tbl8s = config.tbl8_groups;
    struct rte_lpm *lpm = rte_lpm_create(name, SOCKET_ID_ANY, &conf);
    for (const Route &r : routes) {
      rte_lpm_add(lpm, r.prefix.addr(), r.prefix.length(), r.value);
    }
    rte_lpm_free(lpm);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_UpdateByRebuild)
    ->Arg(1024)
    ->Arg(16384)
    ->Arg(65536)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);

// A neighbor update: republish the next-hop table (routes untouched).
void BM_NextHopUpdate(benchmark::State &state) {
  const size_t hops = static_cast<size_t>(state.range(0));
  auto router = Router::Create("route_bench_nh", ConfigFor(1024), hops,
                               bess::control::runtime().rcu())
                    .value();
  NextHop hop;
  hop.neighbor = NeighborState::kResolved;
  for (size_t id = 1; id <= hops; id++) {
    (void)router->SetNextHop(NextHopId(static_cast<uint32_t>(id)), hop);
  }
  uint8_t mac = 0;
  for (auto _ : state) {
    hop.dst_mac.bytes[5] = mac++;
    (void)router->SetNextHop(NextHopId(1), hop);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NextHopUpdate)->Arg(1024)->Arg(65536)->Unit(benchmark::kMicrosecond);

}  // namespace
