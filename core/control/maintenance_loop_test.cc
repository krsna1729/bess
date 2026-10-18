// SPDX-License-Identifier: BSD-3-Clause

#include "control/maintenance_loop.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "control/control_plane.h"
#include "framework/module_requests.h"
#include "framework/module_tick.h"
#include "stats/event_hub.h"

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
  framework::TickHub ticks;
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
  MaintenanceLoop loop(control, hub, ticks, std::chrono::microseconds(500));
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
  framework::TickHub ticks;
  std::atomic<int> handled{0};
  framework::RequestEndpoint<Ping> endpoint(hub, [&](const Ping &) { handled++; });
  {
    MaintenanceLoop loop(control, hub, ticks, std::chrono::microseconds(500));
  }
  ASSERT_TRUE(endpoint.Post({1}));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(0, handled.load());
}

TEST(MaintenanceLoopTest, ZeroIntervalStartsNoThread) {
  ControlPlane control;
  framework::RequestHub hub;
  framework::TickHub ticks;
  MaintenanceLoop loop(control, hub, ticks, std::chrono::microseconds(0));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(0u, loop.ticks());
}

// A tick is control-side work like a module command: it runs on the loop's
// thread with the control-plane lock held, so no RPC (which takes the lock)
// ever sees it half-way through.
TEST(MaintenanceLoopTest, ATickRunsOnTheLoopThreadUnderTheControlLock) {
  ControlPlane control;
  framework::RequestHub hub;
  framework::TickHub ticks;
  std::atomic<int> ran{0};
  std::atomic<bool> inside{false};
  std::atomic<bool> other_thread{false};
  const std::thread::id test_thread = std::this_thread::get_id();
  framework::TickEndpoint endpoint(ticks, {}, [&](framework::TickContext &) {
    other_thread = std::this_thread::get_id() != test_thread;
    inside = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    inside = false;
    ran++;
  });
  MaintenanceLoop loop(control, hub, ticks, std::chrono::microseconds(500));
  ASSERT_TRUE(WaitFor(ran, 1));
  EXPECT_TRUE(other_thread.load());
  int seen_inside = 0;
  for (int i = 0; i < 200; i++) {
    auto lock = control.AcquireLock();
    seen_inside += inside.load() ? 1 : 0;
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }
  EXPECT_EQ(0, seen_inside) << "a holder of the lock never finds a tick running";
}

// What a tick emits reaches the daemon's log with the type, source and fields
// it gave, once the lock is released.
TEST(MaintenanceLoopTest, ATicksEventsReachTheLog) {
  ControlPlane control;
  framework::RequestHub hub;
  framework::TickHub ticks;
  stats::EventHub log;
  std::atomic<int> ran{0};
  framework::TickEndpoint endpoint(ticks, std::chrono::hours(1), [&](framework::TickContext &ctx) {
    ctx.Emit("test.crossed", "mod0", {{"key", "7"}, {"value", "9"}});
    ran++;
  });
  MaintenanceLoop loop(control, hub, ticks, std::chrono::microseconds(500), &log);
  ASSERT_TRUE(WaitFor(ran, 1));
  const stats::EventHub::ReadResult got = log.Read(1, 10, std::chrono::seconds(10));
  ASSERT_FALSE(got.events.empty());
  EXPECT_EQ("test.crossed", got.events[0].type);
  EXPECT_EQ("mod0", got.events[0].source);
  EXPECT_EQ("7", got.events[0].fields.at("key"));
  EXPECT_EQ("9", got.events[0].fields.at("value"));
  EXPECT_EQ(1, ran.load()) << "an hour's period: one call";
}

}  // namespace
}  // namespace bess::control
