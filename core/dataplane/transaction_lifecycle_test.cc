// SPDX-License-Identifier: BSD-3-Clause

// Adversarial tests of the whole transaction lifecycle (G1.2b, D-021):
// registration, reservation (with failures and exceptions injected at every
// position), reference checks, publication, commit, retirement, the removal
// cascade, backpressure and unregistration -- with readers that stall and
// resume at arbitrary points. Every rejected or failed transaction must leave
// no trace (logical state, physical table state, reference ledger), and the
// engine must stay usable after every failure.

#include "dataplane/transaction_engine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "classifier/exact_rule_resource.h"
#include "runtime/runtime_state.h"
#include "dataplane/slot_resource.h"
#include "dataplane/strong_id.h"

// Allocation-failure injection: with g_fail_countdown = k >= 0, the (k+1)-th
// allocation on this thread throws std::bad_alloc (and the countdown turns
// itself off). Section 9 fails every allocation Apply() makes, one by one.
namespace {
thread_local long g_fail_countdown = -1;
}  // namespace

// Replacing the global allocation functions pairs malloc with free by design;
// GCC's -Wmismatched-new-delete does not know these are the replacements.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void *operator new(std::size_t n) {
  if (g_fail_countdown >= 0 && g_fail_countdown-- == 0) {
    throw std::bad_alloc();
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

std::atomic<int64_t> g_live_meters{0};

struct Meter {
  uint32_t rate = 0;
  explicit Meter(uint32_t r = 0) : rate(r) { g_live_meters++; }
  Meter(const Meter &o) : rate(o.rate) { g_live_meters++; }
  ~Meter() { g_live_meters--; }
};

struct Action {
  uint16_t gate = 0;
  MeterId meter;  // 0: none
};

// -- fault injection ---------------------------------------------------------

enum class Fault { kNone, kError, kThrow };

struct Injector {
  Fault mode = Fault::kNone;
  int fail_at = 0;  // the n-th Reserve() call of the transaction (1-based)
  int calls = 0;
  void Arm(Fault m, int at) {
    mode = m;
    fail_at = at;
    calls = 0;
  }
  void Disarm() { Arm(Fault::kNone, 0); }
};

// Wraps a resource and fails its Reserve() when the transaction-wide call
// count reaches the armed position: an error return, or an exception (an
// allocation failure). Everything else is forwarded.
class FaultyResource final : public Resource {
 public:
  FaultyResource(Resource &inner, Injector &injector)
      : Resource(inner.name(), inner.declared_references()),
        inner_(inner),
        injector_(injector) {}

  bool Contains(const ResourceKey &key) const override {
    return inner_.Contains(key);
  }
  std::vector<Reference> ReferencesOf(const ResourceKey &key) const override {
    return inner_.ReferencesOf(key);
  }
  void VisitReferences(
      const std::function<void(const Reference &)> &visit) const override {
    inner_.VisitReferences(visit);
  }

  bool DefersErase() const override { return inner_.DefersErase(); }
  size_t LiveCount() const override { return inner_.LiveCount(); }
  void EndTransaction() noexcept override {
    in_transaction = false;
    inner_.EndTransaction();
  }

  // Reserve() was called and EndTransaction() has not been since.
  bool in_transaction = false;

  std::expected<Reservation, std::string> Reserve(const Op &op) override {
    in_transaction = true;
    if (injector_.mode != Fault::kNone &&
        ++injector_.calls == injector_.fail_at) {
      if (injector_.mode == Fault::kThrow) {
        throw std::bad_alloc();
      }
      return std::unexpected("injected failure");
    }
    return inner_.Reserve(op);
  }

 private:
  Resource &inner_;
  Injector &injector_;
};

constexpr size_t kIds = 64;

class LifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto t = ConcurrentExactTable::Create(
        8, ConcurrentExactTable::CapacityFor(512), domain_);
    ASSERT_TRUE(t.has_value());
    table_ = std::move(*t);
    meters_res_ = std::make_unique<SlotResource<MeterId, Meter>>("meters",
                                                                 meters_);
    actions_res_ = std::make_unique<SlotResource<ActionId, Action>>(
        "actions", actions_,
        [](const Action &a) {
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
          if (action == kThrowingValue) {
            throw std::bad_alloc();  // a reference callback that fails
          }
          return std::vector<Reference>{
              {"actions", EncodeKey(ActionId(static_cast<uint32_t>(action)))}};
        },
        std::vector<std::string>{"actions"});
    faulty_meters_ = std::make_unique<FaultyResource>(*meters_res_, injector_);
    ASSERT_TRUE(engine_.Register(faulty_meters_.get()));
    faulty_actions_ =
        std::make_unique<FaultyResource>(*actions_res_, injector_);
    ASSERT_TRUE(engine_.Register(faulty_actions_.get()));
    faulty_rules_ = std::make_unique<FaultyResource>(*rules_res_, injector_);
    ASSERT_TRUE(engine_.Register(faulty_rules_.get()));
  }

  void TearDown() override {
    injector_.Disarm();
    while (engine_.ReclaimRetired() != 0) {
    }
    domain_.Drain();
  }

  static constexpr uint64_t kThrowingValue = 666;

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

  // Everything a failed transaction could leave behind: logical contents,
  // physical rule-table entries (pending keys count here), the reference
  // ledger and the generation.
  struct Snapshot {
    std::map<uint32_t, uint32_t> meters;              // id -> rate
    std::map<uint32_t, std::pair<uint16_t, uint32_t>> actions;
    std::map<uint64_t, uint64_t> rules;               // physical, raw value
    std::map<std::pair<int, uint32_t>, size_t> refs;
    size_t rules_physical = 0;
    uint64_t generation = 0;
    friend bool operator==(const Snapshot &, const Snapshot &) = default;
  };

  Snapshot Take() const {
    Snapshot s;
    for (uint32_t id = 1; id <= kIds; id++) {
      if (meters_.Contains(MeterId(id))) {
        s.meters[id] = meters_.Current(MeterId(id))->rate;
      }
      if (actions_.Contains(ActionId(id))) {
        const Action *a = actions_.Current(ActionId(id));
        s.actions[id] = {a->gate, a->meter.value()};
      }
      if (size_t n = engine_.ReferenceCount("meters", EncodeKey(MeterId(id)))) {
        s.refs[{0, id}] = n;
      }
      if (size_t n =
              engine_.ReferenceCount("actions", EncodeKey(ActionId(id)))) {
        s.refs[{1, id}] = n;
      }
    }
    table_->ForEach([&](classifier::ConstBytes key, uint64_t value) {
      uint64_t k = 0;
      std::memcpy(&k, key.data(), 8);
      s.rules[k] = value;
    });
    s.rules_physical = s.rules.size();
    s.generation = engine_.generation();
    return s;
  }

  bool RuleExists(uint64_t key) const {
    uint64_t v = 0;
    const auto bytes = EncodeKey(key);
    return table_->LookupBatch(
               classifier::ConstBytes(
                   reinterpret_cast<const classifier::Byte *>(bytes.data()), 8),
               8, &v, 1) != 0 &&
           v != ExactRuleResource::kPending;
  }

  TransactionEngine::Result Apply(const std::vector<Op> &ops) {
    return engine_.Apply(ops);
  }

  rcu::RcuDomain &domain_ = bess::runtime::runtime().rcu();
  std::unique_ptr<ConcurrentExactTable> table_;
  SlotTable<MeterId, Meter> meters_{kIds};
  SlotTable<ActionId, Action> actions_{kIds};
  std::unique_ptr<SlotResource<MeterId, Meter>> meters_res_;
  std::unique_ptr<SlotResource<ActionId, Action>> actions_res_;
  std::unique_ptr<ExactRuleResource> rules_res_;
  Injector injector_;
  std::unique_ptr<FaultyResource> faulty_meters_, faulty_actions_,
      faulty_rules_;
  TransactionEngine engine_{domain_};
};

using Outcome = TransactionEngine::Outcome;

// -- 1. failure at every reservation position --------------------------------

TEST_F(LifecycleTest, FailureAtEveryReservePositionLeavesNoTrace) {
  // A base state to modify: two chains.
  ASSERT_EQ(Apply({Met(1, 10), Act(1, 1, 1), Rule(100, 1), Met(2, 20),
                   Act(2, 2, 2), Rule(200, 2)})
                .outcome,
            Outcome::kApplied);
  const std::vector<std::vector<Op>> shapes = {
      // establish a new chain (new rule keys are placed as pending keys)
      {Rule(300, 3), Act(3, 3, 3), Met(3, 30), Rule(301, 3)},
      // re-point a rule, replace a meter, drop the old action
      {Act(4, 4, 2), Rule(100, 4), EraseAct(1), Met(2, 21)},
      // delete a whole chain
      {EraseRule(200), EraseAct(2), EraseMet(2)},
      // a larger mix: every operation kind on every resource
      {Met(5, 50), Act(5, 5, 5), Rule(500, 5), Rule(501, 5), Rule(100, 5),
       EraseAct(1), EraseMet(1), Met(2, 22), Rule(502, 5), Act(6, 6, 5),
       Rule(503, 6)},
  };
  for (size_t shape = 0; shape < shapes.size(); shape++) {
    const auto &ops = shapes[shape];
    for (Fault mode : {Fault::kError, Fault::kThrow}) {
      for (int at = 1; at <= static_cast<int>(ops.size()); at++) {
        SCOPED_TRACE(testing::Message()
                     << "shape " << shape << " fail at " << at
                     << (mode == Fault::kThrow ? " (throw)" : " (error)"));
        const Snapshot before = Take();
        injector_.Arm(mode, at);
        if (mode == Fault::kThrow) {
          EXPECT_THROW(Apply(ops), std::bad_alloc);
        } else {
          const auto r = Apply(ops);
          ASSERT_EQ(r.outcome, Outcome::kRejected);
          EXPECT_EQ(r.ops[at - 1].error, "injected failure");
        }
        injector_.Disarm();
        table_->ReclaimAll();
        EXPECT_EQ(Take(), before) << "a failed transaction left a trace";
      }
    }
    // The engine is not wedged: the same transaction applies now.
    const Snapshot before = Take();
    const auto r = Apply(ops);
    ASSERT_EQ(r.outcome, Outcome::kApplied) << "shape " << shape;
    EXPECT_NE(Take(), before);
    // Restore the base state for the next shape.
    std::vector<Op> undo;
    for (uint64_t k : {100, 200, 300, 301, 500, 501, 502, 503}) {
      if (RuleExists(k)) {
        undo.push_back(EraseRule(k));
      }
    }
    for (uint32_t id = 1; id <= 6; id++) {
      if (actions_.Contains(ActionId(id))) {
        undo.push_back(EraseAct(id));
      }
      if (meters_.Contains(MeterId(id))) {
        undo.push_back(EraseMet(id));
      }
    }
    ASSERT_EQ(Apply(undo).outcome, Outcome::kApplied);
    while (engine_.ReclaimRetired() != 0) {
    }
    ASSERT_EQ(Apply({Met(1, 10), Act(1, 1, 1), Rule(100, 1), Met(2, 20),
                     Act(2, 2, 2), Rule(200, 2)})
                  .outcome,
              Outcome::kApplied);
  }
}

// -- 2. a user reference callback that throws ---------------------------------

TEST_F(LifecycleTest, ThrowingReferenceCallbackLeavesNoPendingKey) {
  ASSERT_EQ(Apply({Met(1, 1), Act(1, 1, 1)}).outcome, Outcome::kApplied);
  const Snapshot before = Take();
  // Rule 10 is placed as a pending key first; rule 11's reference callback
  // then throws. Neither key may remain, pending or otherwise.
  EXPECT_THROW(Apply({Rule(10, 1), Rule(11, kThrowingValue)}), std::bad_alloc);
  table_->ReclaimAll();
  EXPECT_EQ(Take(), before);
  EXPECT_EQ(table_->size(), 0u) << "a pending key leaked";
  // And the throwing operation alone.
  EXPECT_THROW(Apply({Rule(12, kThrowingValue)}), std::bad_alloc);
  EXPECT_EQ(table_->size(), 0u) << "a pending key leaked";
  EXPECT_EQ(Apply({Rule(10, 1)}).outcome, Outcome::kApplied);
}

// Rejections decided after an operation's own Reserve() succeeded -- its
// references are refused, or leave something dangling -- must undo that
// operation's reservation too (a pending rule key), not only earlier ones.
TEST_F(LifecycleTest, RejectionsAfterReservationUndoTheirOwnReservation) {
  auto other_table = ConcurrentExactTable::Create(8, 768, domain_);
  ASSERT_TRUE(other_table.has_value());
  // Names "rules" (declared, but rules erase at once: refused) and "meters"
  // (never declared: refused).
  ExactRuleResource refs_rules(
      "refs_rules", **other_table,
      [](uint64_t v) {
        std::vector<Reference> refs;
        if (v == 1) {
          refs.push_back(Reference{"rules", EncodeKey(v)});
        } else {
          refs.push_back(Reference{"meters", EncodeKey(MeterId(1))});
        }
        return refs;
      },
      std::vector<std::string>{"rules"});
  ASSERT_TRUE(engine_.Register(&refs_rules));
  ASSERT_EQ(Apply({Met(1, 1), Act(1, 1, 1), Rule(1, 1)}).outcome,
            Outcome::kApplied);
  const Snapshot before = Take();
  const std::vector<std::vector<Op>> cases = {
      // a reference into an immediate-erase resource
      {Op::Upsert("refs_rules", EncodeKey(uint64_t{7}), std::any(uint64_t{1}))},
      // an undeclared reference
      {Op::Upsert("refs_rules", EncodeKey(uint64_t{8}), std::any(uint64_t{2}))},
      // a missing referent, after an earlier pending key
      {Rule(20, 1), Rule(21, 9)},
      // erasing a referenced key, with a new pending key in the same batch
      {Rule(22, 1), EraseAct(1)},
  };
  for (size_t c = 0; c < cases.size(); c++) {
    SCOPED_TRACE(c);
    const auto r = Apply(cases[c]);
    ASSERT_EQ(r.outcome, Outcome::kRejected);
    table_->ReclaimAll();
    (*other_table)->ReclaimAll();
    EXPECT_EQ(Take(), before);
    EXPECT_EQ((*other_table)->size(), 0u) << "a pending key leaked";
  }
  ASSERT_TRUE(engine_.Unregister("refs_rules"));
}

// -- 3. deferred-removal backlog (review P0-2) --------------------------------

// A single resource with room for many objects and no references.
struct Bulk {
  SlotTable<MeterId, Meter> table{8192};
  SlotResource<MeterId, Meter> res{"bulk", table};
};

bool FinishesWithin(std::chrono::milliseconds limit,
                    const std::function<void()> &fn,
                    const std::function<void()> &unblock) {
  std::atomic<bool> done{false};
  std::thread worker([&] {
    fn();
    done = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const bool finished = done.load();
  if (!finished) {
    unblock();  // let a blocked call return so the test can end
  }
  worker.join();
  return finished;
}

TEST_F(LifecycleTest, ManyDeferredRemovalsCannotBlockTheEngine) {
  Bulk bulk;
  ASSERT_TRUE(engine_.Register(&bulk.res));
  const size_t budget = domain_.retire_high_water() / 2;
  const uint32_t total = static_cast<uint32_t>(3 * budget);
  for (uint32_t first = 1; first <= total; first += 1000) {
    std::vector<Op> ops;
    for (uint32_t id = first; id < first + 1000 && id <= total; id++) {
      ops.push_back(
          Op::Upsert("bulk", EncodeKey(MeterId(id)), std::any(Meter(id))));
    }
    ASSERT_EQ(Apply(ops).outcome, Outcome::kApplied);
  }
  constexpr rcu::ReaderId kReader = 40;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);  // stalled

  auto erase_range = [&](uint32_t first, uint32_t count) {
    std::vector<Op> ops;
    for (uint32_t id = first; id < first + count; id++) {
      ops.push_back(Op::Erase("bulk", EncodeKey(MeterId(id))));
    }
    return Apply(ops);
  };
  const uint32_t chunk = static_cast<uint32_t>(budget);
  EXPECT_EQ(erase_range(1, chunk).outcome, Outcome::kApplied);
  // Its removals are still deferred (the reader has not quiesced), so the
  // RCU queue is empty -- but they count against the budget.
  EXPECT_EQ(erase_range(1 + chunk, chunk).outcome, Outcome::kBusy)
      << "admitted a second cascade whose objects exceed the budget";
  EXPECT_EQ(erase_range(1 + 2 * chunk, chunk).outcome, Outcome::kBusy);

  // One quiescent state makes the first cascade's stage eligible; running it
  // must not block, whatever else is queued.
  domain_.Quiescent(kReader);
  EXPECT_TRUE(FinishesWithin(
      std::chrono::milliseconds(3000), [&] { engine_.ReclaimRetired(); },
      [&] { domain_.Quiescent(kReader); }))
      << "ReclaimRetired blocked with a stalled reader";

  domain_.Quiescent(kReader);
  domain_.Offline(kReader);
  domain_.Unregister(kReader);
  while (engine_.ReclaimRetired() != 0) {
  }
  for (uint32_t first = 1 + chunk; first <= total; first += 1000) {
    const uint32_t count = std::min<uint32_t>(1000, total - first + 1);
    ASSERT_EQ(erase_range(first, count).outcome, Outcome::kApplied);
    while (engine_.ReclaimRetired() != 0) {
    }
  }
  domain_.ReclaimReady();
  ASSERT_TRUE(engine_.Unregister("bulk"));
}

// Another RCU user fills the retire queue while a cascade waits: the
// reclaimer must leave its stage pending rather than push the queue past the
// high-water mark (where RetireErased() waits for readers under the lock).
TEST_F(LifecycleTest, ReclaimerHoldsBackWhenTheRcuQueueIsFull) {
  Bulk bulk;
  ASSERT_TRUE(engine_.Register(&bulk.res));
  std::vector<Op> ops;
  for (uint32_t id = 1; id <= 500; id++) {
    ops.push_back(
        Op::Upsert("bulk", EncodeKey(MeterId(id)), std::any(Meter(id))));
  }
  ASSERT_EQ(Apply(ops).outcome, Outcome::kApplied);
  constexpr rcu::ReaderId kReader = 41;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);
  ops.clear();
  for (uint32_t id = 1; id <= 500; id++) {
    ops.push_back(Op::Erase("bulk", EncodeKey(MeterId(id))));
  }
  ASSERT_EQ(Apply(ops).outcome, Outcome::kApplied);  // cascade pending
  domain_.Quiescent(kReader);  // its stage is now eligible
  // Another user of the domain retires objects the stalled reader holds up.
  const rcu::GracePeriod token = domain_.StartGracePeriod();
  const size_t others = domain_.retire_high_water() - 300;
  for (size_t i = 0; i < others; i++) {
    domain_.Retire(token, std::make_unique<int>(0));
  }
  EXPECT_TRUE(FinishesWithin(
      std::chrono::milliseconds(3000), [&] { engine_.ReclaimRetired(); },
      [&] { domain_.Quiescent(kReader); }))
      << "the reclaimer pushed the RCU queue into a blocking wait";
  EXPECT_EQ(engine_.ReclaimRetired(), 1u) << "the stage should still wait";
  domain_.Quiescent(kReader);
  domain_.Offline(kReader);
  domain_.Unregister(kReader);
  while (engine_.ReclaimRetired() != 0) {
  }
  domain_.ReclaimReady();
  ASSERT_TRUE(engine_.Unregister("bulk"));
}

// -- 4. destructors run before a resource may leave (review P0-1) ------------

TEST_F(LifecycleTest, UnregisterWaitsForTheLastDestructor) {
  SlotTable<MeterId, Meter> table(8);
  SlotResource<MeterId, Meter> res("probe", table);
  ASSERT_TRUE(engine_.Register(&res));
  const int64_t base = g_live_meters.load();
  ASSERT_EQ(Apply({Op::Upsert("probe", EncodeKey(MeterId(1)),
                              std::any(Meter(1)))})
                .outcome,
            Outcome::kApplied);
  // Replace it too, so a retired (not removed) object is also pending.
  ASSERT_EQ(Apply({Op::Upsert("probe", EncodeKey(MeterId(1)),
                              std::any(Meter(2)))})
                .outcome,
            Outcome::kApplied);
  constexpr rcu::ReaderId kReader = 42;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);
  ASSERT_EQ(Apply({Op::Erase("probe", EncodeKey(MeterId(1)))}).outcome,
            Outcome::kApplied);
  EXPECT_FALSE(engine_.Unregister("probe"));
  for (int round = 0; round < 4 && g_live_meters.load() != base; round++) {
    // Until every destructor has run, the resource must stay registered.
    EXPECT_FALSE(engine_.Unregister("probe"))
        << "unregistered with " << (g_live_meters.load() - base)
        << " destructor(s) still pending";
    domain_.Quiescent(kReader);
    engine_.ReclaimRetired();
    domain_.ReclaimReady();
  }
  EXPECT_EQ(g_live_meters.load(), base);
  EXPECT_TRUE(engine_.Unregister("probe"));
  domain_.Offline(kReader);
  domain_.Unregister(kReader);
}

// -- 5. a resource that breaks its declared footprint -------------------------

class LyingResource final : public Resource {
 public:
  LyingResource() : Resource("liar") {}
  bool Contains(const ResourceKey &) const override { return false; }
  size_t LiveCount() const override { return 0; }

  std::expected<Reservation, std::string> Reserve(const Op &) override {
    struct Staged final : StagedOp {
      void Publish(Retirer &retirer) noexcept override {
        retirer.Retire(std::make_unique<int>(1));  // never declared
      }
    };
    return Reservation{std::make_unique<Staged>(), {}, {}};  // footprint 0
  }
};

TEST_F(LifecycleTest, ExceedingTheDeclaredFootprintIsFatal) {
  LyingResource liar;
  ASSERT_TRUE(engine_.Register(&liar));
  EXPECT_DEATH(Apply({Op::Upsert("liar", "k", std::any(1))}),
               "exceeded the footprint");
  ASSERT_TRUE(engine_.Unregister("liar"));
}

// -- 6. the whole lifecycle, randomized, against a model ---------------------

TEST_F(LifecycleTest, RandomLifecycleMatchesAModelWithStallingReaders) {
  // A reader resolves rule -> action -> meter throughout, stalling (not
  // quiescing) for random stretches the writer chooses.
  constexpr rcu::ReaderId kReader = 43;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  std::atomic<bool> stop{false}, stall{false};
  std::atomic<uint64_t> dangling{0}, resolved{0};
  std::thread reader([&] {
    domain_.Online(kReader);
    uint64_t keys[8], values[8];
    for (uint64_t i = 0; i < 8; i++) {
      keys[i] = i;
    }
    while (!stop.load(std::memory_order_relaxed)) {
      const uint64_t hits = ExactRuleResource::VisibleHits(
          table_->LookupBatch(
              classifier::ConstBytes(
                  reinterpret_cast<const classifier::Byte *>(keys),
                  sizeof(keys)),
              8, values, 8),
          values);
      for (uint64_t m = hits; m != 0; m &= m - 1) {
        const int i = __builtin_ctzll(m);
        const Action *a =
            actions_.Lookup(ActionId(static_cast<uint32_t>(values[i])));
        if (a == nullptr ||
            (a->meter.value() != 0 && meters_.Lookup(a->meter) == nullptr)) {
          dangling++;
        } else {
          resolved++;
        }
      }
      if (!stall.load(std::memory_order_relaxed)) {
        domain_.Quiescent(kReader);
      }
    }
    domain_.Offline(kReader);
  });

  std::mt19937 rng(20260927);
  std::map<uint32_t, uint32_t> meters;   // id -> rate
  std::map<uint32_t, uint32_t> actions;  // id -> meter (0: none)
  std::map<uint64_t, uint32_t> rules;    // key -> action
  size_t applied = 0, rejected = 0, injected = 0, busy = 0, retiring = 0;

  for (int t = 0; t < 4000; t++) {
    if (rng() % 16 == 0) {
      stall = !stall;  // the reader stalls or resumes
    }
    if (rng() % 4 == 0) {
      engine_.ReclaimRetired();
    }
    std::vector<Op> ops;
    std::set<std::pair<int, uint64_t>> used;
    auto m = meters;
    auto a = actions;
    auto rl = rules;
    bool erase_missing = false;
    const int n = 1 + static_cast<int>(rng() % 7);
    for (int i = 0; i < n; i++) {
      const int kind = static_cast<int>(rng() % 6);
      const uint32_t id = 1 + rng() % 10;
      const uint64_t key = rng() % 8;
      if (kind == 0 && used.insert({0, id}).second) {
        const uint32_t rate = rng() % 1000;
        ops.push_back(Met(id, rate));
        m[id] = rate;
      } else if (kind == 1 && used.insert({0, id}).second) {
        ops.push_back(EraseMet(id));
        erase_missing |= !m.contains(id);
        m.erase(id);
      } else if (kind == 2 && used.insert({1, id}).second) {
        const uint32_t meter = rng() % 3 == 0 ? 0 : 1 + rng() % 10;
        ops.push_back(Act(id, 1, meter));
        a[id] = meter;
      } else if (kind == 3 && used.insert({1, id}).second) {
        ops.push_back(EraseAct(id));
        erase_missing |= !a.contains(id);
        a.erase(id);
      } else if (kind == 4 && used.insert({2, key}).second) {
        ops.push_back(Rule(key, id));
        rl[key] = id;
      } else if (kind == 5 && used.insert({2, key}).second) {
        ops.push_back(EraseRule(key));
        erase_missing |= !rl.contains(key);
        rl.erase(key);
      }
    }
    if (ops.empty()) {
      continue;
    }
    bool valid = !erase_missing;
    for (const auto &[aid, meter] : a) {
      valid &= meter == 0 || m.contains(meter);
    }
    for (const auto &[key, aid] : rl) {
      valid &= a.contains(aid);
    }

    const int roll = static_cast<int>(rng() % 10);
    const Fault fault =
        roll == 0 ? Fault::kError : roll == 1 ? Fault::kThrow : Fault::kNone;
    if (fault != Fault::kNone) {
      injector_.Arm(fault, 1 + static_cast<int>(rng() % ops.size()));
    }
    const Snapshot before = Take();
    bool changed = false;
    try {
      const auto r = Apply(ops);
      if (r.outcome == Outcome::kApplied) {
        ASSERT_TRUE(valid) << "transaction " << t
                           << ": an invalid transaction applied";
        changed = true;
      } else if (r.outcome == Outcome::kBusy) {
        busy++;
      } else {
        bool expected_reason = false;
        for (const auto &op : r.ops) {
          expected_reason |= op.error == "injected failure" ||
                             op.error.find("retiring") != std::string::npos;
        }
        if (valid) {
          ASSERT_TRUE(expected_reason)
              << "transaction " << t << ": a valid transaction was rejected";
        }
        for (const auto &op : r.ops) {
          retiring += op.error.find("retiring") != std::string::npos;
          injected += op.error == "injected failure";
        }
        rejected++;
      }
    } catch (const std::bad_alloc &) {
      injected++;
    }
    injector_.Disarm();
    if (changed) {
      meters = m;
      actions = a;
      rules = rl;
      applied++;
    } else {
      table_->ReclaimAll();
      ASSERT_EQ(Take(), before)
          << "transaction " << t << ": an unapplied transaction left a trace";
    }

    // The model, the ledger and every reference are consistent.
    ASSERT_EQ(meters_.size(), meters.size()) << t;
    ASSERT_EQ(actions_.size(), actions.size()) << t;
    ASSERT_EQ(table_->size(), rules.size()) << t << ": pending keys leaked?";
    std::map<uint32_t, size_t> meter_refs, action_refs;
    for (const auto &[aid, meter] : actions) {
      ASSERT_EQ(actions_.Current(ActionId(aid))->meter.value(), meter);
      if (meter != 0) {
        meter_refs[meter]++;
      }
    }
    for (const auto &[key, aid] : rules) {
      action_refs[aid]++;
    }
    for (uint32_t id = 1; id <= 10; id++) {
      ASSERT_EQ(engine_.ReferenceCount("meters", EncodeKey(MeterId(id))),
                meter_refs[id]);
      ASSERT_EQ(engine_.ReferenceCount("actions", EncodeKey(ActionId(id))),
                action_refs[id]);
    }
  }
  stall = false;
  stop = true;
  reader.join();
  domain_.Unregister(kReader);
  std::printf(
      "lifecycle mix: %zu applied, %zu rejected (%zu injected, %zu "
      "retiring), %zu busy; reader resolved %llu chains\n",
      applied, rejected, injected, retiring, busy,
      static_cast<unsigned long long>(resolved.load()));
  EXPECT_EQ(dangling.load(), 0u) << "of " << resolved.load();
  EXPECT_GT(applied, 500u);
  EXPECT_GT(resolved.load(), 1000u);
  EXPECT_GT(injected, 300u);
  EXPECT_GT(rejected, 300u);
  RecordProperty("applied", static_cast<int>(applied));
  RecordProperty("busy", static_cast<int>(busy));
  RecordProperty("retiring", static_cast<int>(retiring));
}

// -- 9. an allocation failure anywhere in Apply() ----------------------------

// A fresh engine over fresh tables, so that "first use" (no scratch capacity
// yet) can be tested per fault position.
struct World {
  World() {
    table = std::move(*ConcurrentExactTable::Create(
        8, ConcurrentExactTable::CapacityFor(512), bess::runtime::runtime().rcu()));
    meters_res = std::make_unique<SlotResource<MeterId, Meter>>("meters",
                                                                meters);
    actions_res = std::make_unique<SlotResource<ActionId, Action>>(
        "actions", actions,
        [](const Action &a) {
          std::vector<Reference> refs;
          if (a.meter.value() != 0) {
            refs.push_back({"meters", EncodeKey(a.meter)});
          }
          return refs;
        },
        std::vector<std::string>{"meters"});
    rules_res = std::make_unique<ExactRuleResource>(
        "rules", *table,
        [](uint64_t action) {
          return std::vector<Reference>{
              {"actions", EncodeKey(ActionId(static_cast<uint32_t>(action)))}};
        },
        std::vector<std::string>{"actions"});
    // Wrapped (never armed) to check that every resource asked to reserve
    // is told the transaction ended, whatever happened.
    for (Resource *r : {static_cast<Resource *>(meters_res.get()),
                        static_cast<Resource *>(actions_res.get()),
                        static_cast<Resource *>(rules_res.get())}) {
      tracked.push_back(std::make_unique<FaultyResource>(*r, injector));
      EXPECT_TRUE(engine.Register(tracked.back().get()));
    }
  }

  bool AnyInTransaction() const {
    for (const auto &t : tracked) {
      if (t->in_transaction) {
        return true;
      }
    }
    return false;
  }
  ~World() {
    while (engine.ReclaimRetired() != 0) {
    }
    bess::runtime::runtime().rcu().Drain();
  }

  // Logical and physical state, as in LifecycleTest::Snapshot.
  std::string State() const {
    std::string out;
    for (uint32_t id = 1; id <= kIds; id++) {
      if (meters.Contains(MeterId(id))) {
        out += "m" + std::to_string(id) + "=" +
               std::to_string(meters.Current(MeterId(id))->rate) + " ";
      }
      if (actions.Contains(ActionId(id))) {
        const Action *a = actions.Current(ActionId(id));
        out += "a" + std::to_string(id) + "=" + std::to_string(a->gate) + "/" +
               std::to_string(a->meter.value()) + " ";
      }
      for (const char *res : {"meters", "actions"}) {
        const auto key = res[0] == 'm' ? EncodeKey(MeterId(id))
                                       : EncodeKey(ActionId(id));
        if (size_t n = engine.ReferenceCount(res, key)) {
          out += std::string("r") + res[0] + std::to_string(id) + "=" +
                 std::to_string(n) + " ";
        }
      }
    }
    std::map<uint64_t, uint64_t> rules;  // physical: pending keys count
    table->ForEach([&](classifier::ConstBytes key, uint64_t value) {
      uint64_t k = 0;
      std::memcpy(&k, key.data(), 8);
      rules[k] = value;
    });
    for (const auto &[k, v] : rules) {
      out += "k" + std::to_string(k) + "=" + std::to_string(v) + " ";
    }
    return out + "g" + std::to_string(engine.generation());
  }

  std::unique_ptr<ConcurrentExactTable> table;
  SlotTable<MeterId, Meter> meters{kIds};
  SlotTable<ActionId, Action> actions{kIds};
  std::unique_ptr<SlotResource<MeterId, Meter>> meters_res;
  std::unique_ptr<SlotResource<ActionId, Action>> actions_res;
  std::unique_ptr<ExactRuleResource> rules_res;
  Injector injector;
  std::vector<std::unique_ptr<FaultyResource>> tracked;
  TransactionEngine engine{bess::runtime::runtime().rcu()};
};

Op WRule(uint64_t key, uint64_t action) {
  return Op::Upsert("rules", EncodeKey(key), std::any(action));
}
Op WAct(uint32_t id, uint16_t gate, uint32_t meter) {
  return Op::Upsert("actions", EncodeKey(ActionId(id)),
                    std::any(Action{gate, MeterId(meter)}));
}
Op WMet(uint32_t id, uint32_t rate) {
  return Op::Upsert("meters", EncodeKey(MeterId(id)), std::any(Meter(rate)));
}

// Fails the k-th allocation Apply() makes, for every k, in three settings:
// the engine's first transaction (no scratch capacity yet), a transaction
// larger than any before it, and one that starts by advancing a pending
// removal cascade. Each failure must leave the state exactly as it was, and
// the same transaction must then apply.
TEST(LifecycleAllocationTest, FailureAtEveryAllocationLeavesNoTrace) {
  enum Setting { kFirstUse, kLarger, kAfterRemovals };
  auto chains = [](uint32_t first, uint32_t n) {
    std::vector<Op> ops;
    for (uint32_t id = first; id < first + n; id++) {
      ops.push_back(WRule(1000 + id, id));
      ops.push_back(WAct(id, static_cast<uint16_t>(id), id));
      ops.push_back(WMet(id, id * 10));
    }
    return ops;
  };
  for (Setting setting : {kFirstUse, kLarger, kAfterRemovals}) {
    std::vector<Op> ops;
    size_t faults = 0;
    for (long k = 0;; k++) {
      SCOPED_TRACE(testing::Message() << "setting " << setting
                                      << ", failing allocation " << k);
      World w;
      if (setting == kLarger) {
        ASSERT_EQ(w.engine.Apply(chains(1, 1)).outcome,
                  TransactionEngine::Outcome::kApplied);
        ops = chains(2, 12);  // 36 operations against a scratch sized for 3
      } else if (setting == kAfterRemovals) {
        ASSERT_EQ(w.engine.Apply(chains(1, 4)).outcome,
                  TransactionEngine::Outcome::kApplied);
        // Remove two chains (a cascade over two ranks, pending), re-point a
        // rule and replace a meter.
        ASSERT_EQ(w.engine
                      .Apply(std::vector<Op>{
                          Op::Erase("rules", EncodeKey(uint64_t{1001})),
                          Op::Erase("actions", EncodeKey(ActionId(1))),
                          Op::Erase("meters", EncodeKey(MeterId(1))),
                          Op::Erase("rules", EncodeKey(uint64_t{1002})),
                          Op::Erase("actions", EncodeKey(ActionId(2))),
                          Op::Erase("meters", EncodeKey(MeterId(2)))})
                      .outcome,
                  TransactionEngine::Outcome::kApplied);
        ops = chains(5, 3);
        ops.push_back(WRule(1003, 4));
        ops.push_back(WMet(3, 31));
        ops.push_back(Op::Erase("rules", EncodeKey(uint64_t{1004})));
      } else {
        ops = chains(1, 6);
      }
      const std::string before = w.State();
      g_fail_countdown = k;
      bool threw = false;
      TransactionEngine::Result r;
      try {
        r = w.engine.Apply(ops);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      const bool fault_fired = g_fail_countdown < 0 && threw;
      g_fail_countdown = -1;
      if (!threw) {
        // Every allocation of this Apply() has had its turn to fail.
        ASSERT_EQ(r.outcome, TransactionEngine::Outcome::kApplied);
        break;
      }
      ASSERT_TRUE(fault_fired);
      faults++;
      ASSERT_FALSE(w.AnyInTransaction()) << "EndTransaction() skipped";
      w.table->ReclaimAll();
      ASSERT_EQ(w.State(), before) << "a failed allocation left a trace";
      // Not wedged: the same transaction applies now, and ends everywhere.
      ASSERT_EQ(w.engine.Apply(ops).outcome,
                TransactionEngine::Outcome::kApplied);
      ASSERT_FALSE(w.AnyInTransaction()) << "EndTransaction() skipped";
    }
    std::printf("setting %d: %zu allocation sites failed in turn\n", setting,
                faults);
    EXPECT_GT(faults, 10u);
  }
}

}  // namespace
}  // namespace bess::dataplane
