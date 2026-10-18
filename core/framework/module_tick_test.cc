// SPDX-License-Identifier: BSD-3-Clause

#include "framework/module_tick.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace bess::framework {
namespace {

using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

TEST(ModuleTickTest, ACallbackRunsOnEveryTickWithAZeroPeriod) {
  TickHub hub;
  int calls = 0;
  uint64_t last_tick = 0;
  TickEndpoint endpoint(hub, {}, [&](TickContext &ctx) {
    calls++;
    last_tick = ctx.tick();
  });
  EXPECT_EQ(1u, hub.Run(milliseconds(1), 10));
  EXPECT_EQ(1u, hub.Run(milliseconds(1), 11));
  EXPECT_EQ(2, calls);
  EXPECT_EQ(11u, last_tick) << "the callback sees the maintenance tick count";
}

// A period is the least time between the starts of two calls: the first call
// is due at once, and a long period then holds the endpoint back.
TEST(ModuleTickTest, APeriodHoldsAnEndpointBackAfterItsFirstCall) {
  TickHub hub;
  int fast = 0;
  int slow = 0;
  TickEndpoint every(hub, {}, [&](TickContext &) { fast++; });
  TickEndpoint hourly(hub, std::chrono::hours(1), [&](TickContext &) { slow++; });
  for (int i = 0; i < 5; i++) {
    hub.Run(milliseconds(1), static_cast<uint64_t>(i));
  }
  EXPECT_EQ(5, fast);
  EXPECT_EQ(1, slow) << "due at once, then not for an hour";
}

TEST(ModuleTickTest, APeriodElapsingMakesTheEndpointDueAgain) {
  TickHub hub;
  int calls = 0;
  TickEndpoint endpoint(hub, milliseconds(5), [&](TickContext &) { calls++; });
  hub.Run(milliseconds(1), 0);
  hub.Run(milliseconds(1), 1);
  EXPECT_EQ(1, calls);
  std::this_thread::sleep_for(milliseconds(8));
  hub.Run(milliseconds(1), 2);
  EXPECT_EQ(2, calls);
}

// The slice the callback sees is its own, and the hub counts the calls that
// ran past it: the lock is held for the whole call, so overrun is a fact worth
// counting.
TEST(ModuleTickTest, ACallPastItsSliceIsCountedAsAnOverrun) {
  TickHub hub;
  bool expired_early = true;
  TickEndpoint polite(hub, {}, [&](TickContext &ctx) { expired_early = ctx.Expired(); });
  TickEndpoint slow(hub, {}, [](TickContext &ctx) {
    while (!ctx.Expired()) {
    }
    std::this_thread::sleep_for(milliseconds(5));
  });
  hub.Run(milliseconds(2), 0);
  EXPECT_FALSE(expired_early) << "a fresh slice is not expired";
  EXPECT_EQ(2u, hub.calls());
  EXPECT_EQ(1u, hub.overruns()) << "only the call that ran on past its slice";
}

// Destroying an endpoint takes it out of the hub: a module that is gone is
// never called.
TEST(ModuleTickTest, ADestroyedEndpointIsNeverCalled) {
  TickHub hub;
  int calls = 0;
  {
    TickEndpoint endpoint(hub, {}, [&](TickContext &) { calls++; });
    EXPECT_EQ(1u, hub.size());
  }
  EXPECT_EQ(0u, hub.size());
  EXPECT_EQ(0u, hub.Run(milliseconds(1), 0));
  EXPECT_EQ(0, calls);
}

// A module holds its endpoint as a default-constructed member and opens it in
// Init: the registration must follow the endpoint, and the callback must keep
// its state.
TEST(ModuleTickTest, AnEndpointAssignedInInitKeepsItsRegistration) {
  TickHub hub;
  int calls = 0;
  TickEndpoint member;
  EXPECT_FALSE(member.open());
  member = TickEndpoint(hub, {}, [&](TickContext &) { calls++; });
  EXPECT_TRUE(member.open());
  EXPECT_EQ(1u, hub.size()) << "moved, not duplicated";
  EXPECT_EQ(1u, hub.Run(milliseconds(1), 0));
  EXPECT_EQ(1, calls);
}

// A callback may close an endpoint that has not had its turn this round, and
// the loop over the hub's set must not call it.
TEST(ModuleTickTest, ACallbackMayCloseAnEndpointNotYetCalled) {
  TickHub hub;
  int first_calls = 0;
  int second_calls = 0;
  std::unique_ptr<TickEndpoint> second;
  TickEndpoint first(hub, microseconds(0), [&](TickContext &) {
    first_calls++;
    second.reset();  // registered after this one: not yet called this round
  });
  second = std::make_unique<TickEndpoint>(hub, microseconds(0), [&](TickContext &) { second_calls++; });
  EXPECT_EQ(1u, hub.Run(milliseconds(1), 0)) << "only the first was called";
  EXPECT_EQ(1, first_calls);
  EXPECT_EQ(0, second_calls) << "closed before its turn";
  EXPECT_EQ(1u, hub.size());
}

// Events a callback queues come out of the hub, in order and complete, once;
// the hub does not touch the log itself.
TEST(ModuleTickTest, QueuedEventsAreTakenOnceInOrder) {
  TickHub hub;
  TickEndpoint endpoint(hub, {}, [](TickContext &ctx) {
    ctx.Emit("a", "src", {{"k", "1"}});
    ctx.Emit("b", "src", {});
  });
  hub.Run(milliseconds(1), 0);
  const std::vector<PendingEvent> got = hub.TakeEvents();
  ASSERT_EQ(2u, got.size());
  EXPECT_EQ("a", got[0].type);
  EXPECT_EQ("src", got[0].source);
  EXPECT_EQ("1", got[0].fields.at("k"));
  EXPECT_EQ("b", got[1].type);
  EXPECT_TRUE(hub.TakeEvents().empty()) << "taken once";
}

}  // namespace
}  // namespace bess::framework
