// SPDX-License-Identifier: BSD-3-Clause
// Pressure transitions (M25 phase 3, D-089): only changes are events, with
// hysteresis for the RCU backlog, per-direction drop episodes, link changes,
// and a port's first sample as its baseline.

#include "control/pressure_monitor.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "stats/event_hub.h"

namespace bess::control {
namespace {

std::vector<stats::Event> Drain(stats::EventHub &hub, uint64_t &next) {
  auto got = hub.Read(next, 1000, std::chrono::milliseconds(0));
  next = got.next;
  return got.events;
}

TEST(PressureMonitorTest, RcuBacklogIsHighAtHalfTheHighWaterAndClearsBelowAnEighth) {
  stats::EventHub hub;
  PressureMonitor m(hub);
  uint64_t next = 1;
  for (uint64_t pending : {100u, 499u, 500u, 900u, 200u, 125u, 124u, 0u}) {
    m.Observe({pending, 1000, {}});
  }
  const auto events = Drain(hub, next);
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ("bess.rcu_backlog", events[0].type);
  EXPECT_EQ("high", events[0].fields.at("state"));
  EXPECT_EQ("500", events[0].fields.at("pending"));
  EXPECT_EQ("cleared", events[1].fields.at("state"));
  EXPECT_EQ("124", events[1].fields.at("pending"));
}

TEST(PressureMonitorTest, DropsAreEpisodesPerDirectionFromABaseline) {
  stats::EventHub hub;
  PressureMonitor m(hub);
  uint64_t next = 1;
  auto port = [](uint64_t rx, uint64_t tx, bool up = true) {
    return PressureMonitor::Sample{0, 1000, {{"p0", rx, tx, up, 10000}}};
  };
  m.Observe(port(50, 7));   // baseline: earlier drops are not an episode
  m.Observe(port(50, 7));
  EXPECT_TRUE(Drain(hub, next).empty());
  m.Observe(port(50, 10));  // tx starts dropping
  m.Observe(port(50, 30));  // still: no event
  m.Observe(port(52, 30));  // tx clears (episode 23), rx starts
  m.Observe(port(52, 30));  // rx clears
  const auto e = Drain(hub, next);
  ASSERT_EQ(4u, e.size());
  EXPECT_EQ("p0", e[0].source);
  EXPECT_EQ("tx", e[0].fields.at("direction"));
  EXPECT_EQ("dropping", e[0].fields.at("state"));
  EXPECT_EQ("3", e[0].fields.at("dropped"));
  // The order inside one sample: rx first, then tx.
  EXPECT_EQ("rx", e[1].fields.at("direction"));
  EXPECT_EQ("dropping", e[1].fields.at("state"));
  EXPECT_EQ("tx", e[2].fields.at("direction"));
  EXPECT_EQ("cleared", e[2].fields.at("state"));
  EXPECT_EQ("23", e[2].fields.at("dropped"));
  EXPECT_EQ("rx", e[3].fields.at("direction"));
  EXPECT_EQ("cleared", e[3].fields.at("state"));
  // A counter reset is a new baseline, not a negative episode.
  m.Observe(port(0, 0));
  EXPECT_TRUE(Drain(hub, next).empty());
}

TEST(PressureMonitorTest, LinkChangesAreEventsAndARemovedPortStartsOver) {
  stats::EventHub hub;
  PressureMonitor m(hub);
  uint64_t next = 1;
  auto link = [](bool up) { return PressureMonitor::Sample{0, 1000, {{"p1", 0, 0, up, 25000}}}; };
  m.Observe(link(false));  // baseline down: no event
  m.Observe(link(true));
  m.Observe(link(true));
  m.Observe(link(false));
  auto e = Drain(hub, next);
  ASSERT_EQ(2u, e.size());
  EXPECT_EQ("bess.port_link", e[0].type);
  EXPECT_EQ("up", e[0].fields.at("state"));
  EXPECT_EQ("25000", e[0].fields.at("speed_mbps"));
  EXPECT_EQ("down", e[1].fields.at("state"));
  m.Observe({0, 1000, {}});  // p1 removed
  m.Observe(link(true));     // recreated: a baseline again
  EXPECT_TRUE(Drain(hub, next).empty());
}

}  // namespace
}  // namespace bess::control
