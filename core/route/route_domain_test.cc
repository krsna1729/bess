// SPDX-License-Identifier: BSD-3-Clause

#include "route/route_domain.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "rcu/rcu_domain.h"

namespace bess::route {
namespace {

uint32_t Ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (static_cast<uint32_t>(a) << 24) |
         (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(c) << 8) |
         static_cast<uint32_t>(d);
}

class RouteDomainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rcu_domain_ = std::make_unique<rcu::RcuDomain>(4);
    LpmRouteTable::Config config{.max_routes = 256, .tbl8_groups = 64};
    auto router = MultiDomainRouter::Create("test_router", config, 64, *rcu_domain_);
    ASSERT_TRUE(router.has_value());
    router_ = std::move(*router);
  }

  void TearDown() override {
    router_.reset();
    rcu_domain_.reset();
  }

  std::unique_ptr<rcu::RcuDomain> rcu_domain_;
  std::unique_ptr<MultiDomainRouter> router_;
};

TEST_F(RouteDomainTest, DefaultDomainExists) {
  EXPECT_TRUE(router_->HasDomain(kDefaultRouteDomainId));
  EXPECT_EQ(router_->domain_count(), 1u);
}

TEST_F(RouteDomainTest, OverlappingSubnetsInSeparateDomains) {
  // Domain 1: N3 (Access Network)
  // Domain 2: N6 (Data Network / Internet)
  const RouteDomainId n3_domain{1};
  const RouteDomainId n6_domain{2};

  LpmRouteTable::Config config{.max_routes = 256, .tbl8_groups = 64};
  ASSERT_TRUE(router_->CreateDomain(n3_domain, config).has_value());
  ASSERT_TRUE(router_->CreateDomain(n6_domain, config).has_value());
  EXPECT_EQ(router_->domain_count(), 3u);

  // Both domains configure the EXACT SAME overlapping subnet 10.0.0.0/8!
  // N3 routes 10.0.0.0/8 to NextHop 1 (e.g. gNodeB tunnel gateway)
  // N6 routes 10.0.0.0/8 to NextHop 2 (e.g. DN egress router)
  auto p = *Ipv4Prefix::Make(Ip(10, 0, 0, 0), 8);
  ASSERT_TRUE(router_->SetRoute(n3_domain, p, NextHopId(1)).has_value());
  ASSERT_TRUE(router_->SetRoute(n6_domain, p, NextHopId(2)).has_value());

  // Same destination IP queried in both domains:
  const uint32_t test_ip = Ip(10, 1, 2, 3);

  NextHopId hop_n3 = router_->Lookup(n3_domain, test_ip);
  NextHopId hop_n6 = router_->Lookup(n6_domain, test_ip);

  EXPECT_EQ(hop_n3, NextHopId(1));
  EXPECT_EQ(hop_n6, NextHopId(2));
  EXPECT_NE(hop_n3, hop_n6);
}

TEST_F(RouteDomainTest, ApplyRouteSetBatch) {
  const RouteDomainId domain{10};
  LpmRouteTable::Config config{.max_routes = 256, .tbl8_groups = 64};
  ASSERT_TRUE(router_->CreateDomain(domain, config).has_value());

  RouteSet route_set = {
      {*Ipv4Prefix::Make(Ip(10, 0, 0, 0), 8), NextHopId(1)},
      {*Ipv4Prefix::Make(Ip(172, 16, 0, 0), 12), NextHopId(2)},
      {*Ipv4Prefix::Make(Ip(192, 168, 1, 0), 24), NextHopId(3)},
  };

  ASSERT_TRUE(router_->ApplyRouteSet(domain, route_set).has_value());

  EXPECT_EQ(router_->Lookup(domain, Ip(10, 5, 5, 5)), NextHopId(1));
  EXPECT_EQ(router_->Lookup(domain, Ip(172, 20, 1, 1)), NextHopId(2));
  EXPECT_EQ(router_->Lookup(domain, Ip(192, 168, 1, 100)), NextHopId(3));
  EXPECT_EQ(router_->Lookup(domain, Ip(8, 8, 8, 8)), kInvalidNextHopId);
}

TEST_F(RouteDomainTest, BatchLookupX4) {
  const RouteDomainId domain{5};
  LpmRouteTable::Config config{.max_routes = 256, .tbl8_groups = 64};
  ASSERT_TRUE(router_->CreateDomain(domain, config).has_value());

  ASSERT_TRUE(router_->SetRoute(domain, *Ipv4Prefix::Make(Ip(10, 0, 0, 0), 8), NextHopId(10)).has_value());
  ASSERT_TRUE(router_->SetRoute(domain, *Ipv4Prefix::Make(Ip(192, 168, 0, 0), 16), NextHopId(20)).has_value());

  uint32_t ips[4] = {
      Ip(10, 1, 1, 1),      // hit hop 10
      Ip(192, 168, 5, 5),   // hit hop 20
      Ip(1, 1, 1, 1),       // miss
      Ip(10, 99, 99, 99),   // hit hop 10
  };
  NextHopId hops[4];

  router_->LookupBatchX4(domain, ips, hops);

  EXPECT_EQ(hops[0], NextHopId(10));
  EXPECT_EQ(hops[1], NextHopId(20));
  EXPECT_EQ(hops[2], kInvalidNextHopId);
  EXPECT_EQ(hops[3], NextHopId(10));
}

TEST_F(RouteDomainTest, NonExistentDomainFailsSafe) {
  const RouteDomainId missing_domain{999};
  EXPECT_FALSE(router_->HasDomain(missing_domain));

  NextHopId hop = router_->Lookup(missing_domain, Ip(10, 1, 2, 3));
  EXPECT_EQ(hop, kInvalidNextHopId);

  uint32_t ips[4] = {Ip(10, 1, 1, 1), 0, 0, 0};
  NextHopId hops[4];
  router_->LookupBatchX4(missing_domain, ips, hops);
  EXPECT_EQ(hops[0], kInvalidNextHopId);
}

}  // namespace
}  // namespace bess::route
