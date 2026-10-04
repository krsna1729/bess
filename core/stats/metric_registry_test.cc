// SPDX-License-Identifier: BSD-3-Clause

#include "stats/metric_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace bess::stats {
namespace {

TEST(MetricRegistryTest, SourcesAreReadWhenCollectedAndRemovedWithTheirHandle) {
  MetricRegistry registry;
  std::atomic<uint64_t> packets{0};
  {
    MetricSource source = registry.Register([&](MetricWriter &w) {
      w.Counter("bess_test_packets_total", "packets", static_cast<double>(packets.load()),
                {{"worker", "1"}, {"module", "m0"}});
    });
    packets = 5;
    auto samples = registry.Collect();
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0].value, 5);
    EXPECT_EQ(samples[0].kind, MetricKind::kCounter);
    // Labels sorted by name, whatever order the source gave.
    ASSERT_EQ(samples[0].labels.size(), 2u);
    EXPECT_EQ(samples[0].labels[0].first, "module");
    packets = 7;
    EXPECT_EQ(registry.Collect()[0].value, 7) << "read at collection, not registration";
  }
  EXPECT_TRUE(registry.Collect().empty()) << "the handle's destruction removes the source";
  EXPECT_EQ(registry.sources(), 0u);
}

TEST(MetricRegistryTest, SamplesAreSortedByNameThenLabelsAcrossSources) {
  MetricRegistry registry;
  MetricSource b = registry.Register([](MetricWriter &w) {
    w.Gauge("bess_b", "", 1, {{"x", "2"}});
    w.Gauge("bess_a", "", 1);
  });
  MetricSource a = registry.Register([](MetricWriter &w) { w.Gauge("bess_b", "", 1, {{"x", "1"}}); });
  auto s = registry.Collect();
  ASSERT_EQ(s.size(), 3u);
  EXPECT_EQ(s[0].name, "bess_a");
  EXPECT_EQ(s[1].labels[0].second, "1");
  EXPECT_EQ(s[2].labels[0].second, "2");
}

TEST(MetricRegistryTest, MovedAndResetHandles) {
  MetricRegistry registry;
  MetricSource first = registry.Register([](MetricWriter &w) { w.Gauge("bess_x", "", 1); });
  MetricSource moved = std::move(first);
  EXPECT_FALSE(first.registered());
  EXPECT_EQ(registry.sources(), 1u);
  moved = registry.Register([](MetricWriter &w) { w.Gauge("bess_y", "", 2); });
  EXPECT_EQ(registry.sources(), 1u) << "assigning over a handle removes its source";
  EXPECT_EQ(registry.Collect()[0].name, "bess_y");
  moved.Reset();
  EXPECT_EQ(registry.sources(), 0u);
}

// Sources come and go (modules created and destroyed) while the control plane
// collects: no source is called after its handle is gone.
TEST(MetricRegistryTest, CollectingWhileSourcesComeAndGo) {
  MetricRegistry registry;
  std::atomic<bool> stop{false};
  std::atomic<int> calls_after_removal{0};
  std::thread collector([&] {
    while (!stop.load()) {
      (void)registry.Collect();
    }
  });
  for (int i = 0; i < 2000; i++) {
    auto alive = std::make_shared<std::atomic<bool>>(true);
    MetricSource s = registry.Register([alive, &calls_after_removal](MetricWriter &w) {
      if (!alive->load()) {
        calls_after_removal++;
      }
      w.Gauge("bess_churn", "", 1);
    });
    s.Reset();
    alive->store(false);
  }
  stop = true;
  collector.join();
  EXPECT_EQ(calls_after_removal.load(), 0);
}

}  // namespace
}  // namespace bess::stats
