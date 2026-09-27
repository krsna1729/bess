// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// The dataplane transaction engine (G1.2b, Decision D-021): all-or-nothing,
// dependency-ordered publication, reference safety, per-operation results,
// optimistic concurrency, and retirement after one grace period -- over the
// two ready-made resources (SlotResource, ExactRuleResource) the way a module
// would wire them: rules name actions, actions name meters.

#include "dataplane/transaction_engine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "classifier/exact_rule_resource.h"
#include "control/runtime_state.h"
#include "dataplane/slot_resource.h"
#include "dataplane/strong_id.h"

namespace bess::dataplane {
namespace {

using classifier::ConcurrentExactTable;
using classifier::ExactRuleResource;

struct MeterTag;
struct ActionTag;
using MeterId = StrongId<MeterTag, uint32_t>;
using ActionId = StrongId<ActionTag, uint32_t>;

std::atomic<int> live_meters{0};

struct Meter {
  uint32_t rate = 0;
  explicit Meter(uint32_t r = 0) : rate(r) { live_meters++; }
  Meter(const Meter &o) : rate(o.rate) { live_meters++; }
  ~Meter() { live_meters--; }
};

struct Action {
  uint16_t gate = 0;
  MeterId meter;  // 0: none
};

constexpr size_t kIds = 64;

class TransactionEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto t = ConcurrentExactTable::Create(
        8, ConcurrentExactTable::CapacityFor(512), domain_);
    ASSERT_TRUE(t.has_value()) << t.error();
    table_ = std::move(*t);
    meters_res_ = std::make_unique<SlotResource<MeterId, Meter>>(
        "meters", 0, meters_);
    actions_res_ = std::make_unique<SlotResource<ActionId, Action>>(
        "actions", 1, actions_, [](const Action &a) {
          std::vector<Reference> refs;
          if (a.meter.value() != 0) {
            refs.push_back({"meters", EncodeKey(a.meter)});
          }
          return refs;
        });
    rules_res_ = std::make_unique<ExactRuleResource>(
        "rules", 2, *table_, [](uint64_t action) {
          return std::vector<Reference>{
              {"actions", EncodeKey(ActionId(static_cast<uint32_t>(action)))}};
        });
    ASSERT_TRUE(engine_.Register(meters_res_.get()));
    ASSERT_TRUE(engine_.Register(actions_res_.get()));
    ASSERT_TRUE(engine_.Register(rules_res_.get()));
  }

  void TearDown() override {
    domain_.Drain();
  }

  static Op Rule(uint64_t key, uint64_t action) {
    return Op::Upsert("rules", EncodeKey(key), std::any(action));
  }
  static Op EraseRule(uint64_t key) {
    return Op::Erase("rules", EncodeKey(key));
  }
  static Op Act(uint32_t id, uint16_t gate, uint32_t meter) {
    return Op::Upsert("actions", EncodeKey(ActionId(id)),
                      std::any(Action{gate, MeterId(meter)}));
  }
  static Op EraseAct(uint32_t id) {
    return Op::Erase("actions", EncodeKey(ActionId(id)));
  }
  static Op Met(uint32_t id, uint32_t rate) {
    return Op::Upsert("meters", EncodeKey(MeterId(id)), std::any(Meter(rate)));
  }
  static Op EraseMet(uint32_t id) {
    return Op::Erase("meters", EncodeKey(MeterId(id)));
  }

  std::optional<uint64_t> RuleValue(uint64_t key) const {
    uint64_t v = 0;
    const auto bytes = EncodeKey(key);
    if (table_->LookupBatch(
            classifier::ConstBytes(
                reinterpret_cast<const classifier::Byte *>(bytes.data()), 8),
            8, &v, 1) == 0) {
      return std::nullopt;
    }
    return v;
  }

  TransactionEngine::Result Apply(std::vector<Op> ops,
                                  std::optional<uint64_t> gen = {}) {
    return engine_.Apply(ops, gen);
  }

  rcu::RcuDomain &domain_ = control::runtime().rcu();
  std::unique_ptr<ConcurrentExactTable> table_;
  SlotTable<MeterId, Meter> meters_{kIds};
  SlotTable<ActionId, Action> actions_{kIds};
  std::unique_ptr<SlotResource<MeterId, Meter>> meters_res_;
  std::unique_ptr<SlotResource<ActionId, Action>> actions_res_;
  std::unique_ptr<ExactRuleResource> rules_res_;
  TransactionEngine engine_{domain_};
};

using Outcome = TransactionEngine::Outcome;
using OpStatus = TransactionEngine::OpStatus;

TEST_F(TransactionEngineTest, AppliesADependentSetGivenInAnyOrder) {
  // Referrers first in the request; the engine publishes referents first.
  auto r = Apply({Rule(10, 5), Act(5, 3, 7), Met(7, 1000)});
  ASSERT_EQ(r.outcome, Outcome::kApplied);
  EXPECT_EQ(r.generation, 1u);
  ASSERT_EQ(r.ops.size(), 3u);
  for (const auto &op : r.ops) {
    EXPECT_EQ(op.status, OpStatus::kApplied);
  }
  EXPECT_EQ(RuleValue(10), 5u);
  ASSERT_NE(actions_.Lookup(ActionId(5)), nullptr);
  EXPECT_EQ(actions_.Lookup(ActionId(5))->gate, 3);
  EXPECT_EQ(meters_.Lookup(MeterId(7))->rate, 1000u);
  EXPECT_EQ(engine_.ReferenceCount("actions", EncodeKey(ActionId(5))), 1u);
  EXPECT_EQ(engine_.ReferenceCount("meters", EncodeKey(MeterId(7))), 1u);
}

// A test resource that records the order in which operations publish.
class Recorder final : public Resource {
 public:
  Recorder(std::string name, int rank, std::vector<std::string> *log)
      : Resource(std::move(name), rank), log_(log) {}
  bool Contains(const ResourceKey &key) const override {
    return keys_.contains(key);
  }
  std::expected<Reservation, std::string> Reserve(const Op &op) override {
    struct Staged final : StagedOp {
      Recorder *r;
      Op op;
      Staged(Recorder *rec, Op o) : r(rec), op(std::move(o)) {}
      void Publish(Retirer &) noexcept override {
        r->log_->push_back((op.kind == OpKind::kUpsert ? "+" : "-") +
                           r->name());
        if (op.kind == OpKind::kUpsert) {
          r->keys_.insert(op.key);
        } else {
          r->keys_.erase(op.key);
        }
      }
    };
    return Reservation{std::make_unique<Staged>(this, op), {}};
  }

 private:
  std::vector<std::string> *log_;
  std::set<ResourceKey> keys_;
};

TEST_F(TransactionEngineTest, PublishesUpsertsReferentsFirstAndErasesReferrersFirst) {
  std::vector<std::string> log;
  Recorder a("a", 0, &log), b("b", 1, &log), c("c", 2, &log);
  TransactionEngine engine(domain_);
  ASSERT_TRUE(engine.Register(&a) && engine.Register(&b) &&
              engine.Register(&c));
  std::vector<Op> ops = {Op::Upsert("c", "k", std::any(1)),
                         Op::Upsert("a", "k", std::any(1)),
                         Op::Upsert("b", "k", std::any(1))};
  ASSERT_EQ(engine.Apply(ops).outcome, Outcome::kApplied);
  EXPECT_EQ(log, (std::vector<std::string>{"+a", "+b", "+c"}));

  log.clear();
  ops = {Op::Erase("a", "k"), Op::Upsert("b", "j", std::any(1)),
         Op::Erase("c", "k"), Op::Erase("b", "k")};
  ASSERT_EQ(engine.Apply(ops).outcome, Outcome::kApplied);
  EXPECT_EQ(log, (std::vector<std::string>{"+b", "-c", "-b", "-a"}));
}

TEST_F(TransactionEngineTest, MissingReferentRejectsAndNothingIsVisible) {
  auto r = Apply({Met(7, 1000), Rule(10, 5)});  // action 5 does not exist
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.generation, 0u);
  EXPECT_EQ(r.ops[0].status, OpStatus::kNotApplied);
  EXPECT_EQ(r.ops[1].status, OpStatus::kFailed);
  EXPECT_NE(r.ops[1].error.find("references missing actions/"),
            std::string::npos)
      << r.ops[1].error;
  EXPECT_EQ(meters_.Lookup(MeterId(7)), nullptr) << "a rejected transaction "
                                                    "published a meter";
  EXPECT_FALSE(RuleValue(10).has_value());
}

TEST_F(TransactionEngineTest, ReferencedKeyCannotBeErasedAlone) {
  ASSERT_EQ(Apply({Met(7, 1), Act(5, 1, 7), Rule(10, 5)}).outcome,
            Outcome::kApplied);
  auto r = Apply({EraseAct(5)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("still referenced 1 time"), std::string::npos)
      << r.ops[0].error;
  EXPECT_NE(actions_.Lookup(ActionId(5)), nullptr);

  // Re-pointing the only referrer frees it within the same transaction.
  r = Apply({Act(6, 2, 0), Rule(10, 6), EraseAct(5)});
  ASSERT_EQ(r.outcome, Outcome::kApplied)
      << r.ops[0].error << "|" << r.ops[1].error << "|" << r.ops[2].error;
  EXPECT_EQ(RuleValue(10), 6u);
  EXPECT_FALSE(actions_.Contains(ActionId(5)));
  EXPECT_EQ(engine_.ReferenceCount("meters", EncodeKey(MeterId(7))), 0u);

  // Removing a referrer and its referents together is fine in any order.
  ASSERT_EQ(Apply({EraseMet(7), EraseAct(6), EraseRule(10)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(table_->size(), 0u);
}

TEST_F(TransactionEngineTest, StructuralErrorsAreReportedPerOperation) {
  auto r = Apply({Met(1, 1), Op::Upsert("nope", "k", std::any(1))});
  EXPECT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[1].status, OpStatus::kFailed);
  EXPECT_NE(r.ops[1].error.find("unknown resource"), std::string::npos);

  r = Apply({Met(1, 1), Met(1, 2)});
  EXPECT_EQ(r.ops[1].status, OpStatus::kFailed);
  EXPECT_NE(r.ops[1].error.find("second operation"), std::string::npos);

  r = Apply({Op::Upsert("meters", EncodeKey(MeterId(1)), std::any(3.5))});
  EXPECT_NE(r.ops[0].error.find("wrong value type"), std::string::npos);

  r = Apply({Op::Upsert("meters", EncodeKey(MeterId(kIds + 1)),
                        std::any(Meter(1)))});
  EXPECT_NE(r.ops[0].error.find("invalid id"), std::string::npos);

  r = Apply({EraseMet(3)});
  EXPECT_NE(r.ops[0].error.find("not found"), std::string::npos);
  EXPECT_EQ(meters_.size(), 0u);
  EXPECT_EQ(engine_.generation(), 0u);
}

TEST_F(TransactionEngineTest, FullTableRejectsTheWholeTransaction) {
  ASSERT_EQ(Apply({Act(1, 1, 0)}).outcome, Outcome::kApplied);
  const size_t fits = table_->capacity() -
                      ConcurrentExactTable::Headroom(table_->capacity());
  std::vector<Op> ops = {Met(2, 1)};
  for (uint64_t k = 0; k <= fits; k++) {  // one more than fits
    ops.push_back(Rule(1000 + k, 1));
  }
  auto r = Apply(ops);
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops.back().status, OpStatus::kFailed);
  EXPECT_NE(r.ops.back().error.find("table full"), std::string::npos);
  EXPECT_EQ(table_->size(), 0u);
  EXPECT_EQ(meters_.Lookup(MeterId(2)), nullptr);

  // The reservation count was reset: exactly `fits` keys now go in.
  ops.pop_back();
  EXPECT_EQ(Apply(ops).outcome, Outcome::kApplied);
  EXPECT_EQ(table_->size(), fits);
}

TEST_F(TransactionEngineTest, ExpectedGenerationConflictAttemptsNothing) {
  ASSERT_EQ(Apply({Met(1, 1)}).generation, 1u);
  auto r = Apply({Met(2, 1)}, /*gen=*/0);
  EXPECT_EQ(r.outcome, Outcome::kConflict);
  EXPECT_EQ(r.generation, 1u);
  EXPECT_EQ(meters_.Lookup(MeterId(2)), nullptr);
  EXPECT_EQ(Apply({Met(2, 1)}, 1).outcome, Outcome::kApplied);
}

// Replaced and erased objects outlive every reader that could hold them, and
// an erased id is not reused until then.
TEST_F(TransactionEngineTest, RetirementWaitsForReadersAndIdsAreNotReusedEarly) {
  constexpr rcu::ReaderId kReader = 7;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);
  const int before = live_meters.load();

  ASSERT_EQ(Apply({Met(1, 100)}).outcome, Outcome::kApplied);
  const Meter *held = meters_.Lookup(MeterId(1));  // a reader holds it
  ASSERT_EQ(Apply({Met(1, 200)}).outcome, Outcome::kApplied);
  EXPECT_EQ(meters_.Lookup(MeterId(1))->rate, 200u);
  EXPECT_EQ(held->rate, 100u) << "replaced object freed under a reader";

  ASSERT_EQ(Apply({EraseMet(1)}).outcome, Outcome::kApplied);
  auto r = Apply({Met(1, 300)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("still retiring"), std::string::npos);
  EXPECT_EQ(live_meters.load(), before + 2) << "both old meters still alive";

  domain_.Quiescent(kReader);
  domain_.ReclaimReady();
  EXPECT_EQ(live_meters.load(), before + 1)
      << "the replaced meter is freed; the erased one waits for its slot to "
         "be emptied";
  // The next control call empties the slot and frees the id.
  EXPECT_EQ(Apply({Met(1, 300)}).outcome, Outcome::kApplied);
  EXPECT_EQ(meters_.Lookup(MeterId(1))->rate, 300u);
  domain_.Quiescent(kReader);
  domain_.ReclaimReady();
  EXPECT_EQ(live_meters.load(), before + 1) << "only the new meter is alive";

  domain_.Offline(kReader);
  domain_.Unregister(kReader);
}

// The reader side of reference safety, deterministically: a packet read a
// rule's old value (action 5) just before a transaction re-pointed the rule and
// erased action 5. Its lookup of action 5 must still succeed until it has
// passed a quiescent state; only then does the slot empty.
TEST_F(TransactionEngineTest, ErasedReferentStaysReadableForReadersHoldingItsId) {
  constexpr rcu::ReaderId kReader = 8;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);
  ASSERT_EQ(Apply({Act(5, 1, 0), Rule(10, 5)}).outcome, Outcome::kApplied);
  const uint64_t held = *RuleValue(10);  // the packet's read of the rule

  ASSERT_EQ(Apply({Act(6, 2, 0), Rule(10, 6), EraseAct(5)}).outcome,
            Outcome::kApplied);
  EXPECT_FALSE(actions_.Contains(ActionId(5)));
  const Action *a = actions_.Lookup(ActionId(static_cast<uint32_t>(held)));
  ASSERT_NE(a, nullptr) << "a reader holding the old id found nothing";
  EXPECT_EQ(a->gate, 1);

  domain_.Quiescent(kReader);
  engine_.ReclaimRetired();  // what the next control call does
  EXPECT_EQ(actions_.Lookup(ActionId(5)), nullptr);
  domain_.Offline(kReader);
  domain_.Unregister(kReader);
}

// Random transactions against a model of what should be accepted and what
// the state should be afterwards; the invariants (no dangling reference,
// exact reference counts) are checked after every transaction.
TEST_F(TransactionEngineTest, RandomTransactionsMatchAModel) {
  std::mt19937 rng(1234);
  std::map<uint32_t, uint32_t> meters;              // id -> rate
  std::map<uint32_t, uint32_t> actions;             // id -> meter (0 none)
  std::map<uint64_t, uint32_t> rules;               // key -> action
  size_t applied = 0, rejected = 0;

  for (int t = 0; t < 3000; t++) {
    std::vector<Op> ops;
    std::set<std::pair<int, uint64_t>> used;
    auto m = meters;
    auto a = actions;
    auto rl = rules;
    const int n = 1 + static_cast<int>(rng() % 6);
    for (int i = 0; i < n; i++) {
      const int kind = static_cast<int>(rng() % 6);
      const uint32_t id = 1 + rng() % 8;
      const uint64_t key = rng() % 12;
      if (kind == 0 && used.insert({0, id}).second) {
        const uint32_t rate = rng() % 1000;
        ops.push_back(Met(id, rate));
        m[id] = rate;
      } else if (kind == 1 && used.insert({0, id}).second) {
        ops.push_back(EraseMet(id));
        m.erase(id);
      } else if (kind == 2 && used.insert({1, id}).second) {
        const uint32_t meter = rng() % 3 == 0 ? 0 : 1 + rng() % 8;
        ops.push_back(Act(id, 1, meter));
        a[id] = meter;
      } else if (kind == 3 && used.insert({1, id}).second) {
        ops.push_back(EraseAct(id));
        a.erase(id);
      } else if (kind == 4 && used.insert({2, key}).second) {
        ops.push_back(Rule(key, id));
        rl[key] = id;
      } else if (kind == 5 && used.insert({2, key}).second) {
        ops.push_back(EraseRule(key));
        rl.erase(key);
      }
    }
    if (ops.empty()) {
      continue;
    }
    // The model accepts iff every erased key existed and no reference dangles.
    bool ok = true;
    for (const Op &op : ops) {
      if (op.kind != OpKind::kErase) {
        continue;
      }
      if (op.resource == "meters") {
        MeterId id;
        DecodeKey(op.key, &id);
        ok &= meters.contains(id.value());
      } else if (op.resource == "actions") {
        ActionId id;
        DecodeKey(op.key, &id);
        ok &= actions.contains(id.value());
      } else {
        uint64_t k = 0;
        DecodeKey(op.key, &k);
        ok &= rules.contains(k);
      }
    }
    for (const auto &[id, meter] : a) {
      ok &= meter == 0 || m.contains(meter);
    }
    for (const auto &[key, action] : rl) {
      ok &= a.contains(action);
    }
    // Upserts of ids erased in an earlier transaction may still be retiring;
    // with no online reader, grace periods complete at once.
    const auto r = Apply(ops);
    ASSERT_EQ(r.outcome == Outcome::kApplied, ok) << "transaction " << t;
    if (ok) {
      meters = m;
      actions = a;
      rules = rl;
      applied++;
    } else {
      rejected++;
    }

    // State and invariants.
    ASSERT_EQ(meters_.size(), meters.size());
    ASSERT_EQ(actions_.size(), actions.size());
    ASSERT_EQ(table_->size(), rules.size());
    std::map<uint32_t, size_t> meter_refs, action_refs;
    for (const auto &[id, meter] : actions) {
      const Action *act = actions_.Lookup(ActionId(id));
      ASSERT_NE(act, nullptr);
      ASSERT_EQ(act->meter.value(), meter);
      if (meter != 0) {
        ASSERT_NE(meters_.Lookup(MeterId(meter)), nullptr);
        meter_refs[meter]++;
      }
    }
    for (const auto &[key, action] : rules) {
      ASSERT_EQ(RuleValue(key), action);
      ASSERT_NE(actions_.Lookup(ActionId(action)), nullptr);
      action_refs[action]++;
    }
    for (uint32_t id = 1; id <= 8; id++) {
      ASSERT_EQ(engine_.ReferenceCount("meters", EncodeKey(MeterId(id))),
                meter_refs[id]);
      ASSERT_EQ(engine_.ReferenceCount("actions", EncodeKey(ActionId(id))),
                action_refs[id]);
    }
  }
  EXPECT_GT(applied, 300u);
  EXPECT_GT(rejected, 300u);
}

// Packets never resolve a reference to a missing object: a reader that finds
// a rule always finds its action, and the action's meter, while the writer
// creates, re-points and deletes whole chains.
TEST_F(TransactionEngineTest, ReadersNeverResolveADanglingReference) {
  constexpr rcu::ReaderId kReader = 9;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> dangling{0}, resolved{0};
  std::thread reader([&] {
    domain_.Online(kReader);
    uint64_t keys[8], values[8];
    for (uint64_t i = 0; i < 8; i++) {
      keys[i] = i;
    }
    while (!stop.load(std::memory_order_relaxed)) {
      uint64_t hits = table_->LookupBatch(
          classifier::ConstBytes(reinterpret_cast<const classifier::Byte *>(keys),
                                 sizeof(keys)),
          8, values, 8);
      for (uint64_t m = hits; m != 0; m &= m - 1) {
        const size_t i = static_cast<size_t>(__builtin_ctzll(m));
        const Action *a = actions_.Lookup(ActionId(static_cast<uint32_t>(values[i])));
        if (a == nullptr ||
            (a->meter.value() != 0 && meters_.Lookup(a->meter) == nullptr)) {
          dangling++;
        } else {
          resolved++;
        }
      }
      domain_.Quiescent(kReader);
    }
    domain_.Offline(kReader);
  });

  std::mt19937 rng(99);
  // Chain for rule k: action 10+k (or 40+k), meter 10+k (or 40+k): the writer
  // alternates between the two id sets so erased ids have time to retire.
  std::vector<int> version(8, -1);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  size_t txns = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const uint64_t k = rng() % 8;
    std::vector<Op> ops;
    const int v = version[k];
    const int next = v < 0 ? 0 : 1 - v;
    const uint32_t id = static_cast<uint32_t>(10 + 30 * next + k);
    if (v >= 0 && rng() % 3 == 0) {  // delete the chain
      const uint32_t old = static_cast<uint32_t>(10 + 30 * v + k);
      ops = {EraseMet(old), EraseAct(old), EraseRule(k)};
      version[k] = -1;
    } else {  // (re)create: new meter+action, re-point the rule, drop the old
      ops = {Rule(k, id), Act(id, 1, id), Met(id, 1)};
      if (v >= 0) {
        const uint32_t old = static_cast<uint32_t>(10 + 30 * v + k);
        ops.push_back(EraseAct(old));
        ops.push_back(EraseMet(old));
      }
      version[k] = next;
    }
    const auto r = Apply(ops);
    if (r.outcome != Outcome::kApplied) {
      // Only an id still retiring can refuse: retry later.
      bool retiring = false;
      for (const auto &op : r.ops) {
        retiring |= op.error.find("retiring") != std::string::npos;
      }
      ASSERT_TRUE(retiring) << "unexpected rejection";
      version[k] = v;
      continue;
    }
    txns++;
  }
  stop = true;
  reader.join();
  domain_.Unregister(kReader);
  EXPECT_EQ(dangling.load(), 0u) << "of " << resolved.load() << " resolutions";
  EXPECT_GT(resolved.load(), 1000u);
  EXPECT_GT(txns, 1000u);
}

// A resource whose erases take effect at once (an rte_hash rule table) must
// not be a referent: a referrer's old value could hand a reader a key that is
// already gone.
TEST_F(TransactionEngineTest, ReferencesToAnImmediateEraseResourceAreRefused) {
  ExactRuleResource other("other_rules", 3, *table_, [](uint64_t v) {
    return std::vector<Reference>{{"rules", EncodeKey(v)}};
  });
  ASSERT_TRUE(engine_.Register(&other));
  ASSERT_EQ(Apply({Act(1, 1, 0), Rule(10, 1)}).outcome, Outcome::kApplied);
  auto r = Apply({Op::Upsert("other_rules", EncodeKey(uint64_t{99}),
                             std::any(uint64_t{10}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("cannot be referenced"), std::string::npos)
      << r.ops[0].error;
  ASSERT_TRUE(engine_.Unregister("other_rules"));
}

// Every level of the chain in a SlotTable, so erased referrers stay readable
// too: after a grace period, a table that empties a referent before the
// referrer above it is emptied would let a new reader reach the old referrer
// and then find nothing. The removal cascade empties referrers first, a grace
// period apart (found by a ThreadSanitizer harness of exactly this shape).
TEST_F(TransactionEngineTest, SlotChainsNeverDangleWhileRemovalsCascade) {
  struct RuleTag;
  using RuleId = StrongId<RuleTag, uint32_t>;
  struct RuleObj {
    ActionId action;
  };
  SlotTable<RuleId, RuleObj> rules(16);
  SlotResource<RuleId, RuleObj> rules_res(
      "slot_rules", 2, rules, [](const RuleObj &r) {
        return std::vector<Reference>{{"actions", EncodeKey(r.action)}};
      });
  ASSERT_TRUE(engine_.Register(&rules_res));
  constexpr rcu::ReaderId kReader = 10;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> dangling{0}, resolved{0};
  std::thread reader([&] {
    domain_.Online(kReader);
    while (!stop.load(std::memory_order_relaxed)) {
      for (uint32_t k = 1; k <= 8; k++) {
        const RuleObj *r = rules.Lookup(RuleId(k));
        if (r == nullptr) {
          continue;
        }
        const Action *a = actions_.Lookup(r->action);
        if (a == nullptr || meters_.Lookup(a->meter) == nullptr) {
          dangling++;
        } else {
          resolved++;
        }
      }
      domain_.Quiescent(kReader);
    }
    domain_.Offline(kReader);
  });

  std::mt19937 rng(5);
  std::vector<int> version(9, -1);
  size_t txns = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  while (std::chrono::steady_clock::now() < deadline) {
    const uint32_t k = 1 + rng() % 8;
    const int v = version[k];
    const int next = v < 0 ? 0 : 1 - v;
    const uint32_t id = 10 + 20 * next + k;
    const uint32_t old = 10 + 20 * (v < 0 ? 0 : v) + k;
    std::vector<Op> ops;
    if (v >= 0 && rng() % 3 == 0) {
      ops = {EraseMet(old), EraseAct(old),
             Op::Erase("slot_rules", EncodeKey(RuleId(k)))};
      version[k] = -1;
    } else {
      ops = {Op::Upsert("slot_rules", EncodeKey(RuleId(k)),
                        std::any(RuleObj{ActionId(id)})),
             Act(id, 1, id), Met(id, 1)};
      if (v >= 0) {
        ops.push_back(EraseAct(old));
        ops.push_back(EraseMet(old));
      }
      version[k] = next;
    }
    if (Apply(ops).outcome == Outcome::kApplied) {
      txns++;
    } else {
      version[k] = v;  // an id still retiring: try again later
    }
    engine_.ReclaimRetired();
  }
  stop = true;
  reader.join();
  domain_.Unregister(kReader);
  while (engine_.ReclaimRetired() != 0) {
  }
  ASSERT_TRUE(engine_.Unregister("slot_rules"));
  EXPECT_EQ(dangling.load(), 0u) << "of " << resolved.load() << " resolutions";
  EXPECT_GT(resolved.load(), 1000u);
  EXPECT_GT(txns, 500u);
}

}  // namespace
}  // namespace bess::dataplane
