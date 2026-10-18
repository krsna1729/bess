// SPDX-License-Identifier: BSD-3-Clause
// The event hub (M25 phase 2, D-084): bounded worker rings whose losses are
// accounted exactly, per-worker order, gapless sequences, gaps for readers
// that fall behind, and readers woken at close.

#include "stats/crossing_notifier.h"
#include "stats/event_hub.h"
#include "stats/event_throttle.h"

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>
#include <utility>
#include <thread>

namespace bess::stats {
namespace {

using dataplane::WorkerId;
using std::chrono::milliseconds;

TEST(EventHubTest, WorkerEventsBecomeTypedEventsInOrderAndLossIsCounted) {
  EventHub hub(1024, 4);
  const uint32_t t = hub.RegisterType("table.pressure", {"size", "capacity"}, "nat");
  EXPECT_EQ(t, hub.RegisterType("table.pressure", {"size", "capacity"}, "nat")) << "idempotent";
  for (uint64_t i = 0; i < 6; i++) {
    WorkerEvent e;
    e.type = t;
    e.source = 7;
    e.values[0] = i;
    e.values[1] = 100;
    EXPECT_EQ(i < 4, hub.Post(WorkerId(1), e)) << i;  // a ring of 4: the last two refused
  }
  EXPECT_EQ(2u, hub.lost());
  EXPECT_EQ(4u, hub.DrainWorkers());
  WorkerEvent after;
  after.type = t;
  after.values[0] = 99;
  ASSERT_TRUE(hub.Post(WorkerId(1), after));
  EXPECT_EQ(1u, hub.DrainWorkers());
  const auto got = hub.Read(1, 100, milliseconds(0));
  ASSERT_EQ(6u, got.events.size()) << "4 + a loss marker + 1";
  for (uint64_t i = 0; i < 4; i++) {
    EXPECT_EQ(i + 1, got.events[i].sequence);
    EXPECT_EQ("table.pressure", got.events[i].type);
    EXPECT_EQ("nat7", got.events[i].source);
    EXPECT_EQ(std::to_string(i), got.events[i].fields.at("size"));
    EXPECT_EQ("1", got.events[i].fields.at("worker"));
  }
  EXPECT_EQ("bess.events_lost", got.events[4].type);
  EXPECT_EQ("2", got.events[4].fields.at("count"));
  EXPECT_EQ("99", got.events[5].fields.at("size"));
  EXPECT_EQ(7u, got.next);
}

// A recurring packet-path condition (D-089): the first occurrence is an event
// at once, the rest of the interval is counted into the next one, per worker,
// under the poster's own name; a post the ring refuses keeps its count.
TEST(EventThrottleTest, OneEventPerIntervalPerWorkerCarryingTheCount) {
  EventHub hub(1024, 2);
  EventThrottle full(hub, "bess.table_full", "nat0", 1000);
  full.Note(WorkerId(0), 10, 3);    // posted: 3
  full.Note(WorkerId(0), 500, 2);   // counted
  full.Note(WorkerId(1), 600, 1);   // another worker: posted: 1
  full.Note(WorkerId(0), 1009, 4);  // inside the interval still: counted
  full.Note(WorkerId(0), 1010, 1);  // posted: 2 + 4 + 1
  full.Note(WorkerId(0), 1011, 5);  // counted
  ASSERT_EQ(3u, hub.DrainWorkers());
  const auto got = hub.Read(1, 100, milliseconds(0));
  ASSERT_EQ(3u, got.events.size());
  EXPECT_EQ("bess.table_full", got.events[0].type);
  EXPECT_EQ("nat0", got.events[0].source);
  std::multiset<std::pair<std::string, std::string>> seen;
  for (const Event &e : got.events) seen.insert({e.fields.at("worker"), e.fields.at("count")});
  EXPECT_EQ((std::multiset<std::pair<std::string, std::string>>{{"0", "3"}, {"0", "7"}, {"1", "1"}}),
            seen);
  // Worker 2's ring (2 slots) full: the third keeps its count, and is not
  // counted as lost (nothing was).
  EventThrottle other(hub, "bess.queue_full", "q0", 0);
  const uint64_t lost = hub.lost();
  for (uint64_t now = 1; now <= 3; now++) other.Note(WorkerId(2), now, 10);
  EXPECT_EQ(lost, hub.lost());
  ASSERT_EQ(2u, hub.DrainWorkers());
  other.Note(WorkerId(2), 4, 1);
  ASSERT_EQ(1u, hub.DrainWorkers());
  const auto later = hub.Read(4, 100, milliseconds(0));
  ASSERT_FALSE(later.events.empty());
  EXPECT_EQ("11", later.events.back().fields.at("count")) << "the refused 10 + 1";
}

// A growing count passes a limit exactly once, whatever the step: the packet
// path posts one event per arming, not one per packet past the limit.
TEST(CrossingNotifierTest, ACountPassesALimitExactlyOnceAtAnyStepSize) {
  for (const uint64_t step : {1u, 7u, 64u, 1500u}) {
    uint64_t count = 0;
    int crossings = 0;
    while (count < 20000) {
      const uint64_t before = count;
      count += step;
      crossings += CrossingNotifier::Crossed(before, count, 1000) ? 1 : 0;
    }
    EXPECT_EQ(1, crossings) << "step " << step;
  }
  EXPECT_FALSE(CrossingNotifier::Crossed(1000, 1001, 1000)) << "already at the limit: no new crossing";
  EXPECT_FALSE(CrossingNotifier::Crossed(2000, 2001, 1000)) << "already past it";
  EXPECT_TRUE(CrossingNotifier::Crossed(999, 1000, 1000)) << "reaching it is crossing it";
  EXPECT_FALSE(CrossingNotifier::Crossed(0, 5, 0)) << "a zero limit is never crossed by counting";
}

// For a key spread across W planes, WorkerLimit is the per-worker limit that
// cannot miss: whatever the planes hold, if they sum to the total one of them
// is at or past it. Exhaustive over three workers and every split.
TEST(CrossingNotifierTest, WorkerLimitCannotMissACrossingOfTheTotal) {
  constexpr uint64_t kTotal = 7;
  const uint64_t limit = CrossingNotifier::WorkerLimit(kTotal, 3);
  EXPECT_EQ(3u, limit);
  for (uint64_t a = 0; a <= kTotal; a++) {
    for (uint64_t b = 0; b <= kTotal; b++) {
      for (uint64_t c = 0; c <= kTotal; c++) {
        if (a + b + c >= kTotal) {
          EXPECT_TRUE(a >= limit || b >= limit || c >= limit) << a << "," << b << "," << c;
        }
      }
    }
  }
  // And it is the least such limit: one lower would fire with the sum still
  // short, which is the hint the doc names; one higher could miss.
  for (uint32_t workers = 1; workers <= 9; workers++) {
    for (uint64_t total = 1; total <= 200; total++) {
      const uint64_t w = CrossingNotifier::WorkerLimit(total, workers);
      EXPECT_LT((w - 1) * workers, total) << total << " over " << workers;
      EXPECT_LE(total, w * workers) << total << " over " << workers;
    }
  }
  EXPECT_EQ(CrossingNotifier::WorkerLimit(10, 1), CrossingNotifier::WorkerLimit(10, 0))
      << "no workers is read as one";
}

// What a crossing becomes in the log: the shared type, the module's name as
// source, the key, the count when it crossed, the limit, and the worker.
TEST(CrossingNotifierTest, ACrossingBecomesTheSharedEventWithItsWorker) {
  EventHub hub(1024, 4);
  CrossingNotifier crossed(hub, "urr0");
  ASSERT_TRUE(crossed.Note(WorkerId(2), 42, 1001, 1000));
  ASSERT_EQ(1u, hub.DrainWorkers());
  const auto got = hub.Read(1, 10, milliseconds(0));
  ASSERT_EQ(1u, got.events.size());
  EXPECT_EQ(kCounterThresholdEvent, got.events[0].type);
  EXPECT_EQ("urr0", got.events[0].source);
  EXPECT_EQ("42", got.events[0].fields.at("key"));
  EXPECT_EQ("1001", got.events[0].fields.at("value"));
  EXPECT_EQ("1000", got.events[0].fields.at("limit"));
  EXPECT_EQ("2", got.events[0].fields.at("worker"));
}

// A module reporting under a name of its own keeps the same fields.
TEST(CrossingNotifierTest, ATypeOfItsOwnKeepsTheSameFields) {
  EventHub hub(1024, 4);
  CrossingNotifier crossed(hub, "my.threshold", "mod");
  ASSERT_TRUE(crossed.Note(WorkerId(0), 1, 2, 3));
  ASSERT_EQ(1u, hub.DrainWorkers());
  const auto got = hub.Read(1, 10, milliseconds(0));
  ASSERT_EQ(1u, got.events.size());
  EXPECT_EQ("my.threshold", got.events[0].type);
  EXPECT_EQ("1", got.events[0].fields.at("key"));
  EXPECT_EQ("2", got.events[0].fields.at("value"));
  EXPECT_EQ("3", got.events[0].fields.at("limit"));
}

// Never blocks: a worker whose ring is full is refused, and the hub counts it.
TEST(CrossingNotifierTest, AFullRingRefusesAndCounts) {
  EventHub hub(1024, 4);
  CrossingNotifier crossed(hub, "urr0");
  for (uint64_t i = 0; i < 4; i++) {
    EXPECT_TRUE(crossed.Note(WorkerId(1), i, 10, 10)) << i;
  }
  EXPECT_FALSE(crossed.Note(WorkerId(1), 4, 10, 10)) << "the ring of 4 is full";
  EXPECT_EQ(1u, hub.lost());
}

// A module that never opened one runs no event code: a default notifier posts
// nothing and says so.
TEST(CrossingNotifierTest, ADefaultNotifierIsInert) {
  CrossingNotifier none;
  EXPECT_FALSE(none.Note(WorkerId(0), 1, 2, 3));
}

TEST(EventHubTest, AReaderBehindTheLogGetsAGapThenWhatIsHeld) {
  EventHub hub(8, 4);
  for (int i = 0; i < 20; i++) hub.Emit("x", "s", {{"i", std::to_string(i)}});
  const auto got = hub.Read(3, 100, milliseconds(0));
  EXPECT_EQ(3u, got.gap_from);
  EXPECT_EQ(13u, got.gap_to);
  ASSERT_EQ(8u, got.events.size());
  EXPECT_EQ(13u, got.events.front().sequence);
  EXPECT_EQ(20u, got.events.back().sequence);
  EXPECT_EQ(21u, got.next);
  const auto tail = hub.Read(got.next, 100, milliseconds(0));
  EXPECT_TRUE(tail.events.empty());
  EXPECT_EQ(0u, tail.gap_to);
}

TEST(EventHubTest, AWaitingReaderWakesForAnEventAndForClose) {
  EventHub hub;
  std::atomic<bool> woke{false};
  std::thread reader([&] {
    const auto got = hub.Read(0, 10, milliseconds(10000));
    EXPECT_EQ(1u, got.events.size());
    woke = true;
  });
  std::this_thread::sleep_for(milliseconds(20));
  hub.Emit("x", "s", {});
  reader.join();
  EXPECT_TRUE(woke.load());
  std::thread closing([&] {
    const auto got = hub.Read(0, 10, milliseconds(10000));
    EXPECT_TRUE(got.closed);
  });
  std::this_thread::sleep_for(milliseconds(20));
  hub.Close();
  closing.join();
}

// Four workers post while the control side drains: every accepted event is
// in the log once, per-worker order holds, and accepted + lost = posted.
TEST(EventHubTest, ConcurrentPostersAndDrainLoseNothingUncounted) {
  EventHub hub(1 << 20, 64);
  const uint32_t t = hub.RegisterType("w", {"n"});
  constexpr int kWorkers = 4, kEach = 20000;
  std::atomic<int> done{0};
  std::atomic<uint64_t> accepted{0};
  std::vector<std::thread> workers;
  for (int w = 0; w < kWorkers; w++) {
    workers.emplace_back([&, w] {
      for (int i = 0; i < kEach; i++) {
        WorkerEvent e;
        e.type = t;
        e.values[0] = static_cast<uint64_t>(i);
        if (hub.Post(*dataplane::WorkerIdFrom(w), e)) accepted++;
      }
      done++;
    });
  }
  while (done.load() < kWorkers) hub.DrainWorkers();
  hub.DrainWorkers();
  for (auto &th : workers) th.join();
  EXPECT_EQ(accepted.load() + hub.lost(), uint64_t{kWorkers} * kEach);
  const auto got = hub.Read(1, 1 << 20, milliseconds(0));
  std::vector<int64_t> last(kWorkers, -1);
  uint64_t events = 0;
  for (const auto &e : got.events) {
    if (e.type != "w") continue;
    const int w = std::stoi(e.fields.at("worker"));
    const int64_t n = std::stoll(e.fields.at("n"));
    EXPECT_GT(n, last[w]) << "per-worker order";
    last[w] = n;
    events++;
  }
  EXPECT_EQ(accepted.load(), events);
}

}  // namespace
}  // namespace bess::stats
