// SPDX-License-Identifier: BSD-3-Clause

#include "framework/module_requests.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace bess::framework {
namespace {

struct Grow {
  uint64_t capacity;
  uint64_t size;
  uint64_t check;  // capacity ^ size: a torn copy would not match
};

TEST(ModuleRequestsTest, OnePendingRequestPerEndpointUntilDone) {
  RequestHub hub;
  std::vector<Grow> got;
  RequestEndpoint<Grow> endpoint(hub, [&](const Grow &g) { got.push_back(g); });
  EXPECT_EQ(0u, hub.Deliver()) << "nothing posted";

  EXPECT_TRUE(endpoint.Post({1024, 900, 1024 ^ 900}));
  EXPECT_FALSE(endpoint.Post({1024, 950, 1024 ^ 950})) << "pending: not stored";
  EXPECT_EQ(1u, hub.Deliver());
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(900u, got[0].size);

  // Delivered but not Done: still pending, the condition is being dealt with.
  EXPECT_FALSE(endpoint.Post({1024, 990, 1024 ^ 990}));
  EXPECT_EQ(0u, hub.Deliver());
  endpoint.Done();
  EXPECT_TRUE(endpoint.Post({2048, 1900, 2048 ^ 1900}));
  EXPECT_EQ(1u, hub.Deliver());
  ASSERT_EQ(2u, got.size());
  EXPECT_EQ(2048u, got[1].capacity);
}

TEST(ModuleRequestsTest, ClosingAnEndpointDiscardsItsRequest) {
  RequestHub hub;
  int calls = 0;
  {
    RequestEndpoint<Grow> endpoint(hub, [&](const Grow &) { calls++; });
    EXPECT_TRUE(endpoint.Post({1, 1, 0}));
    EXPECT_EQ(1u, hub.size());
  }
  EXPECT_EQ(0u, hub.size());
  EXPECT_EQ(0u, hub.Deliver());
  EXPECT_EQ(0, calls);
}

TEST(ModuleRequestsTest, AnEndpointOpenedInInitKeepsItsRegistration) {
  RequestHub hub;
  int calls = 0;
  RequestEndpoint<Grow> member;  // a module member, default-constructed
  EXPECT_FALSE(member.open());
  member = RequestEndpoint<Grow>(hub, [&](const Grow &) { calls++; });
  EXPECT_TRUE(member.open());
  EXPECT_EQ(1u, hub.size()) << "the temporary's registration moved, not duplicated";
  EXPECT_TRUE(member.Post({1, 1, 0}));
  EXPECT_EQ(1u, hub.Deliver());
  EXPECT_EQ(1, calls);
}

TEST(ModuleRequestsTest, AHandlerMayCloseAnEndpointNotYetDelivered) {
  RequestHub hub;
  std::unique_ptr<RequestEndpoint<Grow>> other;
  // Registered first, so delivered first: its handler closes `other`, whose
  // request (posted, ready) must then never reach its handler.
  RequestEndpoint<Grow> first(hub, [&](const Grow &) { other.reset(); });
  other = std::make_unique<RequestEndpoint<Grow>>(hub, [](const Grow &) {
    ADD_FAILURE() << "delivered to a closed endpoint";
  });
  ASSERT_TRUE(first.Post({1, 1, 0}));
  ASSERT_TRUE(other->Post({1, 1, 0}));
  EXPECT_EQ(1u, hub.Deliver());
  EXPECT_EQ(1u, hub.size());
}

// Workers post concurrently; the control thread delivers and re-arms. Every
// delivered request is whole, every accepted post is delivered exactly once.
TEST(ModuleRequestsTest, ConcurrentPostersAndOneDeliverer) {
  RequestHub hub;
  std::atomic<uint64_t> accepted{0};
  uint64_t delivered = 0;
  bool torn = false;
  RequestEndpoint<Grow> *endpoint_ptr = nullptr;
  RequestEndpoint<Grow> endpoint(hub, [&](const Grow &g) {
    torn |= (g.capacity ^ g.size) != g.check;
    delivered++;
    endpoint_ptr->Done();
  });
  endpoint_ptr = &endpoint;
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (uint64_t w = 1; w <= 4; w++) {
    workers.emplace_back([&, w] {
      uint64_t i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t c = w << 32 | i++;
        if (endpoint.Post({c, i, c ^ i})) {
          accepted.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  const auto hard = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while ((std::chrono::steady_clock::now() < until || delivered < 1000) &&
         std::chrono::steady_clock::now() < hard) {
    (void)hub.Deliver();
  }
  stop = true;
  for (auto &t : workers) {
    t.join();
  }
  while (hub.Deliver() != 0) {
  }
  EXPECT_FALSE(torn);
  EXPECT_EQ(accepted.load(), delivered);
  EXPECT_GE(delivered, 1000u);
}

}  // namespace
}  // namespace bess::framework
