// SPDX-License-Identifier: BSD-3-Clause

// ResourceRegistry::ApplyOwned (D-200): a module or library applies
// transactions to the resources it registered, through the registry face a
// plugin gets, and to nothing else. Each owner holds its resource objects; the
// engine stands in for the process's (the tests build their own, with a small
// retire budget, so the budget refusal is reachable).

#include "dataplane/resource_registry.h"

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dataplane/slot_resource.h"
#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"
#include "dataplane/transaction_engine.h"
#include "rcu/rcu_domain.h"

namespace bess::dataplane {
namespace {

struct EntryTag;
using EntryId = StrongId<EntryTag, uint32_t>;

struct Entry {
  uint32_t value = 0;
};

using Owned = ResourceRegistry::OwnedApplyResult;
using EngineOutcome = TransactionEngine::Outcome;

constexpr size_t kIds = 64;

class ApplyOwnedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(registry_.Register(&mine_).has_value());
    ASSERT_TRUE(registry_.Register(&theirs_).has_value());
  }
  void TearDown() override { domain_.Drain(); }

  static Op Set(const std::string &resource, uint32_t id, uint32_t value) {
    return Op::Upsert(resource, EncodeKey(EntryId(id)), std::any(Entry{value}));
  }
  uint64_t Count(EngineOutcome outcome) const {
    return engine_.outcome_counts()[static_cast<size_t>(outcome)];
  }

  // Budget: retire_high_water / 2 = 4 objects per transaction.
  rcu::RcuDomain domain_{1, /*retire_high_water=*/8};
  SlotTable<EntryId, Entry> my_table_{kIds};
  SlotTable<EntryId, Entry> their_table_{kIds};
  SlotResource<EntryId, Entry> mine_{"owner/entries", my_table_};
  SlotResource<EntryId, Entry> theirs_{"other/entries", their_table_};
  TransactionEngine engine_{domain_};
  ResourceRegistry &registry_ = engine_.registry();
  const Resource *const owned_[1] = {&mine_};
};

TEST_F(ApplyOwnedTest, AnOwnerAppliesToItsOwnResource) {
  const uint64_t before = registry_.generation();
  const std::vector<Op> ops = {Set("owner/entries", 1, 10),
                               Set("owner/entries", 2, 20)};
  const Owned result = registry_.ApplyOwned(owned_, ops);
  ASSERT_EQ(result.outcome, Owned::Outcome::kApplied) << result.error;
  EXPECT_EQ(result.generation, before + 1);
  EXPECT_EQ(registry_.generation(), before + 1);
  EXPECT_TRUE(result.error.empty());
  ASSERT_NE(my_table_.Lookup(EntryId(1)), nullptr);
  EXPECT_EQ(my_table_.Lookup(EntryId(1))->value, 10u);
  ASSERT_NE(my_table_.Lookup(EntryId(2)), nullptr);
  EXPECT_EQ(my_table_.Lookup(EntryId(2))->value, 20u);
  // Counted as the control plane's transactions are.
  EXPECT_EQ(Count(EngineOutcome::kApplied), 1u);
}

// One foreign operation refuses the whole transaction, before anything is
// reserved: the owner's own operation in it is not applied either.
TEST_F(ApplyOwnedTest, AnOperationOnAnotherOwnersResourceRejectsTheTransaction) {
  const uint64_t before = registry_.generation();
  const std::vector<Op> ops = {Set("owner/entries", 1, 10),
                               Set("other/entries", 1, 99)};
  const Owned result = registry_.ApplyOwned(owned_, ops);
  ASSERT_EQ(result.outcome, Owned::Outcome::kRejected);
  EXPECT_EQ(result.failed_op, 1u);
  EXPECT_EQ(result.error, "resource 'other/entries' is not owned by the caller");
  EXPECT_FALSE(result.over_budget);
  EXPECT_EQ(result.generation, before);
  EXPECT_EQ(registry_.generation(), before);
  EXPECT_EQ(my_table_.Lookup(EntryId(1)), nullptr);
  EXPECT_EQ(their_table_.Lookup(EntryId(1)), nullptr);
  EXPECT_EQ(Count(EngineOutcome::kRejected), 1u);
  EXPECT_EQ(Count(EngineOutcome::kApplied), 0u);
}

// Ownership is the registered object, not its name: an object of the caller's
// that carries another owner's name owns nothing. The control plane's own
// Apply is unrestricted, as before.
TEST_F(ApplyOwnedTest, TheNameIsNotTheOwnership) {
  SlotTable<EntryId, Entry> decoy_table(kIds);
  SlotResource<EntryId, Entry> decoy("other/entries", decoy_table);
  const Resource *const claimed[] = {&mine_, &decoy};
  const std::vector<Op> ops = {Set("other/entries", 1, 99)};
  const Owned refused = registry_.ApplyOwned(claimed, ops);
  ASSERT_EQ(refused.outcome, Owned::Outcome::kRejected);
  EXPECT_EQ(refused.failed_op, 0u);
  EXPECT_EQ(refused.error, "resource 'other/entries' is not owned by the caller");
  EXPECT_EQ(their_table_.Lookup(EntryId(1)), nullptr);

  const TransactionEngine::Result applied = engine_.Apply(ops);
  ASSERT_EQ(applied.outcome, EngineOutcome::kApplied);
  ASSERT_NE(their_table_.Lookup(EntryId(1)), nullptr);
  EXPECT_EQ(their_table_.Lookup(EntryId(1))->value, 99u);
}

// What is not about ownership is answered as Apply answers it: a name nobody
// registered, and a resource's own refusal, with the operation that failed.
TEST_F(ApplyOwnedTest, OtherRefusalsAreTheEnginesOwn) {
  const std::vector<Op> unknown = {Set("owner/entries", 1, 10),
                                   Set("nobody/entries", 1, 10)};
  const Owned missing = registry_.ApplyOwned(owned_, unknown);
  ASSERT_EQ(missing.outcome, Owned::Outcome::kRejected);
  EXPECT_EQ(missing.failed_op, 1u);
  EXPECT_EQ(missing.error, "unknown resource 'nobody/entries'");

  // Ids are 1..capacity: id 0 is the resource's refusal ("invalid id").
  const std::vector<Op> invalid = {Set("owner/entries", 1, 10),
                                   Set("owner/entries", 0, 10)};
  const Owned refused = registry_.ApplyOwned(owned_, invalid);
  ASSERT_EQ(refused.outcome, Owned::Outcome::kRejected);
  EXPECT_EQ(refused.failed_op, 1u);
  EXPECT_EQ(refused.error, "invalid id");
  EXPECT_EQ(my_table_.Lookup(EntryId(1)), nullptr);
}

// The same retire budget as the control plane's Apply: a transaction that
// retires more than one batch allows is refused as over budget, and the same
// work in smaller transactions applies. Replacing a value retires the old one.
TEST_F(ApplyOwnedTest, TheRetireBudgetIsTheEngines) {
  constexpr uint32_t kCount = 8;  // twice the budget of 4
  std::vector<Op> seed;
  std::vector<Op> replace;
  for (uint32_t id = 1; id <= kCount; id++) {
    seed.push_back(Set("owner/entries", id, id));
    replace.push_back(Set("owner/entries", id, id + 100));
  }
  // Fresh ids retire nothing.
  ASSERT_EQ(registry_.ApplyOwned(owned_, seed).outcome, Owned::Outcome::kApplied);

  const Owned over = registry_.ApplyOwned(owned_, replace);
  ASSERT_EQ(over.outcome, Owned::Outcome::kRejected);
  EXPECT_TRUE(over.over_budget);
  EXPECT_NE(over.error.find("split it"), std::string::npos) << over.error;
  ASSERT_NE(my_table_.Lookup(EntryId(1)), nullptr);
  EXPECT_EQ(my_table_.Lookup(EntryId(1))->value, 1u);

  // Each half fits; the domain is drained between them so the first half's
  // retirements do not make the second answer kBusy.
  const size_t half = kCount / 2;
  const std::span<const Op> all(replace);
  EXPECT_EQ(registry_.ApplyOwned(owned_, all.first(half)).outcome,
            Owned::Outcome::kApplied);
  domain_.Drain();
  EXPECT_EQ(registry_.ApplyOwned(owned_, all.last(half)).outcome,
            Owned::Outcome::kApplied);
  EXPECT_EQ(my_table_.Lookup(EntryId(kCount))->value, kCount + 100);
}

}  // namespace
}  // namespace bess::dataplane
