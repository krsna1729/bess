// SPDX-License-Identifier: BSD-3-Clause

#include "control/maintenance_loop.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "control/control_plane.h"
#include "framework/module_requests.h"

namespace bess::control {
namespace {

struct Ping {
  uint64_t n;
};

bool WaitFor(const std::atomic<int> &value, int want) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (value.load() < want) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

TEST(MaintenanceLoopTest, DeliversPostedRequestsOnItsOwnThread) {
  ControlPlane control;
  framework::RequestHub hub;
  std::atomic<int> handled{0};
  std::thread::id handler_thread;
  framework::RequestEndpoint<Ping> *self = nullptr;
  framework::RequestEndpoint<Ping> endpoint(hub, [&](const Ping &p) {
    EXPECT_EQ(7u, p.n);
    handler_thread = std::this_thread::get_id();
    self->Done();
    handled++;
  });
  self = &endpoint;
  MaintenanceLoop loop(control, hub, std::chrono::microseconds(500));
  ASSERT_TRUE(endpoint.Post({7}));
  ASSERT_TRUE(WaitFor(handled, 1));
  EXPECT_NE(std::this_thread::get_id(), handler_thread);
  ASSERT_TRUE(endpoint.Post({7})) << "Done() re-armed the endpoint";
  ASSERT_TRUE(WaitFor(handled, 2));
  EXPECT_GE(loop.delivered(), 2u);
}

TEST(MaintenanceLoopTest, NothingIsDeliveredAfterTheLoopStops) {
  ControlPlane control;
  framework::RequestHub hub;
  std::atomic<int> handled{0};
  framework::RequestEndpoint<Ping> endpoint(hub, [&](const Ping &) { handled++; });
  {
    MaintenanceLoop loop(control, hub, std::chrono::microseconds(500));
  }
  ASSERT_TRUE(endpoint.Post({1}));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(0, handled.load());
}

TEST(MaintenanceLoopTest, ZeroIntervalStartsNoThread) {
  ControlPlane control;
  framework::RequestHub hub;
  MaintenanceLoop loop(control, hub, std::chrono::microseconds(0));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(0u, loop.ticks());
}

}  // namespace
}  // namespace bess::control
