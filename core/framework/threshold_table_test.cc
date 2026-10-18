// SPDX-License-Identifier: BSD-3-Clause

#include "framework/threshold_table.h"

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace bess::framework {
namespace {

// A module's counters: key -> total, and how often each was read.
struct Counters {
  std::map<uint64_t, uint64_t> totals;
  std::map<uint64_t, int> reads;

  std::optional<uint64_t> Read(uint64_t key) {
    reads[key]++;
    auto it = totals.find(key);
    if (it == totals.end()) {
      return std::nullopt;
    }
    return it->second;
  }
};

constexpr auto kNeverStop = [] { return false; };
constexpr auto kAlwaysStop = [] { return true; };

ThresholdTable::Result SweepAll(ThresholdTable &t, Counters &c) {
  return t.Sweep([&c](uint64_t k) { return c.Read(k); }, kNeverStop);
}

// One report per arming, at or past the limit, and the entry is gone after.
TEST(ThresholdTableTest, FiresOnceAtTheLimitAndIsRemoved) {
  Counters c;
  c.totals[7] = 9;
  ThresholdTable t;
  t.Arm(7, 10);
  EXPECT_TRUE(SweepAll(t, c).crossed.empty()) << "9 of 10 is not a crossing";
  EXPECT_EQ(1u, t.size());

  c.totals[7] = 10;
  const auto r = SweepAll(t, c);
  ASSERT_EQ(1u, r.crossed.size());
  EXPECT_EQ(7u, r.crossed[0].key);
  EXPECT_EQ(10u, r.crossed[0].value);
  EXPECT_EQ(10u, r.crossed[0].limit);
  EXPECT_EQ(0u, t.size()) << "fired entries are removed";

  c.totals[7] = 500;
  EXPECT_TRUE(SweepAll(t, c).crossed.empty()) << "one report per arming";
}

// Level-triggered: a counter already past its limit when armed fires at the
// next sweep, with the total it has, so no crossing slips between arming and
// counting.
TEST(ThresholdTableTest, ArmingAKeyAlreadyPastItsLimitFiresAtTheNextSweep) {
  Counters c;
  c.totals[3] = 1000;
  ThresholdTable t;
  t.Arm(3, 400);
  const auto r = SweepAll(t, c);
  ASSERT_EQ(1u, r.crossed.size());
  EXPECT_EQ(1000u, r.crossed[0].value);
  EXPECT_EQ(400u, r.crossed[0].limit);
}

TEST(ThresholdTableTest, ArmingAnArmedKeyReplacesItsLimit) {
  Counters c;
  c.totals[1] = 50;
  ThresholdTable t;
  t.Arm(1, 40);
  t.Arm(1, 100);
  EXPECT_EQ(1u, t.size()) << "replaced, not added";
  EXPECT_EQ(100u, t.limit(1));
  EXPECT_TRUE(SweepAll(t, c).crossed.empty()) << "the new limit governs";
  c.totals[1] = 100;
  EXPECT_EQ(1u, SweepAll(t, c).crossed.size());
}

TEST(ThresholdTableTest, DisarmRemovesAnEntryAndSaysWhetherThereWasOne) {
  Counters c;
  c.totals[1] = 99;
  ThresholdTable t;
  t.Arm(1, 10);
  EXPECT_TRUE(t.Disarm(1));
  EXPECT_FALSE(t.Disarm(1));
  EXPECT_FALSE(t.limit(1).has_value());
  EXPECT_TRUE(SweepAll(t, c).crossed.empty());
}

// A rule deleted after it was armed leaves no entry: it is dropped and
// reported, rather than read forever.
TEST(ThresholdTableTest, AKeyTheReadNoLongerKnowsIsDroppedAndReported) {
  Counters c;
  c.totals[1] = 1;
  ThresholdTable t;
  t.Arm(1, 10);
  t.Arm(2, 10);  // never existed in the counters
  const auto r = SweepAll(t, c);
  EXPECT_TRUE(r.crossed.empty());
  ASSERT_EQ(1u, r.gone.size());
  EXPECT_EQ(2u, r.gone[0]);
  EXPECT_EQ(1u, t.size()) << "only the live key stays armed";
  EXPECT_TRUE(SweepAll(t, c).gone.empty()) << "reported once";
}

// The stop condition bounds a sweep: it is looked at every kStopCheckEvery
// entries, and the sweep resumes where it stopped, so a pass over many armed
// keys completes across ticks with every key seen and none reported twice,
// whatever swapping the removals do to the order.
TEST(ThresholdTableTest, AStoppedSweepResumesAndCoversEveryEntryOnce) {
  Counters c;
  ThresholdTable t;
  std::set<uint64_t> should_fire;
  for (uint64_t k = 1; k <= 100; k++) {
    c.totals[k] = (k % 2 == 0) ? 1000 : 1;  // even keys are past their limit
    t.Arm(k, 100);
    if (k % 2 == 0) {
      should_fire.insert(k);
    }
  }
  std::map<uint64_t, int> fired;
  size_t sweeps = 0;
  for (; sweeps < 40 && !should_fire.empty(); sweeps++) {
    const auto r = t.Sweep([&c](uint64_t k) { return c.Read(k); }, kAlwaysStop);
    EXPECT_LE(r.evaluated, ThresholdTable::kStopCheckEvery) << "a sweep stops at its first look";
    for (const auto &x : r.crossed) {
      fired[x.key]++;
      should_fire.erase(x.key);
    }
  }
  EXPECT_TRUE(should_fire.empty()) << "every key past its limit fired";
  EXPECT_LT(sweeps, 40u) << "and within a few passes";
  for (const auto &[key, times] : fired) {
    EXPECT_EQ(1, times) << "key " << key << " reported once";
  }
  EXPECT_EQ(50u, fired.size());
  EXPECT_EQ(50u, t.size()) << "the odd keys, below their limit, stay armed";
  // Round-robin: stopped sweeps keep moving through what is left, so every
  // armed entry is looked at within a few more passes.
  for (int i = 0; i < 10; i++) {
    t.Sweep([&c](uint64_t k) { return c.Read(k); }, kAlwaysStop);
  }
  for (uint64_t k = 1; k <= 100; k += 2) {
    EXPECT_GE(c.reads[k], 1) << "key " << k << " was looked at";
  }
}

// A removal leaves the cursor on the slot the last entry moved into, so that
// entry is read next and none is skipped: pinned by the order of reads, since
// the outcome alone cannot tell (a skipped entry is only read a pass later).
// Seventeen keys, the first sweep stopped after sixteen reads leaves the cursor
// on the last; the next sweep reads it, wraps, fires key 1, and key 17 -- the
// entry that moved into key 1's slot -- is read there before key 2.
TEST(ThresholdTableTest, AFiredEntrysSlotIsReadNextForTheEntryThatMovedIntoIt) {
  Counters c;
  ThresholdTable t;
  for (uint64_t k = 1; k <= 17; k++) {
    c.totals[k] = 1;
    t.Arm(k, 100);
  }
  t.Sweep([&c](uint64_t k) { return c.Read(k); }, kAlwaysStop);  // keys 1..16, none fires

  c.totals[1] = 1000;
  std::vector<uint64_t> seen;
  const auto r = t.Sweep(
      [&](uint64_t k) {
        seen.push_back(k);
        return c.Read(k);
      },
      kAlwaysStop);
  ASSERT_EQ(1u, r.crossed.size());
  EXPECT_EQ(1u, r.crossed[0].key);
  ASSERT_GE(seen.size(), 4u);
  EXPECT_EQ(17u, seen[0]) << "the cursor was left on the last entry";
  EXPECT_EQ(1u, seen[1]) << "then wrapped to the first, which fires";
  EXPECT_EQ(17u, seen[2]) << "the entry that moved into its slot is read there";
  EXPECT_EQ(2u, seen[3]) << "and only then the next";
}

// Even a stop that is true at once lets a sweep evaluate its first batch, so
// a tick always makes progress.
TEST(ThresholdTableTest, ASweepAlwaysEvaluatesItsFirstBatch) {
  Counters c;
  ThresholdTable t;
  for (uint64_t k = 1; k <= 40; k++) {
    c.totals[k] = 0;
    t.Arm(k, 1);
  }
  const auto r = t.Sweep([&c](uint64_t k) { return c.Read(k); }, kAlwaysStop);
  EXPECT_EQ(ThresholdTable::kStopCheckEvery, r.evaluated);
}

// What a module's tick reports: one event per crossing, the shared type, the
// module's name as source, and key, value and limit as fields.
TEST(ThresholdTableTest, ReportCrossingsQueuesTheSharedEvent) {
  Counters c;
  c.totals[42] = 77;
  ThresholdTable t;
  t.Arm(42, 70);
  TickHub hub;
  TickEndpoint tick(hub, {}, [&](TickContext &ctx) {
    ReportCrossings(ctx, "urr0", t.Sweep([&c](uint64_t k) { return c.Read(k); },
                                         [&ctx] { return ctx.Expired(); }));
  });
  EXPECT_EQ(1u, hub.Run(std::chrono::milliseconds(5), 0));
  const std::vector<PendingEvent> events = hub.TakeEvents();
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ(stats::kCounterThresholdEvent, events[0].type);
  EXPECT_EQ("urr0", events[0].source);
  EXPECT_EQ("42", events[0].fields.at("key"));
  EXPECT_EQ("77", events[0].fields.at("value"));
  EXPECT_EQ("70", events[0].fields.at("limit"));
  EXPECT_EQ(0u, t.size());
}

// One event shape from both producers, as the log shows them: the module's
// tick (queued, then appended by the maintenance loop) and a worker's
// CrossingNotifier give the same type and source and the same fields; the
// worker's adds only which plane crossed. A consumer's handler serves both.
TEST(ThresholdTableTest, ATickAndAWorkerReportTheSameEventShape) {
  stats::EventHub log(1024, 4);
  Counters c;
  c.totals[42] = 77;
  ThresholdTable t;
  t.Arm(42, 70);
  TickHub hub;
  TickEndpoint tick(hub, {}, [&](TickContext &ctx) {
    ReportCrossings(ctx, "urr0", t.Sweep([&c](uint64_t k) { return c.Read(k); },
                                         [&ctx] { return ctx.Expired(); }));
  });
  hub.Run(std::chrono::milliseconds(5), 0);
  for (PendingEvent &e : hub.TakeEvents()) {  // what the maintenance loop does
    log.Emit(std::move(e.type), std::move(e.source), std::move(e.fields));
  }
  stats::CrossingNotifier notifier(log, "urr0");
  ASSERT_TRUE(notifier.Note(dataplane::WorkerId(3), 42, 77, 70));
  ASSERT_EQ(1u, log.DrainWorkers());

  const auto got = log.Read(1, 10, std::chrono::milliseconds(0));
  ASSERT_EQ(2u, got.events.size());
  const stats::Event &from_tick = got.events[0];
  const stats::Event &from_worker = got.events[1];
  EXPECT_EQ(from_tick.type, from_worker.type);
  EXPECT_EQ(from_tick.source, from_worker.source);
  std::set<std::string> tick_fields;
  for (const auto &[name, value] : from_tick.fields) {
    tick_fields.insert(name);
    EXPECT_EQ(value, from_worker.fields.at(name)) << name;
  }
  EXPECT_EQ((std::set<std::string>{"key", "value", "limit"}), tick_fields);
  EXPECT_EQ(4u, from_worker.fields.size()) << "the worker path adds only `worker`";
  EXPECT_EQ("3", from_worker.fields.at("worker"));
}

}  // namespace
}  // namespace bess::framework
