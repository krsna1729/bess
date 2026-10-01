// SPDX-License-Identifier: BSD-3-Clause

#include "route/route_domain.h"

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

#include "dpdk.h"
#include "rcu/rcu_domain.h"

namespace {

using bess::route::Ipv4Prefix;
using bess::route::MultiDomainRouter;
using bess::route::NextHopId;
using bess::route::RouteDomainId;

uint32_t Ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (static_cast<uint32_t>(a) << 24) |
         (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(c) << 8) |
         static_cast<uint32_t>(d);
}

class RouteDomainFixture : public benchmark::Fixture {
 public:
  void SetUp(const ::benchmark::State &) override {
    bess::InitDpdk(0);
    rcu_domain_ = std::make_unique<bess::rcu::RcuDomain>(4);
    MultiDomainRouter::Config config{.max_routes = 1024, .tbl8_groups = 64};
    auto router = MultiDomainRouter::Create("bench_router", config, 64, *rcu_domain_);
    router_ = std::move(*router);

    // Populate domains (1..64) with overlapping subnets
    for (uint32_t d = 1; d <= 64; d++) {
      RouteDomainId domain{d};
      (void)router_->CreateDomain(domain, config);
      auto p = *Ipv4Prefix::Make(Ip(10, 0, 0, 0), 8);
      (void)router_->SetRoute(domain, p, NextHopId(d));
    }
  }

  void TearDown(const ::benchmark::State &) override {
    router_.reset();
    rcu_domain_.reset();
  }

 protected:
  std::unique_ptr<bess::rcu::RcuDomain> rcu_domain_;
  std::unique_ptr<MultiDomainRouter> router_;
};

BENCHMARK_DEFINE_F(RouteDomainFixture, BM_RouteDomainLookup)(benchmark::State &state) {
  const uint32_t num_domains = static_cast<uint32_t>(state.range(0));
  const uint32_t test_ip = Ip(10, 1, 2, 3);
  uint32_t d = 1;

  for (auto _ : state) {
    RouteDomainId domain{d};
    NextHopId hop = router_->Lookup(domain, test_ip);
    benchmark::DoNotOptimize(hop);

    if (++d > num_domains) {
      d = 1;
    }
  }

  state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(RouteDomainFixture, BM_RouteDomainLookup)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64);

BENCHMARK_DEFINE_F(RouteDomainFixture, BM_RouteDomainBatchX4)(benchmark::State &state) {
  const uint32_t num_domains = static_cast<uint32_t>(state.range(0));
  const uint32_t ips[4] = {
      Ip(10, 1, 1, 1),
      Ip(10, 2, 2, 2),
      Ip(10, 3, 3, 3),
      Ip(10, 4, 4, 4),
  };
  NextHopId hops[4];
  uint32_t d = 1;

  for (auto _ : state) {
    RouteDomainId domain{d};
    router_->LookupBatchX4(domain, ips, hops);
    benchmark::DoNotOptimize(hops);

    if (++d > num_domains) {
      d = 1;
    }
  }

  state.SetItemsProcessed(state.iterations() * 4);
}
BENCHMARK_REGISTER_F(RouteDomainFixture, BM_RouteDomainBatchX4)
    ->Arg(1)->Arg(4)->Arg(16)->Arg(64);

}  // namespace

BENCHMARK_MAIN();
