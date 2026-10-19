// SPDX-License-Identifier: BSD-3-Clause

#include "control/maintenance_loop.h"

#include <any>
#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include "control/control_plane.h"
#include "control/resource_fill.h"
#include "dataplane/transaction_engine.h"
#include "framework/module_requests.h"
#include "framework/module_tick.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "rcu/rcu_domain.h"
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

// A resource whose count and stated capacity the test sets directly, and a
// codec that carries no entries: the watch reads only LiveCount() and
// Capacity(), and needs the resource reachable, which is what a binding means
// (resource_fill_test.cc has the same pair; they are small and test-local).
class FillResource final : public dataplane::Resource {
 public:
  FillResource(std::string name, uint64_t *count, std::optional<uint64_t> *capacity)
      : Resource(std::move(name), {}), count_(count), capacity_(capacity) {}

  size_t LiveCount() const override { return *count_; }
  std::optional<uint64_t> Capacity() const override { return *capacity_; }
  bool Contains(const dataplane::ResourceKey &) const override { return false; }
  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &) const override {}
  std::expected<Reservation, std::string> Reserve(const dataplane::Op &) override {
    return std::unexpected("the fill watch never reserves");
  }

 private:
  uint64_t *count_;
  std::optional<uint64_t> *capacity_;
};

class StubCodec final : public framework::ResourceCodec {
 public:
  Kind kind() const override { return Kind::kMap; }
  std::string key_type() const override { return "k"; }
  std::string value_type() const override { return "v"; }
  std::expected<dataplane::ResourceKey, std::string> Key(std::string_view,
                                                         const std::string &) const override {
    return std::unexpected("unused");
  }
  std::expected<std::any, std::string> Value(std::string_view,
                                             const std::string &) const override {
    return std::unexpected("unused");
  }
  void Entries(const EntryVisitor &) const override {}
};

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

// A crossing the fill watch finds reaches the daemon's log with the type,
// source and fields a controller reads. The resource is over its level before
// the loop starts, so the first sweep reports it; the watch is level-triggered
// and does not need the crossing to happen while the loop runs.
TEST(MaintenanceLoopTest, AFillCrossingReachesTheLog) {
  ControlPlane control;
  framework::RequestHub hub;
  framework::TickHub ticks;
  stats::EventHub log;
  uint64_t count = 80;  // 80% of 100: the level
  std::optional<uint64_t> capacity = 100;
  FillResource resource("mod/table", &count, &capacity);
  rcu::RcuDomain domain(1, 8);
  dataplane::TransactionEngine engine(domain);
  ASSERT_TRUE(engine.Register(&resource).has_value());
  framework::ResourceBindings bindings;
  const framework::ResourceBinding binding = bindings.Bind(resource, std::make_shared<StubCodec>());
  ResourceFillWatch fills(engine, bindings);

  MaintenanceLoop loop(control, hub, ticks, std::chrono::microseconds(500), &log,
                       std::chrono::seconds(1), std::chrono::microseconds(250), &fills);
  const stats::EventHub::ReadResult got = log.Read(1, 10, std::chrono::seconds(10));
  ASSERT_FALSE(got.events.empty());
  EXPECT_EQ(kResourceFillEvent, got.events[0].type);
  EXPECT_EQ("mod", got.events[0].source);
  EXPECT_EQ("mod/table", got.events[0].fields.at("resource"));
  EXPECT_EQ("80", got.events[0].fields.at("count"));
  EXPECT_EQ("80", got.events[0].fields.at("limit"));
  EXPECT_EQ("100", got.events[0].fields.at("capacity"));
}

}  // namespace
}  // namespace bess::control
