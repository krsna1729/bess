// SPDX-License-Identifier: BSD-3-Clause

// M6: route domains inside the one Router -- the lookup matrix the roadmap
// requires. All rows go through Router::Resolve*/LookupRoute with no Module,
// metadata or gate.
//
//   DomainSweep      one lookup per call, rotating over 1/4/16/64 domains
//                    (arg 0 = Resolve, 1 = LookupRoute, the id-only call the
//                    pre-M6 MultiDomainRouter::Lookup measured at 1.69-2.41
//                    ns/op in the same sweep)
//   HotDomain        one lookup, always the same domain (0 = domain 1 by id,
//                    1 = the default-domain overload)
//   BatchX4          4-wide batch inside one domain, rotating 1/4/16/64
//                    (pre-M6 LookupBatchX4)
//   Batch32          32-wide batch inside one domain, rotating 1/4/16/64
//   Fib              32-wide batches over a small (1K) or large (64K) FIB in
//                    each of 4 domains
//   DuringUpdates    32-wide batches in domain 1 while another thread updates
//                    the router (0 = in-place SetRoute/RemoveRoute churn in
//                    domain 1 itself, 1 = ReplaceRouteSetAtomic in domain 2)
//   DomainLifecycle  CreateDomain + RemoveDomain
//
// Lookups per second are what the packet path pays; the lifecycle row is the
// control path.

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "route/route_domain.h"
#include "route/router.h"
#include "runtime/runtime_state.h"

namespace {

using bess::route::Ipv4Prefix;
using bess::route::NeighborState;
using bess::route::NextHop;
using bess::route::NextHopId;
using bess::route::RouteDomainId;
using bess::route::Router;

constexpr uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a << 24 | b << 16 | c << 8 | d;
}

Ipv4Prefix P(uint32_t addr, uint8_t len) {
  return Ipv4Prefix::Make(addr, len).value();
}

// Tests and benchmarks bring the EAL up with a 512 MB no-hugepage heap; 16
// real domains (1 GB of tbl24) need more. Ask for 2600 MB unless the caller
// chose a size (BESS_DPDK_NOHUGE_MB). Untouched anonymous pages cost no RAM.
[[maybe_unused]] const int kHeapRequest =
    setenv("BESS_DPDK_NOHUGE_MB", "2600", 0);

constexpr uint32_t kMaxSweep = 64;
constexpr size_t kBatch = 32;

Router::Config ConfigFor(size_t routes) {
  Router::Config config;
  config.max_routes = static_cast<uint32_t>(routes * 2 + 16);
  config.tbl8_groups = 256;
  return config;
}

void AddNextHops(Router &router, uint32_t n) {
  for (uint32_t id = 1; id <= n; id++) {
    NextHop hop;
    hop.egress = bess::dataplane::InterfaceId(static_cast<uint32_t>(id % 8));
    hop.neighbor = NeighborState::kResolved;
    (void)router.SetNextHop(NextHopId(id), hop);
  }
}

// Domains 1..64 (and the default), 10/8 in each, domain d via next hop d+1.
// Built once; every sweep row uses a prefix of it. Each domain is a real
// rte_lpm (64 MB tbl24), so how many exist depends on the EAL heap:
// RealDomains() says how many were created, and a row that needs more is
// skipped with a message rather than measuring misses on missing domains.
uint32_t g_real_domains = 0;
Router &SweepRouter() {
  static std::unique_ptr<Router> router = [] {
    auto r = Router::Create("dom_sweep", ConfigFor(16), 128,
                            bess::runtime::runtime().rcu(), kMaxSweep + 1)
                 .value();
    AddNextHops(*r, 128);
    (void)r->SetRoute(P(Ip(10, 0, 0, 0), 8), NextHopId(1));
    // 16 real domains by default (1 GB of tbl24). BESS_DOMAIN_BENCH_MAX=64
    // asks for 64 (4 GB: also set BESS_DPDK_NOHUGE_MB above that).
    const char *max_env = std::getenv("BESS_DOMAIN_BENCH_MAX");
    const uint32_t max_real =
        max_env != nullptr ? static_cast<uint32_t>(std::atoi(max_env)) : 16;
    for (uint32_t d = 1; d <= kMaxSweep && d <= max_real; d++) {
      if (!r->CreateDomain(RouteDomainId(d), ConfigFor(16))) {
        break;
      }
      (void)r->SetRoute(RouteDomainId(d), P(Ip(10, 0, 0, 0), 8),
                        NextHopId(d + 1));
      g_real_domains = d;
    }
    return r;
  }();
  return *router;
}

bool HaveDomains(benchmark::State &state, uint32_t domains) {
  SweepRouter();
  if (domains > g_real_domains) {
    state.SkipWithMessage("not enough EAL memory for this many real domains");
    return false;
  }
  return true;
}

void BM_DomainSweep(benchmark::State &state) {
  Router &router = SweepRouter();
  const uint32_t domains = static_cast<uint32_t>(state.range(0));
  if (!HaveDomains(state, domains)) {
    return;
  }
  const bool id_only = state.range(1) == 1;
  const uint32_t ip = Ip(10, 1, 2, 3);
  uint32_t d = 1;
  uint64_t sink = 0;
  for (auto _ : state) {
    const RouteDomainId domain(d);
    if (id_only) {
      sink += router.LookupRoute(domain, ip).value();
    } else {
      sink += router.Resolve(domain, ip)->egress.value();
    }
    if (++d > domains) {
      d = 1;
    }
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations());
  state.SetLabel(id_only ? "LookupRoute" : "Resolve");
}
BENCHMARK(BM_DomainSweep)->ArgsProduct({{1, 4, 16, 64}, {0, 1}});

void BM_HotDomain(benchmark::State &state) {
  Router &router = SweepRouter();
  const bool by_overload = state.range(0) == 1;
  const uint32_t ip = Ip(10, 1, 2, 3);
  uint64_t sink = 0;
  for (auto _ : state) {
    const NextHop *hop = by_overload ? router.Resolve(ip)
                                     : router.Resolve(RouteDomainId(1), ip);
    sink += hop->egress.value();
    benchmark::ClobberMemory();
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations());
  state.SetLabel(by_overload ? "default domain overload" : "domain by id");
}
BENCHMARK(BM_HotDomain)->Arg(0)->Arg(1);

void BM_BatchX4(benchmark::State &state) {
  Router &router = SweepRouter();
  const uint32_t domains = static_cast<uint32_t>(state.range(0));
  if (!HaveDomains(state, domains)) {
    return;
  }
  const uint32_t ips[4] = {Ip(10, 1, 1, 1), Ip(10, 2, 2, 2), Ip(10, 3, 3, 3),
                           Ip(10, 4, 4, 4)};
  const NextHop *hops[4];
  uint32_t d = 1;
  uint64_t sink = 0;
  for (auto _ : state) {
    sink += router.ResolveBatch(RouteDomainId(d), ips, hops);
    benchmark::DoNotOptimize(hops);
    if (++d > domains) {
      d = 1;
    }
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations() * 4);
}
BENCHMARK(BM_BatchX4)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

void BM_Batch32(benchmark::State &state) {
  Router &router = SweepRouter();
  const uint32_t domains = static_cast<uint32_t>(state.range(0));
  if (!HaveDomains(state, domains)) {
    return;
  }
  uint32_t ips[kBatch];
  for (size_t i = 0; i < kBatch; i++) {
    ips[i] = Ip(10, static_cast<uint32_t>(i), static_cast<uint32_t>(i * 7), 1);
  }
  const NextHop *hops[kBatch];
  uint32_t d = 1;
  uint64_t sink = 0;
  for (auto _ : state) {
    sink += router.ResolveBatch(RouteDomainId(d), ips, hops);
    benchmark::DoNotOptimize(hops);
    if (++d > domains) {
      d = 1;
    }
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_Batch32)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

// n /24s under 10.0.0.0/8, route i -> 10.(i>>8).(i&255).0/24, with a default.
std::vector<uint32_t> LoadFib(Router &router, RouteDomainId domain, size_t n) {
  for (size_t i = 0; i < n; i++) {
    (void)router.SetRoute(domain,
                          P(Ip(10, static_cast<uint32_t>(i >> 8),
                             static_cast<uint32_t>(i & 255), 0),
                            24),
                          NextHopId(1 + i % 100));
  }
  std::mt19937 rng(0x6d36);
  std::vector<uint32_t> keys(1 << 15);
  for (auto &k : keys) {
    const size_t i = rng() % n;
    k = Ip(10, static_cast<uint32_t>(i >> 8), static_cast<uint32_t>(i & 255),
           static_cast<uint32_t>(rng() & 255));
  }
  return keys;
}

struct FibRig {
  std::unique_ptr<Router> router;
  std::vector<uint32_t> keys;
};

FibRig &FibFor(size_t n) {
  static std::vector<std::pair<size_t, FibRig>> rigs;
  for (auto &[size, rig] : rigs) {
    if (size == n) {
      return rig;
    }
  }
  FibRig rig;
  rig.router = Router::Create("dom_fib", ConfigFor(n), 128,
                              bess::runtime::runtime().rcu(), 4)
                   .value();
  AddNextHops(*rig.router, 128);
  for (uint32_t d = 0; d < 4; d++) {
    if (d != 0) {
      (void)rig.router->CreateDomain(RouteDomainId(d), ConfigFor(n));
    }
    rig.keys = LoadFib(*rig.router, RouteDomainId(d), n);
  }
  rigs.emplace_back(n, std::move(rig));
  return rigs.back().second;
}

void BM_Fib(benchmark::State &state) {
  FibRig &rig = FibFor(static_cast<size_t>(state.range(0)));
  const NextHop *hops[kBatch];
  size_t offset = 0;
  uint32_t d = 0;
  uint64_t sink = 0;
  for (auto _ : state) {
    sink += rig.router->ResolveBatch(
        RouteDomainId(d), std::span(&rig.keys[offset], kBatch), hops);
    benchmark::DoNotOptimize(hops);
    offset = (offset + kBatch) % rig.keys.size();
    d = (d + 1) & 3;
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_Fib)->Arg(1024)->Arg(65536);

void BM_DuringUpdates(benchmark::State &state) {
  const bool replace = state.range(0) == 1;
  bess::rcu::RcuDomain &rcu = bess::runtime::runtime().rcu();
  auto router = Router::Create("dom_upd", ConfigFor(1024), 128, rcu, 3).value();
  AddNextHops(*router, 128);
  const RouteDomainId d1(1), d2(2);
  (void)router->CreateDomain(d1, ConfigFor(1024));
  (void)router->CreateDomain(d2, ConfigFor(1024));
  const std::vector<uint32_t> keys = LoadFib(*router, d1, 1024);
  bess::route::RouteSet set;
  for (uint32_t i = 0; i < 256; i++) {
    set.push_back({P(Ip(10, 0, i, 0), 24), NextHopId(1 + i % 100)});
  }

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> updates{0};
  std::thread writer([&] {
    uint32_t n = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (replace) {
        (void)router->ReplaceRouteSetAtomic(d2, set);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      } else {
        const Ipv4Prefix p = P(Ip(198, 51, n & 255, 0), 24);
        (void)router->SetRoute(d1, p, NextHopId(1 + n % 100));
        (void)router->RemoveRoute(d1, p);
        n++;
      }
      updates.fetch_add(1, std::memory_order_relaxed);
    }
  });

  const NextHop *hops[kBatch];
  size_t offset = 0;
  uint64_t sink = 0;
  for (auto _ : state) {
    sink += router->ResolveBatch(d1, std::span(&keys[offset], kBatch), hops);
    benchmark::DoNotOptimize(hops);
    offset = (offset + kBatch) % keys.size();
  }
  stop = true;
  writer.join();
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations() * kBatch);
  state.counters["updates"] = static_cast<double>(updates.load());
  state.SetLabel(replace ? "ReplaceRouteSetAtomic in another domain"
                         : "SetRoute/RemoveRoute in this domain");
}
BENCHMARK(BM_DuringUpdates)->Arg(0)->Arg(1);

void BM_DomainLifecycle(benchmark::State &state) {
  auto router = Router::Create("dom_life", ConfigFor(16), 8,
                               bess::runtime::runtime().rcu(), 2)
                    .value();
  for (auto _ : state) {
    if (!router->CreateDomain(RouteDomainId(1), ConfigFor(16)) ||
        !router->RemoveDomain(RouteDomainId(1))) {
      state.SkipWithError("lifecycle failed");
      break;
    }
  }
}
BENCHMARK(BM_DomainLifecycle)->Unit(benchmark::kMillisecond);

}  // namespace
