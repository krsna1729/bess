// SPDX-License-Identifier: BSD-3-Clause

// RebuildResource (D-191): an immutable table rebuilt once per transaction and
// published whole, driven through the transaction engine the way a module's
// "<name>/rules" resource is.

#include "dataplane/rebuild_resource.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "runtime/runtime_state.h"
#include "testing/allocation_faults.h"

namespace bess::dataplane {
namespace {

using Outcome = TransactionEngine::Outcome;
using OpStatus = TransactionEngine::OpStatus;

std::atomic<int> live_tables{0};

struct Rule {
  int value = 0;
};

// What a build made: the (id, value) pairs it was given, in the order given.
struct Table {
  Table() { live_tables++; }
  ~Table() { live_tables--; }
  Table(const Table &) = delete;
  Table &operator=(const Table &) = delete;
  std::vector<std::pair<uint32_t, int>> rules;
};

using View = RebuildRules<uint32_t, Rule>;

// Counts its calls; refuses a negative value, naming its rule. The view's
// size must be the number of rules it iterates (a build reserves by it).
struct Builder {
  int *builds;
  std::expected<std::unique_ptr<const Table>, std::string> operator()(
      const View &rules) const {
    ++*builds;
    auto table = std::make_unique<Table>();
    for (const auto &[id, rule] : rules) {
      if (rule->value < 0) {
        return std::unexpected("rule " + std::to_string(id) + " is negative");
      }
      table->rules.emplace_back(id, rule->value);
    }
    EXPECT_EQ(table->rules.size(), rules.size());
    EXPECT_EQ(std::distance(rules.begin(), rules.end()),
              static_cast<std::ptrdiff_t>(rules.size()));
    return table;
  }
};

using TestResource = RebuildResource<uint32_t, Rule, Table, Builder>;
using Pairs = std::vector<std::pair<uint32_t, int>>;

thread_local std::optional<fault_injection::AllocationFaults> t_publish_window;
std::atomic<size_t> g_publish_allocations{0};

class RebuildResourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    resource_ = std::make_unique<TestResource>("tbl", published_, Builder{&builds_}, "a test rule");
    ASSERT_TRUE(resource_->Initialize({{1, Rule{10}}, {2, Rule{20}}}));
    ASSERT_EQ(builds_, 1);
    builds_ = 0;
    ASSERT_TRUE(engine_.Register(resource_.get()));
  }

  void TearDown() override {
    const std::string name = "tbl";
    ASSERT_TRUE(engine_.ReleaseForTeardown(std::span<const std::string>(&name, 1)));
    domain_.Drain();
  }

  static Op Upsert(uint32_t id, int value) {
    return Op::Upsert("tbl", EncodeKey(id), std::any(Rule{value}));
  }
  static Op Erase(uint32_t id) { return Op::Erase("tbl", EncodeKey(id)); }

  TransactionEngine::Result Apply(std::vector<Op> ops) { return engine_.Apply(ops); }

  Pairs Published() const { return published_.Read()->rules; }
  Pairs Committed() const {
    Pairs out;
    for (const auto &[id, rule] : resource_->rules()) {
      out.emplace_back(id, rule.value);
    }
    return out;
  }

  rcu::RcuDomain &domain_ = bess::runtime::runtime().rcu();
  rcu::RcuPtr<Table> published_{domain_};
  int builds_ = 0;
  std::unique_ptr<TestResource> resource_;
  TransactionEngine engine_{domain_};
};

TEST_F(RebuildResourceTest, InitializePublishesTheFirstTable) {
  EXPECT_EQ(Published(), (Pairs{{1, 10}, {2, 20}}));
  EXPECT_EQ(Committed(), (Pairs{{1, 10}, {2, 20}}));
  EXPECT_EQ(resource_->LiveCount(), 2u);
  EXPECT_TRUE(resource_->Contains(EncodeKey(uint32_t{1})));
  EXPECT_FALSE(resource_->Contains(EncodeKey(uint32_t{3})));
  EXPECT_FALSE(resource_->Contains("x")) << "a key of the wrong width";

  rcu::RcuPtr<Table> other(domain_);
  int builds = 0;
  TestResource refused("other", other, Builder{&builds}, "a test rule");
  auto initialized = refused.Initialize({{4, Rule{-1}}});
  ASSERT_FALSE(initialized);
  EXPECT_EQ(initialized.error(), "rule 4 is negative");
  EXPECT_EQ(other.Read(), nullptr) << "nothing published";
  EXPECT_EQ(refused.LiveCount(), 0u);
}

// Every operation of a transaction lands in one build: the committed rules and
// the operations merged in id order, whatever order the request names them in.
TEST_F(RebuildResourceTest, ATransactionOfManyOperationsBuildsOnce) {
  auto r = Apply({Upsert(5, 50), Erase(1), Upsert(3, 30), Upsert(2, 21)});
  ASSERT_EQ(r.outcome, Outcome::kApplied);
  EXPECT_EQ(builds_, 1);
  EXPECT_EQ(Published(), (Pairs{{2, 21}, {3, 30}, {5, 50}}));
  EXPECT_EQ(Committed(), Published());
  EXPECT_EQ(resource_->LiveCount(), 3u);

  // One table built and one retired per transaction, not per operation.
  r = Apply({Erase(2), Erase(3), Erase(5)});
  ASSERT_EQ(r.outcome, Outcome::kApplied);
  EXPECT_EQ(builds_, 2);
  EXPECT_EQ(Published(), Pairs{});
  EXPECT_EQ(resource_->LiveCount(), 0u);
}

// A refused transaction, at a reservation or at the build, leaves the published
// table (the same object) and the committed rules as they were, and the next
// transaction starts clean.
TEST_F(RebuildResourceTest, ARefusedTransactionLeavesThePublishedTableUntouched) {
  const Table *before = published_.Read();
  const Pairs committed = Committed();

  auto r = Apply({Upsert(4, 40), Upsert(6, -1)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[0].status, OpStatus::kNotApplied);
  EXPECT_EQ(r.ops[1].status, OpStatus::kFailed) << "the resource's last operation";
  EXPECT_EQ(r.ops[1].error, "rule 6 is negative");
  EXPECT_EQ(builds_, 1);

  r = Apply({Upsert(4, 40), Erase(9)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[1].error, "not found");
  EXPECT_EQ(builds_, 1) << "refused before the build";

  r = Apply({Op::Upsert("tbl", EncodeKey(uint32_t{4}), std::any(4))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[0].error, "wrong value type (want a test rule)");
  r = Apply({Op::Upsert("tbl", "x", std::any(Rule{4}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[0].error, "invalid rule id key");

  EXPECT_EQ(published_.Read(), before);
  EXPECT_EQ(Committed(), committed);
  EXPECT_EQ(builds_, 1);

  r = Apply({Upsert(4, 40)});
  ASSERT_EQ(r.outcome, Outcome::kApplied);
  EXPECT_EQ(Published(), (Pairs{{1, 10}, {2, 20}, {4, 40}}));
}

// The publish swaps the table and moves the staged nodes: every allocation
// happened in Reserve() and Prepare().
TEST_F(RebuildResourceTest, PublicationDoesNotAllocate) {
  internal::g_publish_window_hook = [](bool entering) {
    if (entering) {
      t_publish_window.emplace();
    } else {
      g_publish_allocations += t_publish_window->allocations();
      t_publish_window.reset();
    }
  };
  g_publish_allocations = 0;
  // Add, replace, erase, and all three in one transaction.
  EXPECT_EQ(Apply({Upsert(3, 30)}).outcome, Outcome::kApplied);
  EXPECT_EQ(Apply({Upsert(3, 31)}).outcome, Outcome::kApplied);
  EXPECT_EQ(Apply({Erase(3)}).outcome, Outcome::kApplied);
  EXPECT_EQ(Apply({Upsert(4, 40), Upsert(1, 11), Erase(2)}).outcome, Outcome::kApplied);
  internal::g_publish_window_hook = nullptr;
  EXPECT_EQ(g_publish_allocations.load(), 0u) << "the publication phase allocated";
  EXPECT_EQ(Published(), (Pairs{{1, 11}, {4, 40}}));
}

// The replaced table stays readable for a reader that loaded it before the
// publish, and is freed once that reader passes a quiescent state.
TEST_F(RebuildResourceTest, TheReplacedTableIsFreedAfterAGracePeriod) {
  constexpr rcu::ReaderId kReader = 7;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);
  const int before = live_tables.load();

  const Table *held = published_.Read();
  ASSERT_EQ(Apply({Upsert(3, 30)}).outcome, Outcome::kApplied);
  EXPECT_NE(published_.Read(), held);
  EXPECT_EQ(held->rules, (Pairs{{1, 10}, {2, 20}})) << "replaced table freed under a reader";
  EXPECT_EQ(live_tables.load(), before + 1);

  domain_.Quiescent(kReader);
  domain_.ReclaimReady();
  EXPECT_EQ(live_tables.load(), before) << "the replaced table is freed";

  domain_.Offline(kReader);
  domain_.Unregister(kReader);
}

// The committed rules read back by id, whatever order they were written in.
TEST_F(RebuildResourceTest, ReadbackIsInIdOrder) {
  ASSERT_EQ(Apply({Upsert(9, 90), Upsert(7, 70)}).outcome, Outcome::kApplied);
  ASSERT_EQ(Apply({Upsert(8, 80)}).outcome, Outcome::kApplied);
  ASSERT_EQ(Apply({Upsert(3, 30), Erase(7)}).outcome, Outcome::kApplied);
  EXPECT_EQ(Committed(), (Pairs{{1, 10}, {2, 20}, {3, 30}, {8, 80}, {9, 90}}));
  EXPECT_EQ(Published(), Committed()) << "the build saw the same order";
}

// Random transactions of upserts (new ids and replacements) and erases, some
// refused by the build, against a std::map: the published table and the
// committed rules always equal the model.
TEST_F(RebuildResourceTest, RandomTransactionsMatchAModel) {
  std::map<uint32_t, int> model = {{1, 10}, {2, 20}};
  std::mt19937 rng(1234);
  for (int round = 0; round < 500; round++) {
    std::vector<Op> ops;
    std::map<uint32_t, int> next = model;
    bool refused = false;
    std::vector<uint32_t> ids(24);
    std::iota(ids.begin(), ids.end(), 1);
    std::shuffle(ids.begin(), ids.end(), rng);
    const size_t n = 1 + rng() % 6;
    for (size_t k = 0; k < n; k++) {
      const uint32_t id = ids[k];
      if (model.contains(id) && rng() % 3 == 0) {
        ops.push_back(Erase(id));
        next.erase(id);
      } else {
        const int value = rng() % 40 == 0 ? -1 : static_cast<int>(rng() % 1000);
        refused |= value < 0;
        ops.push_back(Upsert(id, value));
        next[id] = value;
      }
    }
    const auto outcome = Apply(ops).outcome;
    ASSERT_EQ(outcome, refused ? Outcome::kRejected : Outcome::kApplied) << "round " << round;
    if (!refused) {
      model = std::move(next);
    }
    const Pairs want(model.begin(), model.end());
    ASSERT_EQ(Committed(), want) << "round " << round;
    ASSERT_EQ(Published(), want) << "round " << round;
  }
}

}  // namespace
}  // namespace bess::dataplane
