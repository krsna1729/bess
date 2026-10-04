// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/expiry_wheel.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "testing/allocation_faults.h"

namespace bess::dataplane {
namespace {

using fault_injection::AllocationFaults;
using fault_injection::ForEachFailurePoint;

using Wheel = ExpiryWheel<uint64_t>;

__extension__ typedef __int128 I128;

template <typename W>
std::unique_ptr<W> Make(size_t capacity, typename W::tick_type start = 0,
                        unsigned shift = 0) {
  auto wheel = W::Create(capacity, start, shift);
  EXPECT_TRUE(wheel.has_value());
  return std::move(wheel).value();
}

// A budget no correct poll comes near: a test never arms more than ten
// thousand timers, and a timer costs at most Levels + 1 units. It stands in for
// "unlimited" so that a poll that cannot finish (a corrupted list that
// delivers forever) fails the test at a bounded cost instead of growing the
// result vector until the process dies. SIZE_MAX itself is tried in
// AnUnlimitedBudgetDeliversEverythingDue.
constexpr size_t kUnlimited = size_t{1} << 22;

// Polls to completion at `now` with kUnlimited budget and returns the
// payloads in delivery order.
template <typename W>
std::vector<uint64_t> Drain(W &wheel, typename W::tick_type now) {
  std::vector<uint64_t> out;
  const auto result = wheel.Poll(now, kUnlimited, [&](const uint64_t &p) noexcept {
    out.push_back(p);
  });
  EXPECT_FALSE(result.exhausted);
  EXPECT_EQ(out.size(), result.fired);
  return out;
}

// -- basics -----------------------------------------------------------------------

TEST(ExpiryWheel, FiresAtItsDeadlineAndNotBefore) {
  auto wheel = Make<Wheel>(8, 1000);
  const ExpiryHandle h = wheel->Schedule(1100, 7);
  ASSERT_NE(h, kNoExpiry);
  EXPECT_TRUE(Drain(*wheel, 1099).empty());
  EXPECT_TRUE(wheel->Alive(h));
  EXPECT_EQ(std::vector<uint64_t>{7}, Drain(*wheel, 1100));
  EXPECT_FALSE(wheel->Alive(h));
  EXPECT_TRUE(Drain(*wheel, 5000).empty());
  EXPECT_EQ(0u, wheel->size());
}

TEST(ExpiryWheel, ZeroAndAlreadyPastDeadlinesFireOnNextPoll) {
  auto wheel = Make<Wheel>(8, 1000);
  wheel->Schedule(1000, 1);  // zero timeout
  wheel->Schedule(3, 2);     // long past
  wheel->Schedule(1000, 3);
  // Already late when armed: delivered first, in arming order.
  wheel->Schedule(1500, 4);
  EXPECT_EQ((std::vector<uint64_t>{1, 2, 3}), Drain(*wheel, 1000));
  EXPECT_EQ((std::vector<uint64_t>{4}), Drain(*wheel, 1500));
}

TEST(ExpiryWheel, RefreshSupersedesTheOldDeadline) {
  auto wheel = Make<Wheel>(8, 0);
  const ExpiryHandle later = wheel->Schedule(100, 1);
  ASSERT_TRUE(wheel->Refresh(later, 200));
  EXPECT_EQ(200u, *wheel->DeadlineOf(later));
  EXPECT_TRUE(Drain(*wheel, 150).empty());  // the old deadline is gone
  EXPECT_TRUE(Drain(*wheel, 199).empty());
  EXPECT_EQ((std::vector<uint64_t>{1}), Drain(*wheel, 200));

  const ExpiryHandle earlier = wheel->Schedule(10'000, 2);
  ASSERT_TRUE(wheel->Refresh(earlier, 500));
  EXPECT_TRUE(Drain(*wheel, 499).empty());
  EXPECT_EQ((std::vector<uint64_t>{2}), Drain(*wheel, 500));
}

TEST(ExpiryWheel, ARefreshedFlowIsMovedOncePerTimeoutNotOncePerRefresh) {
  // A flow touched every tick, with a 1000-tick idle timeout: the deadline is
  // stored on each touch and the node is looked at only when the wheel
  // reaches its old position.
  auto wheel = Make<Wheel>(4, 0);
  const ExpiryHandle h = wheel->Schedule(1000, 1);
  size_t moved = 0;
  for (uint64_t now = 1; now <= 100'000; now++) {
    ASSERT_TRUE(wheel->Refresh(h, now + 1000));
    const auto r = wheel->Poll(now, 16, [&](const uint64_t &) noexcept {
      ADD_FAILURE() << "an active flow expired";
    });
    EXPECT_EQ(0u, r.fired);
    moved += r.moved;
  }
  EXPECT_LE(moved, 100'000u / 1000 * 3 + 6);  // a handful per timeout period
  EXPECT_GT(moved, 0u);
  EXPECT_EQ((std::vector<uint64_t>{1}), Drain(*wheel, 101'000));
}

TEST(ExpiryWheel, CancelPreventsTheFire) {
  auto wheel = Make<Wheel>(8, 0);
  const ExpiryHandle a = wheel->Schedule(10, 1);
  const ExpiryHandle b = wheel->Schedule(10, 2);
  EXPECT_TRUE(wheel->Cancel(a));
  EXPECT_FALSE(wheel->Cancel(a));
  EXPECT_FALSE(wheel->Refresh(a, 20));
  EXPECT_EQ(1u, wheel->size());
  EXPECT_EQ((std::vector<uint64_t>{2}), Drain(*wheel, 10));
  EXPECT_FALSE(wheel->Cancel(b)) << "a fired timer's handle is dead";
}

TEST(ExpiryWheel, ACancelledTimerLeavesTheWheelEmpty) {
  // Cancelling the only timer of a slot must clear the slot's occupancy: a
  // phantom bit would make every later poll stop at that slot once per
  // revolution, work no budget accounts for. An empty wheel has nothing to do
  // however far time jumps, at every level.
  for (const uint64_t deadline : {uint64_t{5}, uint64_t{300}, uint64_t{20'000},
                                  uint64_t{5'000'000}, uint64_t{900'000'000}}) {
    auto wheel = Make<Wheel>(4, 0);
    const ExpiryHandle a = wheel->Schedule(deadline, 1);
    const ExpiryHandle b = wheel->Schedule(deadline, 2);  // same slot
    ASSERT_TRUE(wheel->Cancel(a));
    const auto never = [](const uint64_t &) noexcept { ADD_FAILURE(); };
    EXPECT_TRUE(wheel->Poll(uint64_t{1} << 40, 0, never).exhausted)
        << "one timer is still due";
    ASSERT_TRUE(wheel->Cancel(b));
    ASSERT_FALSE(wheel->Poll(uint64_t{1} << 40, 0, never).exhausted)
        << "an emptied slot still counts as work, deadline " << deadline;
    const auto r = wheel->Poll(uint64_t{1} << 40, 16, never);
    EXPECT_EQ(0u, r.work());
    EXPECT_EQ(uint64_t{1} << 40, wheel->wheel_time());
  }
}

TEST(ExpiryWheel, ACapacityOfNothingOrAGranularityTooLargeIsRefused) {
  EXPECT_EQ(ExpiryError::kInvalidCapacity, Wheel::Create(0).error());
  EXPECT_EQ(ExpiryError::kInvalidGranularity,
            Wheel::Create(4, 0, Wheel::kMaxGranularityShift + 1).error());
  EXPECT_TRUE(Wheel::Create(4, 0, Wheel::kMaxGranularityShift).has_value());
  EXPECT_FALSE(std::string(ToString(ExpiryError::kOutOfMemory)).empty());
}

TEST(ExpiryWheel, AFullEngineRefusesAndChangesNothing) {
  auto wheel = Make<Wheel>(3, 0);
  ExpiryHandle h[3];
  for (int i = 0; i < 3; i++) {
    h[i] = wheel->Schedule(100 + i, i);
    ASSERT_NE(h[i], kNoExpiry);
  }
  EXPECT_TRUE(wheel->full());
  EXPECT_EQ(kNoExpiry, wheel->Schedule(5, 99));
  EXPECT_EQ(3u, wheel->size());
  EXPECT_EQ((std::vector<uint64_t>{0, 1, 2}), Drain(*wheel, 1000));
  EXPECT_FALSE(wheel->full());
  EXPECT_NE(kNoExpiry, wheel->Schedule(2000, 5));
}

// -- handles ----------------------------------------------------------------------

TEST(ExpiryWheel, AStaleHandleCannotTouchTheTimerThatReusedItsNode) {
  auto wheel = Make<Wheel>(2, 0);
  const ExpiryHandle old_handle = wheel->Schedule(10, 1);
  ASSERT_EQ((std::vector<uint64_t>{1}), Drain(*wheel, 10));
  // The node was freed; the next arming takes it again.
  const ExpiryHandle fresh = wheel->Schedule(500, 2);
  ASSERT_EQ(old_handle.id, fresh.id) << "test premise: the node was reused";
  ASSERT_NE(old_handle.generation, fresh.generation);

  EXPECT_FALSE(wheel->Alive(old_handle));
  EXPECT_FALSE(wheel->Cancel(old_handle));
  EXPECT_FALSE(wheel->Refresh(old_handle, 20));
  EXPECT_FALSE(wheel->DeadlineOf(old_handle).has_value());
  // The new timer is untouched.
  EXPECT_EQ(500u, *wheel->DeadlineOf(fresh));
  EXPECT_TRUE(Drain(*wheel, 499).empty());
  EXPECT_EQ((std::vector<uint64_t>{2}), Drain(*wheel, 500));
}

TEST(ExpiryWheel, ACancelledHandleStaysDeadAcrossManyReuses) {
  auto wheel = Make<Wheel>(1, 0);
  const ExpiryHandle first = wheel->Schedule(10, 1);
  ASSERT_TRUE(wheel->Cancel(first));
  for (uint64_t i = 0; i < 1000; i++) {
    const ExpiryHandle h = wheel->Schedule(10 + i, i);
    ASSERT_EQ(first.id, h.id);
    ASSERT_FALSE(wheel->Cancel(first));
    ASSERT_FALSE(wheel->Refresh(first, 1));
    ASSERT_TRUE(wheel->Cancel(h));
  }
}

TEST(ExpiryWheel, ForgedHandlesNameNothing) {
  auto wheel = Make<Wheel>(4, 0);
  const ExpiryHandle real = wheel->Schedule(100, 1);
  const uint32_t nodes_before = real.id.value();
  // The empty handle, an index inside the wheel's own list heads, an index
  // past the end, an even generation for a live node, and a generation off by
  // one.
  const ExpiryHandle forged[] = {
      kNoExpiry,
      {ExpiryId(1), 1},
      {ExpiryId(nodes_before - 1), 1},
      {ExpiryId(0xFFFFFFFFu), 1},
      {real.id, real.generation + 1},
      {real.id, real.generation - 1},
      {real.id, 0},
  };
  for (const ExpiryHandle &h : forged) {
    EXPECT_FALSE(wheel->Alive(h));
    EXPECT_FALSE(wheel->Cancel(h));
    EXPECT_FALSE(wheel->Refresh(h, 5));
  }
  EXPECT_TRUE(wheel->Alive(real));
  EXPECT_EQ(100u, *wheel->DeadlineOf(real));
  EXPECT_EQ(1u, wheel->size());

  // A free node's own (even) generation is not a handle either: it was never
  // issued, and honouring it would let a forger free the node twice.
  const ExpiryHandle spent = wheel->Schedule(200, 2);
  ASSERT_TRUE(wheel->Cancel(spent));
  const ExpiryHandle current_generation{spent.id, spent.generation + 1};
  EXPECT_FALSE(wheel->Alive(current_generation));
  EXPECT_FALSE(wheel->Cancel(current_generation));
  EXPECT_FALSE(wheel->Refresh(current_generation, 5));
  EXPECT_EQ(1u, wheel->size());
  const ExpiryHandle again = wheel->Schedule(300, 3);
  ASSERT_NE(kNoExpiry, again);
  EXPECT_EQ(spent.id, again.id) << "test premise: the freed node is reused";
  EXPECT_FALSE(wheel->Alive(spent));
  EXPECT_EQ(2u, wheel->size());
}

TEST(ExpiryWheel, AnExhaustedGenerationRetiresTheNode) {
  auto wheel = Make<Wheel>(2, 0);
  wheel->SetNextNodeGenerationForTesting(0xFFFFFFFEu);
  const ExpiryHandle h = wheel->Schedule(10, 1);
  ASSERT_EQ(0xFFFFFFFFu, h.generation);
  ASSERT_EQ((std::vector<uint64_t>{1}), Drain(*wheel, 10));
  // Freeing wrapped the generation to 0: reusing the node would make the
  // handle of its first lifetime valid again, so it is retired instead.
  EXPECT_EQ(1u, wheel->quarantined());
  EXPECT_FALSE(wheel->Alive(h));
  const ExpiryHandle a = wheel->Schedule(20, 2);
  ASSERT_NE(kNoExpiry, a);
  EXPECT_NE(h.id, a.id);
  EXPECT_EQ(kNoExpiry, wheel->Schedule(20, 3)) << "one node is left";
  EXPECT_FALSE(wheel->Cancel(h));
  EXPECT_EQ((std::vector<uint64_t>{2}), Drain(*wheel, 20));
}

TEST(ExpiryWheel, NodeSizes) {
  EXPECT_EQ(32u, (ExpiryWheel<uint64_t>::node_bytes()));
  EXPECT_EQ(24u, (ExpiryWheel<uint64_t, uint32_t, 6, 5>::node_bytes()));
  EXPECT_EQ(20u, (ExpiryWheel<uint32_t, uint32_t, 6, 5>::node_bytes()));
  auto wheel = Make<Wheel>(1000, 0);
  // Nodes plus the 2 + levels * slots list heads.
  EXPECT_EQ(sizeof(Wheel) + (1000 + 6 * 64 + 2) * 32, wheel->memory_bytes());
}

// -- the callback -----------------------------------------------------------------

TEST(ExpiryWheel, ACallbackMayCancelTheTimerBeingDelivered) {
  // A flow table's OnErase cancels the flow's timer while the expiry is
  // erasing the flow.
  auto wheel = Make<Wheel>(4, 0);
  std::vector<ExpiryHandle> handles;
  for (uint64_t i = 0; i < 4; i++) {
    handles.push_back(wheel->Schedule(10, i));
  }
  size_t fired = 0;
  wheel->Poll(10, 100, [&](const uint64_t &p) noexcept {
    fired++;
    EXPECT_TRUE(wheel->Cancel(handles[p]));
    EXPECT_FALSE(wheel->Cancel(handles[p]));
    EXPECT_FALSE(wheel->Refresh(handles[p], 99));
  });
  EXPECT_EQ(4u, fired);
  EXPECT_EQ(0u, wheel->size());
  for (int i = 0; i < 4; i++) {  // all four nodes are usable again
    EXPECT_NE(kNoExpiry, wheel->Schedule(50, 100 + i));
  }
  EXPECT_TRUE(wheel->full());
}

TEST(ExpiryWheel, ACallbackMayCancelAndRefreshOtherTimersAndArmNewOnes) {
  auto wheel = Make<Wheel>(8, 0);
  const ExpiryHandle victim = wheel->Schedule(10, 100);  // due with the trigger
  const ExpiryHandle trigger = wheel->Schedule(10, 1);   // fires first
  const ExpiryHandle moved = wheel->Schedule(10, 200);
  (void)trigger;
  std::vector<uint64_t> got;
  wheel->Poll(10, 100, [&](const uint64_t &p) noexcept {
    got.push_back(p);
    if (p == 100) {
      EXPECT_TRUE(wheel->Cancel(wheel->Schedule(50, 7)));
      EXPECT_TRUE(wheel->Refresh(moved, 30));
      // Armed already due: delivered in this very poll.
      wheel->Schedule(5, 300);
    }
  });
  EXPECT_EQ((std::vector<uint64_t>{100, 1, 300}), got);
  (void)victim;
  EXPECT_EQ((std::vector<uint64_t>{200}), Drain(*wheel, 30));
}

TEST(ExpiryWheel, ARefreshOfTheTimerBeingDeliveredIsRefused) {
  auto wheel = Make<Wheel>(4, 0);
  ExpiryHandle h = wheel->Schedule(10, 1);
  bool refused = false;
  wheel->Poll(10, 10, [&](const uint64_t &) noexcept {
    refused = !wheel->Refresh(h, 99);
  });
  EXPECT_TRUE(refused);
  EXPECT_FALSE(wheel->Alive(h)) << "it was spent: the callback returned void";
}

TEST(ExpiryWheel, TheCallbackMayRearmTheSameTimerByReturningADeadline) {
  // The owner keeps the real deadline in its own state; the engine's record
  // may be early.
  auto wheel = Make<Wheel>(4, 0);
  uint64_t real_deadline = 400;
  const ExpiryHandle h = wheel->Schedule(100, 1);
  std::vector<uint64_t> at;
  for (uint64_t now = 0; now <= 1000; now += 50) {
    wheel->Poll(now, 10, [&](const uint64_t &) noexcept -> std::optional<uint64_t> {
      if (now < real_deadline) {
        return real_deadline;  // not yet: keep the timer, same handle
      }
      at.push_back(now);
      return std::nullopt;
    });
    if (now == 200) {
      EXPECT_TRUE(wheel->Alive(h)) << "re-armed, so the handle still names it";
      EXPECT_EQ(400u, *wheel->DeadlineOf(h));
    }
  }
  EXPECT_EQ((std::vector<uint64_t>{400}), at);
  EXPECT_FALSE(wheel->Alive(h));
}

TEST(ExpiryWheel, ARearmDeadlineInThePastFiresAgainWithinTheBudget) {
  auto wheel = Make<Wheel>(4, 0);
  wheel->Schedule(10, 1);
  size_t calls = 0;
  const auto r = wheel->Poll(
      10, 5, [&](const uint64_t &) noexcept -> std::optional<uint64_t> {
        calls++;
        // In the past: due again at once. (After a thousand calls it stops, so
        // that an engine that ignores the budget fails here instead of
        // looping.)
        return calls > 1000 ? std::nullopt : std::optional<uint64_t>(3);
      });
  EXPECT_EQ(5u, calls);
  EXPECT_TRUE(r.exhausted);
  EXPECT_EQ(1u, wheel->size());
}

// -- time ---------------------------------------------------------------------------

TEST(ExpiryWheel, ATimeThatGoesBackwardsIsTreatedAsNoTimePassing) {
  auto wheel = Make<Wheel>(4, 1000);
  wheel->Schedule(1500, 1);
  EXPECT_TRUE(Drain(*wheel, 1400).empty());
  EXPECT_EQ(1400u, wheel->wheel_time());
  EXPECT_TRUE(Drain(*wheel, 900).empty());
  EXPECT_EQ(1400u, wheel->wheel_time());
  EXPECT_EQ((std::vector<uint64_t>{1}), Drain(*wheel, 1500));
}

TEST(ExpiryWheel, VeryLongTimeoutsAreOrderedAndNeverEarly) {
  using LongWheel = ExpiryWheel<uint64_t>;
  auto wheel = Make<LongWheel>(8, 12345, /*shift=*/20);
  const uint64_t start = 12345;
  EXPECT_EQ(start + LongWheel::kMaxTimeout,
            LongWheel::After(start, UINT64_MAX));
  EXPECT_EQ(start + 77, LongWheel::After(start, 77));
  const uint64_t forever = LongWheel::After(start, UINT64_MAX);
  const uint64_t two_to_40 = start + (uint64_t{1} << 40);
  wheel->Schedule(forever, 1);
  wheel->Schedule(two_to_40, 2);
  wheel->Schedule(start + (uint64_t{1} << 55), 3);
  // Polls may be at most kMaxTimeout apart.
  EXPECT_TRUE(Drain(*wheel, two_to_40 - (uint64_t{1} << 20)).empty());
  EXPECT_EQ((std::vector<uint64_t>{2}), Drain(*wheel, two_to_40 + (uint64_t{1} << 20)));
  EXPECT_EQ((std::vector<uint64_t>{3}), Drain(*wheel, start + (uint64_t{1} << 56)));
  EXPECT_TRUE(Drain(*wheel, forever - (uint64_t{1} << 20)).empty());
  EXPECT_EQ(1u, wheel->size());
  EXPECT_EQ((std::vector<uint64_t>{1}), Drain(*wheel, forever + (uint64_t{1} << 20)));
}

TEST(ExpiryWheel, ADeadlineBeyondTheWheelSpanFiresExactly) {
  // Span of a 3-level wheel of 8 slots is 512 ticks; the deadline is 100x
  // that. It is re-placed once per revolution of the top level, never fires
  // early and fires on the first poll at or after the deadline.
  using Small = ExpiryWheel<uint64_t, uint64_t, 3, 3>;
  auto wheel = Make<Small>(4, 0);
  wheel->Schedule(51'000, 1);
  size_t moved = 0;
  for (uint64_t now = 100; now < 51'000; now += 100) {
    const auto r = wheel->Poll(now, 100, [](const uint64_t &) noexcept {
      ADD_FAILURE() << "fired early";
    });
    moved += r.moved;
  }
  EXPECT_GT(moved, 50u);
  EXPECT_LT(moved, 400u);
  EXPECT_EQ((std::vector<uint64_t>{1}), Drain(*wheel, 51'000));
}

TEST(ExpiryWheel, GranularityRoundsUpAndNeverFiresEarly) {
  const unsigned shift = 3;  // 8-tick wheel tick
  for (uint64_t start = 0; start < 16; start++) {
    for (uint64_t timeout = 0; timeout < 200; timeout++) {
      auto wheel = Make<Wheel>(1, start, shift);
      const uint64_t deadline = start + timeout;
      wheel->Schedule(deadline, 1);
      std::optional<uint64_t> fired_at;
      for (uint64_t now = start; now < deadline + 20 && !fired_at; now++) {
        if (!Drain(*wheel, now).empty()) fired_at = now;
      }
      ASSERT_TRUE(fired_at.has_value());
      ASSERT_GE(*fired_at, deadline) << "early";
      ASSERT_LT(*fired_at, deadline + 8) << "later than one wheel tick";
    }
  }
}

// -- same deadline and order ----------------------------------------------------------

TEST(ExpiryWheel, ManyTimersWithOneDeadlineFireInArmingOrder) {
  for (const uint64_t when : {uint64_t{1}, uint64_t{70}, uint64_t{5000},
                              uint64_t{300'000}, uint64_t{50'000'000}}) {
    auto wheel = Make<Wheel>(10'000, 0);
    for (uint64_t i = 0; i < 10'000; i++) {
      ASSERT_NE(kNoExpiry, wheel->Schedule(when, i));
    }
    const auto fired = Drain(*wheel, when);
    ASSERT_EQ(10'000u, fired.size());
    for (uint64_t i = 0; i < fired.size(); i++) {
      ASSERT_EQ(i, fired[i]) << "deadline " << when;
    }
  }
}

TEST(ExpiryWheel, TimersFireInDeadlineOrderAndTiesInArmingOrder) {
  std::mt19937_64 rng(1);
  auto wheel = Make<Wheel>(5000, 0);
  std::vector<uint64_t> deadline(5000);
  for (uint64_t i = 0; i < 5000; i++) {
    // Few distinct deadlines, over every level.
    const uint64_t scale = uint64_t{1} << (rng() % 22);
    deadline[i] = 1 + (rng() % 6) * scale;
    ASSERT_NE(kNoExpiry, wheel->Schedule(deadline[i], i));
  }
  const auto fired = Drain(*wheel, uint64_t{1} << 40);
  ASSERT_EQ(5000u, fired.size());
  for (size_t i = 1; i < fired.size(); i++) {
    const uint64_t a = fired[i - 1], b = fired[i];
    ASSERT_LE(deadline[a], deadline[b]);
    if (deadline[a] == deadline[b]) {
      ASSERT_LT(a, b) << "equal deadlines, armed at the same time";
    }
  }
}

TEST(ExpiryWheel, TheSameHistoryGivesTheSameOrder) {
  auto run = [] {
    std::mt19937_64 rng(5);
    auto wheel = Make<Wheel>(300, 0);
    std::vector<uint64_t> order;
    uint64_t now = 0, id = 0;
    for (int step = 0; step < 4000; step++) {
      if (rng() % 3 != 0) {
        // Few distinct deadlines at different arming times.
        wheel->Schedule(now + (rng() % 4) * 100 + 50, id++);
      } else {
        now += rng() % 70;
        wheel->Poll(now, 1 + rng() % 9, [&](const uint64_t &p) noexcept {
          order.push_back(p);
        });
      }
    }
    Drain(*wheel, now + 1000);
    return order;
  };
  EXPECT_EQ(run(), run());
}

// -- budget -------------------------------------------------------------------------

TEST(ExpiryWheel, ABudgetedPollDoesAtMostThatMuchWorkHoweverManyAreDue) {
  constexpr uint64_t kN = 5000;
  for (const uint64_t when : {uint64_t{3}, uint64_t{5'000'000}}) {
    // 3: all in the due list. 5,000,000: in level 3, so each timer is moved
    // three times before it can fire.
    auto wheel = Make<Wheel>(kN, 0);
    for (uint64_t i = 0; i < kN; i++) wheel->Schedule(when, i);
    std::vector<uint64_t> seen;
    size_t polls = 0, total_work = 0;
    for (;;) {
      polls++;
      const auto r = wheel->Poll(when, 37, [&](const uint64_t &p) noexcept {
        seen.push_back(p);
      });
      ASSERT_LE(r.work(), 37u);
      total_work += r.work();
      if (!r.exhausted) break;
      ASSERT_EQ(37u, r.work()) << "stopped early with work left";
      ASSERT_LT(polls, 100'000u);
    }
    ASSERT_EQ(kN, seen.size());
    for (uint64_t i = 0; i < kN; i++) ASSERT_EQ(i, seen[i]);  // none lost, none twice
    EXPECT_GE(polls, total_work / 37);
    EXPECT_EQ(0u, wheel->size());
    if (when > 100) {
      EXPECT_GE(total_work, kN * 2) << "moved down through the levels";
    }
  }
}

TEST(ExpiryWheel, AMovingStormNeverFiresEarlyAndALatePollFinishesIt) {
  constexpr uint64_t kN = 2000;
  auto wheel = Make<Wheel>(kN, 0);
  for (uint64_t i = 0; i < kN; i++) wheel->Schedule(5'000'000, i);
  // Poll just before the deadline in small budgets: the timers move down but
  // none fires.
  uint64_t now = 4'999'999;
  for (int i = 0; i < 20'000; i++) {
    const auto r = wheel->Poll(now, 10, [](const uint64_t &) noexcept {
      ADD_FAILURE() << "fired before the deadline";
    });
    ASSERT_EQ(0u, r.fired);
    ASSERT_LE(r.work(), 10u);
    if (!r.exhausted) break;
  }
  EXPECT_EQ(kN, wheel->size());
  EXPECT_EQ(kN, Drain(*wheel, 5'000'000).size());
}

TEST(ExpiryWheel, AZeroBudgetDoesNothingAndSaysWhetherWorkIsWaiting) {
  auto wheel = Make<Wheel>(4, 0);
  wheel->Schedule(100, 1);
  const auto never = [](const uint64_t &) noexcept { ADD_FAILURE(); };
  auto r = wheel->Poll(50, 0, never);
  EXPECT_FALSE(r.exhausted);
  EXPECT_EQ(0u, r.work());
  EXPECT_EQ(0u, wheel->wheel_time()) << "a zero budget does not advance time";
  r = wheel->Poll(100, 0, never);
  EXPECT_TRUE(r.exhausted);
  EXPECT_EQ(0u, r.work());
  EXPECT_EQ(1u, wheel->size());
  EXPECT_EQ((std::vector<uint64_t>{1}), Drain(*wheel, 100));
}

TEST(ExpiryWheel, AnUnlimitedBudgetDeliversEverythingDue) {
  // The budget arithmetic must not overflow at SIZE_MAX, with timers that are
  // delivered directly and timers that are moved down first.
  auto wheel = Make<Wheel>(8, 0);
  for (uint64_t i = 0; i < 4; i++) wheel->Schedule(10, i);
  for (uint64_t i = 4; i < 8; i++) wheel->Schedule(5'000'000, i);
  std::vector<uint64_t> seen;
  const auto r = wheel->Poll(5'000'000, SIZE_MAX, [&](const uint64_t &p) noexcept {
    seen.push_back(p);
  });
  EXPECT_FALSE(r.exhausted);
  EXPECT_EQ((std::vector<uint64_t>{0, 1, 2, 3, 4, 5, 6, 7}), seen);
  EXPECT_EQ(0u, wheel->size());
}

TEST(ExpiryWheel, ABudgetedStormResumesAcrossLaterTimesAndNewArmings) {
  auto wheel = Make<Wheel>(300, 0);
  for (uint64_t i = 0; i < 100; i++) wheel->Schedule(10, i);
  std::vector<uint64_t> seen;
  auto collect = [&](const uint64_t &p) noexcept { seen.push_back(p); };
  auto r = wheel->Poll(10, 30, collect);
  ASSERT_TRUE(r.exhausted);
  ASSERT_EQ(30u, seen.size());
  // Meanwhile: time moves on and more timers are armed, some already due.
  for (uint64_t i = 100; i < 150; i++) wheel->Schedule(20, i);
  for (uint64_t i = 150; i < 200; i++) wheel->Schedule(5, i);
  r = wheel->Poll(15, 40, collect);
  ASSERT_TRUE(r.exhausted);
  while ((r = wheel->Poll(25, 33, collect)).exhausted) {
  }
  EXPECT_EQ(200u, seen.size());
  EXPECT_EQ(std::set<uint64_t>(seen.begin(), seen.end()).size(), seen.size());
  EXPECT_EQ(0u, wheel->size());
}

// -- exhaustive wraparound over a tiny tick type --------------------------------------

// An 8-bit tick wraps every 256 ticks, so every start phase, deadline offset
// and polling step can be tried. Wheels of 4-slot levels span 16 ticks (two
// levels) or 64 (three), so most offsets also exercise re-placement beyond the
// span. `now` is tracked as an exact integer; the engine only ever sees it
// modulo 256.
template <typename W>
void ExhaustiveSingleTimer(unsigned shift) {
  const uint8_t kSteps[] = {1, 2, 3, 5, 8, 13, 31, 63};
  const int unit = 1 << shift;
  for (int start = 0; start < 256; start++) {
    for (int timeout = 0; timeout <= static_cast<int>(W::kMaxTimeout); timeout++) {
      for (const uint8_t step : kSteps) {
        auto wheel = W::Create(1, static_cast<uint8_t>(start), shift).value();
        const int deadline = start + timeout;
        wheel->Schedule(static_cast<uint8_t>(deadline), 1);
        int fired = 0;
        for (int now = start; now <= deadline + 3 * unit + step; now += step) {
          const auto r = wheel->Poll(static_cast<uint8_t>(now), 64,
                                     [&](const uint32_t &) noexcept { fired++; });
          // Due iff the deadline is no later than `now` rounded down.
          const bool due = deadline <= (now & ~(unit - 1));
          ASSERT_EQ(due ? 1 : 0, fired)
              << "start " << start << " timeout " << timeout << " step "
              << int{step} << " now " << now << " shift " << shift;
          if (fired) {
            ASSERT_EQ(1u, r.fired);
            break;
          }
        }
        ASSERT_EQ(1, fired);
      }
    }
  }
}

TEST(ExpiryWheelWrap, EveryStartTimeoutAndStepOfAnEightBitClock) {
  ExhaustiveSingleTimer<ExpiryWheel<uint32_t, uint8_t, 2, 2>>(0);
  ExhaustiveSingleTimer<ExpiryWheel<uint32_t, uint8_t, 2, 2>>(1);
  ExhaustiveSingleTimer<ExpiryWheel<uint32_t, uint8_t, 2, 3>>(0);
  ExhaustiveSingleTimer<ExpiryWheel<uint32_t, uint8_t, 2, 3>>(1);
  ExhaustiveSingleTimer<ExpiryWheel<uint32_t, uint8_t, 1, 6>>(1);
}

// Two timers, a refresh (to every deadline in a spread of offsets) and a
// cancel, at every start phase.
TEST(ExpiryWheelWrap, EveryStartWithARefreshAndACancelOfAnEightBitClock) {
  using W = ExpiryWheel<uint32_t, uint8_t, 2, 2>;
  const int offsets[] = {0, 1, 3, 4, 8, 16, 17, 33, 48, 63};
  for (const unsigned shift : {0u, 1u}) {
    const int unit = 1 << shift;
    for (int start = 0; start < 256; start += 1) {
      for (const int d1 : offsets) {
        for (const int d2 : offsets) {
          for (const int mid : {3, 20}) {
            for (const int refresh_to : offsets) {
              auto wheel = W::Create(2, static_cast<uint8_t>(start), shift).value();
              const ExpiryHandle a = wheel->Schedule(static_cast<uint8_t>(start + d1), 1);
              const ExpiryHandle b = wheel->Schedule(static_cast<uint8_t>(start + d2), 2);
              // Model: a is refreshed at time start+mid to start+mid+refresh_to
              // if still armed (it fires before that if due earlier); b is
              // cancelled at the same time if still armed.
              int now = start;
              int deadline_a = start + d1;
              int deadline_b = start + d2;
              bool a_live = true, b_live = true;
              int fired_a = -1, fired_b = -1;
              auto poll = [&](int to) {
                wheel->Poll(static_cast<uint8_t>(to), 16,
                            [&](const uint32_t &p) noexcept {
                              (p == 1 ? fired_a : fired_b) = to;
                            });
                const int floor_to = to & ~(unit - 1);
                if (a_live && deadline_a <= floor_to) {
                  a_live = false;
                  ASSERT_EQ(to, fired_a);
                }
                if (b_live && deadline_b <= floor_to) {
                  b_live = false;
                  ASSERT_EQ(to, fired_b);
                }
                ASSERT_EQ(a_live, wheel->Alive(a));
                ASSERT_EQ(b_live, wheel->Alive(b));
              };
              now = start + mid;
              poll(now);
              if (HasFatalFailure()) return;
              if (a_live) {
                deadline_a = now + refresh_to;
                ASSERT_TRUE(wheel->Refresh(a, static_cast<uint8_t>(deadline_a)));
              }
              if (b_live) {
                ASSERT_TRUE(wheel->Cancel(b));
                b_live = false;
              }
              for (int t = now + 1; t <= now + 70; t += 3) {
                poll(t);
                if (HasFatalFailure()) return;
              }
              ASSERT_FALSE(a_live);
            }
          }
        }
      }
    }
  }
}

// -- random differential against a trivially correct model ---------------------------

struct Params {
  uint64_t seed;
  size_t steps;
  size_t capacity;
  unsigned shift;
  uint64_t start;  // the clock's initial value (near its wrap, to cross it)
  uint64_t max_span;  // largest timeout and step; 0: a quarter of kMaxTimeout
};

// The model: a multimap of true (unwrapped, 128-bit) deadlines. A timer is due
// iff its deadline is at most the true time rounded down to the granularity.
template <typename W>
void RunDifferential(const Params &p) {
  using Tick = typename W::tick_type;
  auto wheel = Make<W>(p.capacity, static_cast<Tick>(p.start), p.shift);
  std::mt19937_64 rng(p.seed);
  const uint64_t max_span = p.max_span != 0 ? p.max_span : W::kMaxTimeout / 4;

  struct Live {
    ExpiryHandle handle;
    I128 deadline;
  };
  std::map<uint64_t, Live> live;  // by payload
  std::multimap<I128, uint64_t> by_deadline;
  std::map<uint64_t, std::multimap<I128, uint64_t>::iterator> position;
  std::vector<ExpiryHandle> dead;  // ring of recently spent handles
  I128 now = p.start;
  uint64_t next_id = 1;
  const I128 unit_mask = (I128{1} << p.shift) - 1;

  auto log_uniform = [&](uint64_t cap) -> uint64_t {
    if (cap == 0) return 0;
    const int bits = static_cast<int>(rng() % (std::bit_width(cap) + 1));
    const uint64_t v = bits == 0 ? 0 : rng() >> (64 - bits);
    return v > cap ? cap : v;
  };
  auto new_deadline = [&]() -> I128 {
    switch (rng() % 10) {
      case 0:
        return now;  // zero timeout
      case 1:
        return now - static_cast<I128>(log_uniform(40));  // already late
      default:
        return now + static_cast<I128>(log_uniform(max_span));
    }
  };
  auto retire = [&](uint64_t id) {
    auto it = live.find(id);
    ASSERT_TRUE(it != live.end());
    by_deadline.erase(position[id]);
    position.erase(id);
    if (dead.size() < 64) {
      dead.push_back(it->second.handle);
    } else {
      dead[rng() % dead.size()] = it->second.handle;
    }
    live.erase(it);
  };
  auto set_deadline = [&](uint64_t id, I128 d) {
    by_deadline.erase(position[id]);
    position[id] = by_deadline.emplace(d, id);
    live[id].deadline = d;
  };

  auto poll_once = [&](size_t budget) -> bool {
    std::vector<uint64_t> fired;
    const I128 floor_now = now & ~unit_mask;
    const auto r = wheel->Poll(static_cast<Tick>(now), budget,
                               [&](const uint64_t &id) noexcept {
                                 fired.push_back(id);
                               });
    EXPECT_LE(r.work(), budget);
    EXPECT_EQ(fired.size(), r.fired);
    if (r.exhausted) {
      EXPECT_EQ(budget, r.work());
    }
    for (const uint64_t id : fired) {
      auto it = live.find(id);
      EXPECT_TRUE(it != live.end()) << "fired a spent or cancelled timer " << id;
      if (it == live.end()) return false;
      EXPECT_LE(it->second.deadline, floor_now) << "fired early";
      EXPECT_FALSE(wheel->Alive(it->second.handle));
      retire(id);
    }
    if (!r.exhausted) {
      EXPECT_TRUE(by_deadline.empty() || by_deadline.begin()->first > floor_now)
          << "a due timer was not delivered";
    }
    return r.exhausted;
  };

  int carried = 0;
  for (size_t step = 0; step < p.steps && !::testing::Test::HasFailure(); step++) {
    const uint64_t op = rng() % 100;
    if (op < 32) {
      const I128 d = new_deadline();
      const uint64_t id = next_id++;
      const ExpiryHandle h = wheel->Schedule(static_cast<Tick>(d), id);
      if (live.size() + wheel->quarantined() >= p.capacity) {
        ASSERT_EQ(kNoExpiry, h);
        ASSERT_TRUE(wheel->full());
      } else {
        ASSERT_NE(kNoExpiry, h);
        live[id] = {h, d};
        position[id] = by_deadline.emplace(d, id);
      }
    } else if (op < 48 && !live.empty()) {
      auto it = live.begin();
      std::advance(it, rng() % live.size());
      const I128 d = new_deadline();
      ASSERT_TRUE(wheel->Refresh(it->second.handle, static_cast<Tick>(d)));
      set_deadline(it->first, d);
    } else if (op < 60 && !live.empty()) {
      auto it = live.begin();
      std::advance(it, rng() % live.size());
      const uint64_t id = it->first;
      ASSERT_TRUE(wheel->Cancel(it->second.handle));
      retire(id);
    } else if (op < 66 && !dead.empty()) {
      const ExpiryHandle h = dead[rng() % dead.size()];
      ASSERT_FALSE(wheel->Alive(h));
      ASSERT_FALSE(wheel->Cancel(h));
      ASSERT_FALSE(wheel->Refresh(h, static_cast<Tick>(now)));
    } else {
      const uint64_t step_size = log_uniform(max_span);
      now += step_size;
      const size_t budget = std::vector<size_t>{1, 2, 5, 17, 100, kUnlimited}[rng() % 6];
      bool exhausted = poll_once(budget);
      if (exhausted && carried < 2 && rng() % 3 == 0) {
        carried++;  // leave the backlog for later operations to meet
      } else {
        while (exhausted) exhausted = poll_once(1 + rng() % 40);
        carried = 0;
      }
    }
    ASSERT_EQ(live.size(), wheel->size());
    if (live.empty()) {
      // Nothing armed: however far time would jump there is nothing to do (a
      // slot emptied by a cancel or a poll leaves no phantom event behind).
      const auto idle = wheel->Poll(static_cast<Tick>(now + max_span), 0,
                                    [](const uint64_t &) noexcept { ADD_FAILURE(); });
      ASSERT_FALSE(idle.exhausted) << "step " << step;
    }
    if (step % 97 == 0) {
      for (const auto &[id, l] : live) {
        ASSERT_TRUE(wheel->Alive(l.handle));
        ASSERT_EQ(static_cast<Tick>(l.deadline), *wheel->DeadlineOf(l.handle));
      }
    }
  }
  // Everything armed eventually fires: advance past every deadline.
  I128 last = now;
  for (const auto &[d, id] : by_deadline) last = std::max(last, d);
  now = last + 2 * (unit_mask + 1);  // past the deadline rounded up
  while (poll_once(1 + rng() % 50)) {
  }
  EXPECT_EQ(0u, wheel->size()) << "seed " << p.seed << " shift " << p.shift
                               << " cap " << p.capacity;
  EXPECT_TRUE(by_deadline.empty());
}

TEST(ExpiryWheelDifferential, SixtyFourBitTicksNearTheWrapAtEveryGranularity) {
  for (uint64_t seed = 1; seed <= 3; seed++) {
    // Timeouts past the wheel's span (2^36 ticks at g = 0) are re-placed once
    // per revolution, so cap them at 2^44 here to keep a huge poll step cheap.
    RunDifferential<ExpiryWheel<uint64_t>>(
        {seed, 120'000, 2000, 0, UINT64_MAX - 5000, uint64_t{1} << 44});
    RunDifferential<ExpiryWheel<uint64_t>>(
        {seed, 60'000, 500, 20, UINT64_MAX - (uint64_t{3} << 20), 0});
    RunDifferential<ExpiryWheel<uint64_t>>(
        {seed, 60'000, 64, 27, UINT64_MAX - (uint64_t{3} << 27), 0});
  }
}

TEST(ExpiryWheelDifferential, NarrowTicksWrapManyTimesDuringARun) {
  for (uint64_t seed = 1; seed <= 3; seed++) {
    RunDifferential<ExpiryWheel<uint64_t, uint32_t, 6, 5>>(
        {seed, 100'000, 1000, 1, UINT32_MAX - 2000, 0});
    RunDifferential<ExpiryWheel<uint64_t, uint32_t, 4, 4>>(
        {seed, 100'000, 700, 9, UINT32_MAX - 2000, 0});
    RunDifferential<ExpiryWheel<uint64_t, uint16_t, 3, 4>>(
        {seed, 100'000, 300, 2, UINT16_MAX - 20, 0});
    RunDifferential<ExpiryWheel<uint64_t, uint16_t, 1, 8>>(
        {seed, 100'000, 300, 3, UINT16_MAX - 20, 0});
    RunDifferential<ExpiryWheel<uint64_t, uint8_t, 2, 3>>(
        {seed, 100'000, 16, 1, 250, 0});
    RunDifferential<ExpiryWheel<uint64_t, uint8_t, 3, 2>>(
        {seed, 100'000, 40, 0, 200, 0});
    RunDifferential<ExpiryWheel<uint64_t, uint8_t, 1, 4>>(
        {seed, 100'000, 8, 0, 255, 0});
  }
}

// The owner keeps the real deadline (last_seen + timeout) in its own state and
// refreshes it with a plain store; the engine's record is only a lower bound,
// and the callback re-arms the timer when it fires early.
template <typename W>
void RunOwnerSideRefresh(uint64_t seed, unsigned shift, uint64_t start) {
  using Tick = typename W::tick_type;
  constexpr size_t kFlows = 400;
  const uint64_t timeout = std::min<uint64_t>(W::kMaxTimeout / 8, 5000);
  auto wheel = Make<W>(kFlows, static_cast<Tick>(start), shift);
  std::mt19937_64 rng(seed);
  struct Flow {
    bool live = false;
    ExpiryHandle handle;
    I128 last_seen = 0;
  };
  std::vector<Flow> flows(kFlows);
  I128 now = start;
  const I128 unit_mask = (I128{1} << shift) - 1;
  size_t expired = 0;
  for (int step = 0; step < 60'000 && !::testing::Test::HasFailure(); step++) {
    Flow &f = flows[rng() % kFlows];
    const uint64_t op = rng() % 10;
    if (op < 3 && !f.live) {
      f.live = true;
      f.last_seen = now;
      f.handle = wheel->Schedule(static_cast<Tick>(now + timeout),
                                 static_cast<uint64_t>(&f - flows.data()));
      ASSERT_NE(kNoExpiry, f.handle);
    } else if (op < 7 && f.live) {
      f.last_seen = now;  // a packet: one store, no call into the engine
    } else if (op == 7 && f.live) {
      ASSERT_TRUE(wheel->Cancel(f.handle));  // explicit erase
      f.live = false;
    } else {
      now += rng() % (timeout / 8 + 1);
      const I128 floor_now = now & ~unit_mask;
      size_t budget = std::vector<size_t>{3, 50, kUnlimited}[rng() % 3];
      for (;;) {
        const auto r = wheel->Poll(
            static_cast<Tick>(now), budget,
            [&](const uint64_t &i) noexcept -> std::optional<Tick> {
              Flow &g = flows[i];
              EXPECT_TRUE(g.live);
              const I128 real = g.last_seen + timeout;
              if (real > floor_now) {
                return static_cast<Tick>(real);  // touched since: re-arm
              }
              g.live = false;
              expired++;
              return std::nullopt;
            });
        if (!r.exhausted) break;
      }
      // A completed poll leaves no live flow idle past its real deadline.
      for (const Flow &g : flows) {
        ASSERT_TRUE(!g.live || g.last_seen + timeout > floor_now);
      }
    }
  }
  // Drain: nothing may linger past its real deadline.
  now += timeout + 2 * (unit_mask + 1);
  for (;;) {
    const I128 floor_now = now & ~unit_mask;
    const auto r = wheel->Poll(
        static_cast<Tick>(now), kUnlimited,
        [&](const uint64_t &i) noexcept -> std::optional<Tick> {
          Flow &g = flows[i];
          const I128 real = g.last_seen + timeout;
          if (real > floor_now) return static_cast<Tick>(real);
          g.live = false;
          expired++;
          return std::nullopt;
        });
    if (!r.exhausted) break;
  }
  for (const Flow &f : flows) EXPECT_FALSE(f.live);
  EXPECT_EQ(0u, wheel->size());
  EXPECT_GT(expired, 100u);
}

TEST(ExpiryWheelDifferential, OwnerSideRefreshExpiresExactlyWhenTheRealDeadlineIs) {
  for (uint64_t seed = 1; seed <= 3; seed++) {
    RunOwnerSideRefresh<ExpiryWheel<uint64_t>>(seed, 0, UINT64_MAX - 100);
    RunOwnerSideRefresh<ExpiryWheel<uint64_t, uint32_t, 6, 5>>(seed, 1, UINT32_MAX - 100);
    RunOwnerSideRefresh<ExpiryWheel<uint64_t, uint16_t, 3, 4>>(seed, 2, UINT16_MAX - 10);
  }
}

// -- no allocation after Create ------------------------------------------------------

// Every allocation Create makes (the node array, the engine), refused in
// turn: kOutOfMemory, and whatever was already allocated is released.
TEST(ExpiryWheel, CreateFailureAtEveryAllocationLeavesNothingAllocated) {
  const size_t points = ForEachFailurePoint([](size_t k) {
    SCOPED_TRACE(::testing::Message() << "failing allocation " << k);
    const AllocationFaults faults(k);
    auto wheel = Wheel::Create(1024, 0, 4);
    if (!faults.injected()) {
      ASSERT_TRUE(wheel.has_value());
      ASSERT_NE(kNoExpiry, (*wheel)->Schedule(100, 1));
      return;
    }
    ASSERT_FALSE(wheel.has_value());
    EXPECT_EQ(ExpiryError::kOutOfMemory, wheel.error());
    EXPECT_EQ(faults.allocations(), faults.frees() + 1)
        << "the refused allocation is the only one not freed";
  });
  EXPECT_EQ(2u, points) << "the node array and the engine";
}

TEST(ExpiryWheel, NothingAllocatesAfterCreate) {
  auto wheel = Make<Wheel>(4096, 0);
  std::mt19937_64 rng(3);
  std::vector<ExpiryHandle> handles;
  handles.reserve(4096);
  uint64_t fired = 0;
  const AllocationFaults window;
  uint64_t now = 0;
  for (int i = 0; i < 200'000; i++) {
    switch (rng() % 5) {
      case 0:
      case 1:
        if (handles.size() < 4096) {
          const auto h = wheel->Schedule(now + rng() % 5'000'000, i);
          if (h != kNoExpiry) handles.push_back(h);
        }
        break;
      case 2:
        if (!handles.empty()) {
          wheel->Refresh(handles[rng() % handles.size()], now + rng() % 100'000);
        }
        break;
      case 3:
        if (!handles.empty()) {
          const size_t k = rng() % handles.size();
          wheel->Cancel(handles[k]);
          handles[k] = handles.back();
          handles.pop_back();
        }
        break;
      default:
        now += rng() % 20'000;
        wheel->Poll(now, 64, [&](const uint64_t &) noexcept { fired++; });
    }
  }
  EXPECT_EQ(0u, window.allocations()) << "a timer operation allocated";
  EXPECT_GT(fired, 0u);
}

}  // namespace
}  // namespace bess::dataplane
