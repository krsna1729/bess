// SPDX-License-Identifier: BSD-3-Clause

#include "control/resource_fill.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "gtest/gtest.h"
#include "rcu/rcu_domain.h"

namespace bess::control {
namespace {

// A resource whose count and stated capacity the test sets directly. The watch
// reads only LiveCount() and Capacity(); the rest is what Resource requires,
// and Reserve is never called by the watch.
class FillResource final : public dataplane::Resource {
 public:
  FillResource(std::string name, uint64_t *count, std::optional<uint64_t> *capacity,
               std::optional<uint64_t> *level = nullptr)
      : Resource(std::move(name), {}), count_(count), capacity_(capacity), level_(level) {}

  size_t LiveCount() const override { return *count_; }
  std::optional<uint64_t> Capacity() const override { return *capacity_; }
  std::optional<uint64_t> FillLevel() const override {
    return level_ == nullptr ? std::nullopt : *level_;
  }
  bool Contains(const dataplane::ResourceKey &) const override { return false; }
  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &) const override {}
  std::expected<Reservation, std::string> Reserve(const dataplane::Op &) override {
    return std::unexpected("the fill watch never reserves");
  }

 private:
  uint64_t *count_;
  std::optional<uint64_t> *capacity_;
  std::optional<uint64_t> *level_;
};

// A codec that carries no entries: the watch only needs the resource to be
// reachable, which is what a binding means (GetResourceStatus's rule).
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

class ResourceFillWatchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(engine_.Register(&resource_).has_value());
    binding_ = bindings_.Bind(resource_, std::make_shared<StubCodec>());
  }

  uint64_t count_ = 0;
  std::optional<uint64_t> capacity_ = 100;
  std::optional<uint64_t> level_;  // unstated: the daemon's fraction applies
  rcu::RcuDomain domain_{1, 8};
  dataplane::TransactionEngine engine_{domain_};
  FillResource resource_{"mod/table", &count_, &capacity_, &level_};
  framework::ResourceBindings bindings_;
  framework::ResourceBinding binding_;
};

// 100 slots at an 80% level: 79 is below, 80 is the level.
TEST_F(ResourceFillWatchTest, FiresAtTheLevelAndOncePerCrossing) {
  ResourceFillWatch fills(engine_, bindings_);

  count_ = 79;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  count_ = 80;
  const ResourceFillWatch::Result fired = fills.Sweep([] { return false; });
  ASSERT_EQ(1u, fired.crossed.size());
  EXPECT_EQ("mod/table", fired.crossed[0].resource);
  EXPECT_EQ(80u, fired.crossed[0].count);
  EXPECT_EQ(80u, fired.crossed[0].limit);
  EXPECT_EQ(100u, fired.crossed[0].capacity);
  EXPECT_EQ(1u, fills.fired());

  // Still above: reported once per crossing, not once per sweep.
  count_ = 95;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(1u, fills.fired());
}

TEST_F(ResourceFillWatchTest, FiresAgainAfterItFallsBelowAndRises) {
  ResourceFillWatch fills(engine_, bindings_);

  count_ = 90;
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());
  EXPECT_EQ(1u, fills.fired());

  count_ = 10;  // below the level again: it may fire once more
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  count_ = 81;
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());
}

TEST_F(ResourceFillWatchTest, ARisingCountWithoutAFallDoesNotFireTwice) {
  ResourceFillWatch fills(engine_, bindings_);

  count_ = 80;
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());

  // Above the level and never below it: nothing new to report.
  count_ = 100;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
}

TEST_F(ResourceFillWatchTest, AResourceThatStatesNoCapacityIsNotWatched) {
  ResourceFillWatch fills(engine_, bindings_);

  capacity_ = std::nullopt;
  count_ = 1000000;  // as full as a count can be, and still not "full"
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  // And a capacity of zero is nothing to be full of, not an always-full table.
  capacity_ = 0;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());
}

TEST_F(ResourceFillWatchTest, AnUnboundResourceIsNotWatched) {
  framework::ResourceBindings unbound;
  ResourceFillWatch fills(engine_, unbound);

  count_ = 100;
  const ResourceFillWatch::Result skipped = fills.Sweep([] { return false; });
  EXPECT_TRUE(skipped.crossed.empty());
  EXPECT_EQ(0u, skipped.visited);  // the walk skipped it: no binding, no watch
}

TEST_F(ResourceFillWatchTest, AResourceThatIsGoneAndComesBackFiresAgain) {
  ResourceFillWatch fills(engine_, bindings_);

  count_ = 100;
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());
  EXPECT_EQ(1u, fills.fired());

  // Its module unloads: the resource is gone, and the watch forgets it, so a
  // later table of the same name is a new one and not one already fired.
  ASSERT_TRUE(engine_.Unregister("mod/table").has_value());
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  ASSERT_TRUE(engine_.Register(&resource_).has_value());
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());
  EXPECT_EQ(1u, fills.fired());
}

TEST_F(ResourceFillWatchTest, ACapacityOfOneIsNotFullWhenEmpty) {
  ResourceFillWatch fills(engine_, bindings_);

  // A module's settings singleton states a capacity of 1 and holds no entries,
  // so its level is 1: empty must never look full (floor put the level at 0 and
  // every module's settings fired on the first sweep).
  capacity_ = 1;
  count_ = 0;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  count_ = 1;  // a one-entry resource of that capacity is full, and fires
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());
}

TEST_F(ResourceFillWatchTest, AResourceThatBecomesUnreachableAndBackFiresAgain) {
  ResourceFillWatch fills(engine_, bindings_);

  count_ = 100;
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());

  // Still registered and still full, but no longer reachable: releasing the
  // binding makes a complete pass skip it, so it is forgotten like a resource
  // that went away.
  binding_ = framework::ResourceBinding();
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  // Bound again, and still full: reported again. A watch cannot remember a
  // resource it could not see, and a controller that lost sight of it wants to
  // hear where it stands.
  binding_ = bindings_.Bind(resource_, std::make_shared<StubCodec>());
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());
}

TEST_F(ResourceFillWatchTest, ACapacityThatGrowsOrIsWithdrawnRearmsTheResource) {
  ResourceFillWatch fills(engine_, bindings_);

  count_ = 80;  // 80% of 100: over the level
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());

  // The table grows (a bigger table, or a different configuration): 80 of 200
  // is below the level, so the resource is no longer over it.
  capacity_ = 200;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  // And it is reported again when it does reach the new level.
  count_ = 160;
  EXPECT_EQ(1u, fills.Sweep([] { return false; }).crossed.size());

  // A capacity withdrawn entirely (the table no longer states one) is not
  // watched, and does not stay remembered as full.
  capacity_ = std::nullopt;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());
}

TEST_F(ResourceFillWatchTest, AStatedLevelIsTheCountItFiresAt) {
  ResourceFillWatch fills(engine_, bindings_);

  level_ = 12;  // the resource states a count of its own, not a fraction
  count_ = 11;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());

  count_ = 12;
  const ResourceFillWatch::Result fired = fills.Sweep([] { return false; });
  ASSERT_EQ(1u, fired.crossed.size());
  EXPECT_EQ(12u, fired.crossed[0].limit);
  EXPECT_EQ(std::optional<uint64_t>(100), fired.crossed[0].capacity);
  EXPECT_EQ(1u, fills.fired());

  // A count, not a fraction: the daemon's default over this capacity is 80, so
  // 12 would not fire under it. A rise to 79 is the same crossing and is not
  // reported twice.
  count_ = 79;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(1u, fills.fired());
}

TEST_F(ResourceFillWatchTest, AStatedLevelWatchesAResourceThatStatesNoCapacity) {
  ResourceFillWatch fills(engine_, bindings_);

  capacity_ = std::nullopt;  // a tuple space: no ceiling to state
  level_ = 5;
  count_ = 4;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());

  count_ = 5;
  const ResourceFillWatch::Result fired = fills.Sweep([] { return false; });
  ASSERT_EQ(1u, fired.crossed.size());
  EXPECT_EQ(5u, fired.crossed[0].limit);
  EXPECT_FALSE(fired.crossed[0].capacity.has_value());
}

TEST_F(ResourceFillWatchTest, AStatedLevelAboveTheDefaultSuppressesIt) {
  ResourceFillWatch fills(engine_, bindings_);

  level_ = 200;  // above the 80% the default would give for a capacity of 100
  count_ = 100;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  count_ = 200;
  const ResourceFillWatch::Result fired = fills.Sweep([] { return false; });
  ASSERT_EQ(1u, fired.crossed.size());
  EXPECT_EQ(200u, fired.crossed[0].limit);
}

TEST_F(ResourceFillWatchTest, AStatedLevelOfZeroIsReadAsNone) {
  ResourceFillWatch fills(engine_, bindings_);

  // With no capacity either, there is nothing to watch at all.
  capacity_ = std::nullopt;
  level_ = 0;  // would fire for an empty resource and never fall below
  count_ = 0;
  EXPECT_TRUE(fills.Sweep([] { return false; }).crossed.empty());
  EXPECT_EQ(0u, fills.fired());

  // Read as unstated, not as a level of zero: the daemon's fraction applies.
  capacity_ = 100;
  count_ = 80;
  const ResourceFillWatch::Result fired = fills.Sweep([] { return false; });
  ASSERT_EQ(1u, fired.crossed.size());
  EXPECT_EQ(80u, fired.crossed[0].limit);
}

TEST(ResourceFillWatchSlice, BoundedPassesReachTheTailAndHandleRegistryChanges) {
  rcu::RcuDomain domain(1, 8);
  dataplane::TransactionEngine engine(domain);
  framework::ResourceBindings bindings;
  uint64_t count = 100;  // over the level: every one would fire if reached
  std::optional<uint64_t> capacity = 100;
  std::vector<std::unique_ptr<FillResource>> resources;
  std::vector<framework::ResourceBinding> bindings_kept;
  for (size_t i = 0; i < ResourceFillWatch::kStopCheckEvery + 8; i++) {
    resources.push_back(std::make_unique<FillResource>("mod/t" + std::to_string(i), &count, &capacity));
    ASSERT_TRUE(engine.Register(resources.back().get()).has_value());
    bindings_kept.push_back(bindings.Bind(*resources.back(), std::make_shared<StubCodec>()));
  }
  ResourceFillWatch fills(engine, bindings);

  // Every slice is exhausted at its first checkpoint, not only the first.
  const ResourceFillWatch::Result stopped = fills.Sweep([] { return true; });
  EXPECT_TRUE(stopped.incomplete);
  EXPECT_EQ(ResourceFillWatch::kStopCheckEvery, stopped.visited);
  const ResourceFillWatch::Result finished = fills.Sweep([] { return true; });
  EXPECT_FALSE(finished.incomplete);
  EXPECT_EQ(8u, finished.visited);
  std::vector<std::string> reported;
  reported.reserve(stopped.crossed.size() + finished.crossed.size());
  for (const auto &crossing : stopped.crossed) {
    reported.push_back(crossing.resource);
  }
  for (const auto &crossing : finished.crossed) {
    reported.push_back(crossing.resource);
  }
  std::sort(reported.begin(), reported.end());
  EXPECT_EQ(engine.ResourceNames(), reported);
  EXPECT_EQ(ResourceFillWatch::kStopCheckEvery + 8, fills.fired());

  // An incomplete pass must not forget the tail's already-fired history.
  const ResourceFillWatch::Result cut = fills.Sweep([] { return true; });
  ASSERT_TRUE(cut.incomplete);
  EXPECT_TRUE(cut.crossed.empty());
  EXPECT_EQ(ResourceFillWatch::kStopCheckEvery + 8, fills.fired());

  ASSERT_TRUE(engine.Unregister(resources[0]->name()).has_value());
  bindings_kept[0].Reset();
  resources.push_back(std::make_unique<FillResource>("mod/aaa", &count, &capacity));
  ASSERT_TRUE(engine.Register(resources.back().get()).has_value());
  bindings_kept.push_back(bindings.Bind(*resources.back(), std::make_shared<StubCodec>()));
  const ResourceFillWatch::Result tail = fills.Sweep([] { return true; });
  EXPECT_FALSE(tail.incomplete);
  EXPECT_TRUE(tail.crossed.empty());
  EXPECT_EQ(ResourceFillWatch::kStopCheckEvery + 7, fills.fired());
  const ResourceFillWatch::Result next_pass = fills.Sweep([] { return true; });
  ASSERT_EQ(1u, next_pass.crossed.size());
  EXPECT_EQ("mod/aaa", next_pass.crossed[0].resource);
}

TEST(ResourceFillWatchSlice, UnboundResourcesAlsoConsumeTheSlice) {
  rcu::RcuDomain domain(1, 8);
  dataplane::TransactionEngine engine(domain);
  framework::ResourceBindings bindings;
  uint64_t count = 100;
  std::optional<uint64_t> capacity = 100;
  std::vector<std::unique_ptr<FillResource>> resources;
  for (size_t i = 0; i < ResourceFillWatch::kStopCheckEvery + 1; i++) {
    resources.push_back(
        std::make_unique<FillResource>("mod/t" + std::to_string(i), &count, &capacity));
    ASSERT_TRUE(engine.Register(resources.back().get()).has_value());
  }
  ResourceFillWatch fills(engine, bindings);
  const auto first = fills.Sweep([] { return true; });
  EXPECT_TRUE(first.incomplete);
  EXPECT_EQ(0u, first.visited);
  EXPECT_TRUE(first.crossed.empty());
  const auto tail = fills.Sweep([] { return true; });
  EXPECT_FALSE(tail.incomplete);
  EXPECT_EQ(0u, tail.visited);
  EXPECT_TRUE(tail.crossed.empty());
}

TEST(ResourceFillWatchLevel, IsTheFirstCountThatReachesTheLevel) {
  EXPECT_EQ(0u, ResourceFillWatch::LevelOf(0));
  EXPECT_EQ(80u, ResourceFillWatch::LevelOf(100));
  EXPECT_EQ(81u, ResourceFillWatch::LevelOf(101));  // ceil(80.8)
  EXPECT_EQ(52u, ResourceFillWatch::LevelOf(64));   // ceil(51.2)
  EXPECT_EQ(410u, ResourceFillWatch::LevelOf(512)); // ceil(409.6), not 409 (79.9%)
  // A capacity of 1 reaches its level at 1, not at 0: a module's settings
  // singleton (capacity 1, LiveCount always 0) must never look full.
  EXPECT_EQ(1u, ResourceFillWatch::LevelOf(1));
  // The remainder is kept, so the level is the true 80% rounded up and never
  // overflows. UINT64_MAX is divisible by 5, so its 80% is a whole number and
  // rounding up changes nothing.
  EXPECT_EQ(14757395258967641292ULL, ResourceFillWatch::LevelOf(UINT64_MAX));
}

}  // namespace
}  // namespace bess::control
