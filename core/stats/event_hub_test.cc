// SPDX-License-Identifier: BSD-3-Clause
// The event hub (M25 phase 2, D-084): bounded worker rings whose losses are
// accounted exactly, per-worker order, gapless sequences, gaps for readers
// that fall behind, and readers woken at close.

#include "stats/event_hub.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace bess::stats {
namespace {

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
    EXPECT_EQ(i < 4, hub.Post(1, e)) << i;  // a ring of 4: the last two refused
  }
  EXPECT_EQ(2u, hub.lost());
  EXPECT_EQ(4u, hub.DrainWorkers());
  WorkerEvent after;
  after.type = t;
  after.values[0] = 99;
  ASSERT_TRUE(hub.Post(1, after));
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
        if (hub.Post(w, e)) accepted++;
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
