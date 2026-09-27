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
#include <cstdlib>
#include <new>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "classifier/exact_rule_resource.h"
#include "control/runtime_state.h"
#include "dataplane/slot_resource.h"
#include "dataplane/strong_id.h"

// Allocation counting for the publication window: every allocation made while
// the engine's test hook reports "publishing" is counted (none are allowed).
namespace {
thread_local bool g_in_publish = false;
std::atomic<size_t> g_publish_allocations{0};
}  // namespace

// Replacing the global allocation functions pairs malloc with free by design;
// GCC's -Wmismatched-new-delete does not know these are the replacements.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void *operator new(std::size_t n) {
  if (g_in_publish) {
    g_publish_allocations++;
  }
  if (void *p = std::malloc(n == 0 ? 1 : n)) {
    return p;
  }
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#pragma GCC diagnostic pop

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
        "meters", meters_);
    actions_res_ = std::make_unique<SlotResource<ActionId, Action>>(
        "actions", actions_, [](const Action &a) {
          std::vector<Reference> refs;
          if (a.meter.value() != 0) {
            refs.push_back({"meters", EncodeKey(a.meter)});
          }
          return refs;
        },
        std::vector<std::string>{"meters"});
    rules_res_ = std::make_unique<ExactRuleResource>(
        "rules", *table_,
        [](uint64_t action) {
          return std::vector<Reference>{
              {"actions", EncodeKey(ActionId(static_cast<uint32_t>(action)))}};
        },
        std::vector<std::string>{"actions"});
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
            8, &v, 1) == 0 ||
        v == ExactRuleResource::kPending) {
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
  Recorder(std::string name, std::vector<std::string> deps,
           std::vector<std::string> *log)
      : Resource(std::move(name), std::move(deps)), log_(log) {}
  size_t LiveCount() const override { return keys_.size(); }
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
  Recorder a("a", {}, &log), b("b", {"a"}, &log), c("c", {"b"}, &log);
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
  EXPECT_TRUE(r.ops.back().error.find("table full") != std::string::npos ||
              r.ops.back().error.find("no room") != std::string::npos)
      << r.ops.back().error;
  EXPECT_EQ(table_->size(), 0u);
  EXPECT_EQ(meters_.Lookup(MeterId(2)), nullptr);

  // The aborted transaction's pending keys were taken back: slots are free
  // again once their grace period passes (no reader is online here).
  table_->ReclaimAll();
  ops.resize(fits / 2);
  EXPECT_EQ(Apply(ops).outcome, Outcome::kApplied);
  EXPECT_EQ(table_->size(), fits / 2 - 1);
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
      uint64_t hits = ExactRuleResource::VisibleHits(
          table_->LookupBatch(
              classifier::ConstBytes(
                  reinterpret_cast<const classifier::Byte *>(keys),
                  sizeof(keys)),
              8, values, 8),
          values);
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
  auto other_table = ConcurrentExactTable::Create(8, 768, domain_);
  ASSERT_TRUE(other_table.has_value());
  ExactRuleResource other(
      "other_rules", **other_table,
      [](uint64_t v) {
        return std::vector<Reference>{{"rules", EncodeKey(v)}};
      },
      {"rules"});
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
      "slot_rules", rules,
      [](const RuleObj &r) {
        return std::vector<Reference>{{"actions", EncodeKey(r.action)}};
      },
      {"actions"});
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
  // Empty the resource before unregistering it (live keys block that).
  for (uint32_t k = 1; k <= 8; k++) {
    if (version[k] >= 0) {
      ASSERT_EQ(Apply({Op::Erase("slot_rules", EncodeKey(RuleId(k)))}).outcome,
                Outcome::kApplied);
    }
  }
  while (engine_.ReclaimRetired() != 0) {
  }
  ASSERT_TRUE(engine_.Unregister("slot_rules"));
  EXPECT_EQ(dangling.load(), 0u) << "of " << resolved.load() << " resolutions";
  EXPECT_GT(resolved.load(), 1000u);
  EXPECT_GT(txns, 500u);
}

// The publication order is derived from declared dependencies, never from
// caller-assigned numbers: a value may only name a declared resource, a
// dependency must be registered first (so the graph is acyclic), and a
// resource leaves only when nothing depends on it and it holds no keys.
TEST_F(TransactionEngineTest, DependencyDeclarationsAreEnforced) {
  EXPECT_EQ(meters_res_->rank(), 0);
  EXPECT_EQ(actions_res_->rank(), 1);
  EXPECT_EQ(rules_res_->rank(), 2);

  // Names meters without declaring them.
  SlotTable<ActionId, Action> other_actions(kIds);
  SlotResource<ActionId, Action> undeclared(
      "undeclared_actions", other_actions, [](const Action &a) {
        return std::vector<Reference>{{"meters", EncodeKey(a.meter)}};
      });
  ASSERT_TRUE(engine_.Register(&undeclared));
  EXPECT_EQ(undeclared.rank(), 0);
  auto r = Apply({Met(1, 1), Op::Upsert("undeclared_actions",
                                         EncodeKey(ActionId(1)),
                                         std::any(Action{1, MeterId(1)}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[1].error.find("undeclared reference"), std::string::npos)
      << r.ops[1].error;
  EXPECT_EQ(meters_.Lookup(MeterId(1)), nullptr);
  ASSERT_TRUE(engine_.Unregister("undeclared_actions"));

  // Self-dependency, and a dependency that is not registered.
  SlotResource<ActionId, Action> self("self", other_actions, {}, {"self"});
  auto reg = engine_.Register(&self);
  ASSERT_FALSE(reg);
  EXPECT_NE(reg.error().find("itself"), std::string::npos);
  SlotResource<ActionId, Action> orphan("orphan", other_actions, {},
                                        {"not_registered"});
  reg = engine_.Register(&orphan);
  ASSERT_FALSE(reg);
  EXPECT_NE(reg.error().find("register it first"), std::string::npos);

  // Nothing leaves while a registered resource depends on it, or while it
  // holds keys.
  auto un = engine_.Unregister("meters");
  ASSERT_FALSE(un);
  EXPECT_NE(un.error().find("may reference 'meters'"), std::string::npos);
  SlotTable<MeterId, Meter> leaf_table(8);
  SlotResource<MeterId, Meter> leaf("leaf", leaf_table);
  ASSERT_TRUE(engine_.Register(&leaf));
  ASSERT_EQ(Apply({Op::Upsert("leaf", EncodeKey(MeterId(1)),
                              std::any(Meter(1)))})
                .outcome,
            Outcome::kApplied);
  un = engine_.Unregister("leaf");
  ASSERT_FALSE(un);
  EXPECT_NE(un.error().find("live key"), std::string::npos);
  ASSERT_EQ(Apply({Op::Erase("leaf", EncodeKey(MeterId(1)))}).outcome,
            Outcome::kApplied);
  ASSERT_TRUE(engine_.Unregister("leaf"));

  // A populated resource that may reference others cannot join: its
  // existing references would be missing from the ledger.
  SlotTable<ActionId, Action> populated_table(8);
  populated_table.Publish(ActionId(1),
                          std::make_unique<const Action>(Action{1, MeterId(9)}));
  SlotResource<ActionId, Action> populated(
      "populated", populated_table,
      [](const Action &a) {
        return std::vector<Reference>{{"meters", EncodeKey(a.meter)}};
      },
      {"meters"});
  reg = engine_.Register(&populated);
  ASSERT_FALSE(reg);
  EXPECT_NE(reg.error().find("populated"), std::string::npos);
}

// Pending removal steps capture a resource's tables: Unregister must refuse
// until they ran, even when nothing references the resource any more.
TEST_F(TransactionEngineTest, UnregisterWaitsForPendingRemovals) {
  constexpr rcu::ReaderId kReader = 11;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);
  for (int cycle = 0; cycle < 3; cycle++) {
    SCOPED_TRACE(cycle);
    SlotTable<MeterId, Meter> table(8);
    SlotResource<MeterId, Meter> res("temp_meters", table);
    ASSERT_TRUE(engine_.Register(&res));
    ASSERT_EQ(Apply({Op::Upsert("temp_meters", EncodeKey(MeterId(1)),
                                std::any(Meter(5)))})
                  .outcome,
              Outcome::kApplied);
    ASSERT_EQ(Apply({Op::Erase("temp_meters", EncodeKey(MeterId(1)))}).outcome,
              Outcome::kApplied);
    EXPECT_FALSE(engine_.Unregister("temp_meters"))
        << "unregistered with a removal step still pending (reader online)";
    // One quiescent state: the removal step runs and hands the object to
    // RCU, whose destructor -- code of the resource's module -- still waits
    // for another grace period.
    domain_.Quiescent(kReader);
    auto un = engine_.Unregister("temp_meters");
    EXPECT_FALSE(un) << "unregistered while a retired object's destructor "
                        "was still pending";
    if (!un) {
      EXPECT_NE(un.error().find("retired object"), std::string::npos)
          << un.error();
    }
    domain_.Quiescent(kReader);
    ASSERT_TRUE(engine_.Unregister("temp_meters"));
    // `table` and `res` are destroyed here: nothing may still point at them.
  }
  EXPECT_EQ(engine_.ReclaimRetired(), 0u);
  domain_.Offline(kReader);
  domain_.Unregister(kReader);
}

// Aggregate capacity cannot promise placement: keys chosen to share one
// rte_hash bucket pair fill it at 2 x 8 entries whatever the table's size.
// Such a transaction must be rejected with nothing visible -- it used to
// reach a CHECK in Publish and abort the daemon.
TEST_F(TransactionEngineTest, KeysThatCannotBePlacedRejectCleanly) {
  auto small = ConcurrentExactTable::Create(8, 768, domain_);
  ASSERT_TRUE(small.has_value());
  ExactRuleResource res(
      "crowded_rules", **small,
      [](uint64_t v) {
        return std::vector<Reference>{
            {"actions", EncodeKey(ActionId(static_cast<uint32_t>(v)))}};
      },
      {"actions"});
  ASSERT_TRUE(engine_.Register(&res));
  ASSERT_EQ(Apply({Act(1, 1, 0)}).outcome, Outcome::kApplied);

  // rte_hash: primary bucket = sig & mask, alternate = (primary ^ (sig >>
  // 16)) & mask. Keys agreeing on both land in the same two buckets.
  const uint32_t mask = 1024 / 8 - 1;  // 768 entries -> 1024 -> 128 buckets
  std::vector<uint64_t> crowd;
  uint32_t want_primary = 0, want_short = 0;
  for (uint64_t k = 1; crowd.size() < 20; k++) {
    const auto bytes = EncodeKey(k);
    const uint32_t sig = (*small)->DpdkHash(classifier::ConstBytes(
        reinterpret_cast<const classifier::Byte *>(bytes.data()), 8));
    const uint32_t primary = sig & mask;
    const uint32_t short_sig = (sig >> 16) & mask;
    if (crowd.empty()) {
      want_primary = primary;
      want_short = short_sig;
    }
    if (primary == want_primary && short_sig == want_short) {
      crowd.push_back(k);
    }
  }
  std::vector<Op> ops;
  for (uint64_t k : crowd) {
    ops.push_back(Op::Upsert("crowded_rules", EncodeKey(k), std::any(uint64_t{1})));
  }
  const auto r = Apply(ops);
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  bool placement = false;
  for (const auto &op : r.ops) {
    placement |= op.error.find("no room for this key") != std::string::npos;
  }
  EXPECT_TRUE(placement);
  EXPECT_EQ((*small)->size(), 0u) << "pending keys of the rejected "
                                     "transaction were not taken back";
  EXPECT_EQ(engine_.ReferenceCount("actions", EncodeKey(ActionId(1))), 0u);

  // Sixteen of them fit in the pair's 2 x 8 slots.
  (*small)->ReclaimAll();
  ops.resize(16);
  EXPECT_EQ(Apply(ops).outcome, Outcome::kApplied);
  EXPECT_EQ((*small)->size(), 16u);
  std::vector<Op> erase;
  for (size_t i = 0; i < 16; i++) {
    erase.push_back(Op::Erase("crowded_rules", EncodeKey(crowd[i])));
  }
  ASSERT_EQ(Apply(erase).outcome, Outcome::kApplied);
  ASSERT_TRUE(engine_.Unregister("crowded_rules"));
}

// Publication must not do work that can fail: every allocation happens in
// preparation, and the publish phase only runs the staged operations and
// updates bookkeeping that already exists. Counted with a global operator new
// while the engine reports the publication window.
TEST_F(TransactionEngineTest, PublicationDoesNotAllocate) {
  internal::g_publish_window_hook = [](bool entering) {
    g_in_publish = entering;
  };
  g_publish_allocations = 0;
  // Establish, re-point, replace, erase a chain -- every publish path.
  ASSERT_EQ(Apply({Rule(10, 5), Act(5, 3, 7), Met(7, 1000)}).outcome,
            Outcome::kApplied);
  ASSERT_EQ(Apply({Act(6, 2, 7), Rule(10, 6), EraseAct(5)}).outcome,
            Outcome::kApplied);
  ASSERT_EQ(Apply({Met(7, 2000)}).outcome, Outcome::kApplied);
  ASSERT_EQ(Apply({EraseRule(10), EraseAct(6), EraseMet(7)}).outcome,
            Outcome::kApplied);
  internal::g_publish_window_hook = nullptr;
  EXPECT_EQ(g_publish_allocations.load(), 0u)
      << "the publication phase allocated";
}

// Reclamation runs behind readers. With a reader that stops quiescing, the
// engine must refuse new work retriably (kBusy) rather than push the RCU
// retire queue to the point where it waits for readers under the lock -- a
// hang for as long as the reader stalls.
TEST_F(TransactionEngineTest, SlowReadersGetBackpressureNotAHang) {
  constexpr rcu::ReaderId kReader = 12;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);  // and never reports quiescence below
  ASSERT_EQ(Apply({Met(1, 0)}).outcome, Outcome::kApplied);
  size_t applied = 0;
  bool busy = false;
  for (uint32_t i = 1; i < 10000 && !busy; i++) {
    // Each replacement retires the previous meter object.
    const auto r = Apply({Met(1, i)});
    if (r.outcome == Outcome::kBusy) {
      busy = true;
      EXPECT_EQ(r.ops[0].status, OpStatus::kNotApplied);
    } else {
      ASSERT_EQ(r.outcome, Outcome::kApplied);
      applied++;
    }
  }
  EXPECT_TRUE(busy) << "no backpressure after " << applied << " transactions";
  EXPECT_LE(domain_.Stats().pending_retired_objects,
            domain_.retire_high_water());
  domain_.Quiescent(kReader);
  domain_.ReclaimReady();
  EXPECT_EQ(Apply({Met(1, 1)}).outcome, Outcome::kApplied)
      << "work resumes once the reader quiesces";
  domain_.Offline(kReader);
  domain_.Unregister(kReader);
}

}  // namespace
}  // namespace bess::dataplane
