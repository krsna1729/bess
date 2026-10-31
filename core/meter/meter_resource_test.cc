// SPDX-License-Identifier: BSD-3-Clause

// MeterResource (D-199): a meter set as a transactional resource, driven
// through the transaction engine alone (no module), and its BESS codec.

#include "meter/meter_resource.h"

#include <gtest/gtest.h>

#include <any>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dataplane/slot_resource.h"
#include "dataplane/strong_id.h"
#include "dataplane/transaction_engine.h"
#include "framework/memory_scope.h"
#include "meter/meter_resource_codec.h"
#include "runtime/runtime_state.h"
#include "testing/allocation_faults.h"
#include "utils/dpdk_memory.h"

namespace bess::meter {
namespace {

using dataplane::EncodeKey;
using dataplane::Op;
using Outcome = dataplane::TransactionEngine::Outcome;

constexpr size_t kCapacity = 8;
constexpr uint32_t kReader = 1;

// Counts what the resource admits through its hook (D-198); refuses growth
// while `refuse` is set.
struct CountingGrowth final : utils::MemoryGrowth {
  size_t grown = 0;
  size_t shrunk = 0;
  size_t glibc_grown = 0;
  size_t glibc_shrunk = 0;
  bool refuse = false;
  size_t limit = std::numeric_limits<size_t>::max();
  void Grow(size_t bytes, size_t glibc_bytes) override {
    if (refuse || bytes > limit || grown - shrunk > limit - bytes) {
      throw utils::MemoryRefused(utils::MemoryRefused::Reason::kNormalLimit,
                                 bytes, "refused");
    }
    grown += bytes;
    glibc_grown += glibc_bytes;
  }
  void Shrink(size_t bytes, size_t glibc_bytes) noexcept override {
    shrunk += bytes;
    glibc_shrunk += glibc_bytes;
  }
  size_t glibc_net() const { return glibc_grown - glibc_shrunk; }
};

MeterProfileSpec TrTcm(uint64_t cir) {
  return TrTcmSpec{cir, 2000, 2 * cir, 4000};
}

class MeterResourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // The owner admits the first builder before construction.
    growth_.Grow(MeterResource::Bytes(kCapacity), 0);
    resource_ = std::make_unique<MeterResource>("mt/meters", kCapacity,
                                                published_, &growth_);
    ASSERT_TRUE(engine_.Register(resource_.get()));
  }

  void TearDown() override {
    if (reader_online_) {
      domain_.Offline(kReader);
      domain_.Unregister(kReader);
    }
    while (engine_.ReclaimRetired() != 0) {
    }
    const std::string name = "mt/meters";
    ASSERT_TRUE(engine_.ReleaseForTeardown(std::span<const std::string>(&name, 1)));
    domain_.Drain();
  }

  // Replaces the resource with one of `capacity` meters.
  void Recreate(size_t capacity) {
    const std::string name = "mt/meters";
    ASSERT_TRUE(engine_.ReleaseForTeardown(std::span<const std::string>(&name, 1)));
    domain_.Drain();
    published_.ResetQuiesced();
    resource_.reset();
    growth_.Grow(MeterResource::Bytes(capacity), 0);
    resource_ = std::make_unique<MeterResource>(name, capacity, published_, &growth_);
    ASSERT_TRUE(engine_.Register(resource_.get()));
  }
  // Upserts ids from..to with `spec`.
  void Install(uint32_t from, uint32_t to, MeterProfileSpec spec) {
    std::vector<Op> ops;
    for (uint32_t id = from; id <= to; ++id) {
      ops.push_back(Upsert(id, spec));
    }
    ASSERT_EQ(engine_.ApplyBatched(ops).outcome, Outcome::kApplied);
  }
  std::map<uint32_t, MeterProfileSpec> Policies() const {
    std::map<uint32_t, MeterProfileSpec> policies;
    resource_->VisitPolicies([&](MeterId id, const MeterProfileSpec &spec) {
      policies.emplace(id.value(), spec);
    });
    return policies;
  }

  // Holds grace periods open, so an erase's removal stage waits.
  void HoldGracePeriods() {
    ASSERT_TRUE(domain_.Register(kReader).has_value());
    domain_.Online(kReader);
    reader_online_ = true;
  }
  void PassGracePeriod() {
    domain_.Quiescent(kReader);
    while (engine_.ReclaimRetired() != 0) {
      domain_.Quiescent(kReader);
    }
  }

  static Op Upsert(uint32_t id, MeterProfileSpec spec) {
    return Op::Upsert("mt/meters", EncodeKey(MeterId(id)), std::any(spec));
  }
  static Op Erase(uint32_t id) {
    return Op::Erase("mt/meters", EncodeKey(MeterId(id)));
  }
  dataplane::TransactionEngine::Result Apply(std::vector<Op> ops) {
    return engine_.Apply(ops);
  }
  void Refusal(Op op) {
    EXPECT_EQ(Apply({std::move(op)}).outcome, Outcome::kRejected);
  }
  bool Has(uint32_t id) const {
    return resource_->Contains(EncodeKey(MeterId(id)));
  }
  std::vector<uint32_t> Visited() const {
    std::vector<uint32_t> ids;
    resource_->VisitPolicies(
        [&](MeterId id, const MeterProfileSpec &) { ids.push_back(id.value()); });
    return ids;
  }

  rcu::RcuDomain &domain_ = bess::runtime::runtime().rcu();
  CountingGrowth growth_;
  std::unique_ptr<MeterResource> resource_;
  rcu::RcuPtr<MeterSet> published_{domain_};
  dataplane::TransactionEngine engine_{domain_};
  bool reader_online_ = false;
};

// The constructor publishes an empty generation; what the owner admits is
// Bytes(capacity), and bytes() is that plus what the hook admitted since.
TEST_F(MeterResourceTest, ConstructionPublishesAnEmptyGeneration) {
  ASSERT_NE(published_.Read(), nullptr);
  EXPECT_EQ(published_.Read()->size(), 0u);
  EXPECT_EQ(resource_->capacity(), kCapacity);
  EXPECT_EQ(resource_->LiveCount(), 0u);
  EXPECT_EQ(resource_->bytes(), growth_.grown - growth_.shrunk);
}

// One transaction of several upserts publishes them together; a reconfigure
// keeps other meters' state; read-back is the policies as written.
TEST_F(MeterResourceTest, UpsertPublishesAndReadsBack) {
  ASSERT_EQ(Apply({Upsert(2, TrTcm(1000)), Upsert(1, TrTcm(500))}).outcome,
            Outcome::kApplied);
  const MeterSet *set = published_.Read();
  ASSERT_NE(set->Lookup(MeterId(1)), nullptr);
  ASSERT_NE(set->Lookup(MeterId(2)), nullptr);
  MeterState *two = set->Lookup(MeterId(2));
  EXPECT_TRUE(Has(1));
  EXPECT_TRUE(Has(2));
  EXPECT_EQ(resource_->LiveCount(), 2u);
  EXPECT_EQ(Visited(), (std::vector<uint32_t>{1, 2}));

  ASSERT_EQ(Apply({Upsert(1, TrTcm(700))}).outcome, Outcome::kApplied);
  EXPECT_EQ(published_.Read()->Lookup(MeterId(2)), two)
      << "an unrelated reconfigure keeps the meter's state";
  bool found = false;
  resource_->VisitPolicies([&](MeterId id, const MeterProfileSpec &spec) {
    if (id == MeterId(1)) {
      found = true;
      EXPECT_EQ(spec, TrTcm(700));
    }
  });
  EXPECT_TRUE(found);
  EXPECT_EQ(resource_->bytes(), growth_.grown - growth_.shrunk);
}

// A transaction costs what it changes, not what is installed (D-199):
// reserving and preparing the same operations admits the same bytes and makes
// the same glibc allocations with 64 meters installed as with 8,000, far less
// than one pointer per id; refused, it leaves nothing behind.
TEST_F(MeterResourceTest, ATransactionCostsWhatItChangesNotWhatIsInstalled) {
  constexpr uint32_t kLarge = 65536;
  Recreate(kLarge);
  struct Cost {
    size_t admitted;
    size_t allocations;
  };
  // Two new meters far from the installed ones, a reconfigure and an erase
  // of installed ones, reserved and prepared, then aborted as the engine
  // aborts a refused transaction.
  auto probe = [&]() -> Cost {
    const MeterSet *before = published_.Read();
    MeterState *one = before->Lookup(MeterId(1));
    const auto policies = Policies();
    const size_t net = growth_.grown - growth_.shrunk;
    // DPDK bytes: the index edit. The glibc policy containers (class C,
    // D-206) grow their bucket arrays as they fill, amortized, not per edit.
    const size_t grown = growth_.grown - growth_.glibc_grown;
    const std::vector<Op> ops = {Upsert(kLarge - 1, TrTcm(500)),
                                 Upsert(kLarge, TrTcm(500)),
                                 Upsert(1, TrTcm(900)), Erase(2)};
    std::vector<dataplane::Resource::Reservation> reserved;
    reserved.reserve(ops.size());
    Cost cost{};
    {
      fault_injection::AllocationFaults window;
      for (const Op &op : ops) {
        auto reservation = resource_->Reserve(op);
        EXPECT_TRUE(reservation);
        reserved.push_back(std::move(*reservation));
      }
      EXPECT_TRUE(resource_->Prepare());
      cost.allocations = window.allocations();
    }
    cost.admitted = growth_.grown - growth_.glibc_grown - grown;
    for (auto &reservation : reserved) {
      reservation.staged->Abort();
    }
    resource_->EndTransaction();
    reserved.clear();

    EXPECT_EQ(published_.Read(), before);
    EXPECT_EQ(published_.Read()->Lookup(MeterId(1)), one);
    EXPECT_FALSE(Has(kLarge));
    EXPECT_TRUE(Has(2));
    EXPECT_FALSE(resource_->Retiring(MeterId(2)));
    EXPECT_EQ(Policies(), policies);
    EXPECT_EQ(growth_.grown - growth_.shrunk, net) << "the abort left bytes admitted";
    return cost;
  };

  Install(1, 64, TrTcm(500));
  Install(3, 3, TrTcm(900));  // the reconfigure's profile is live in both
  const Cost small = probe();
  Install(65, 8000, TrTcm(500));
  const Cost large = probe();

  EXPECT_EQ(large.allocations, small.allocations)
      << "glibc allocations grew with the installed meters";
  EXPECT_EQ(large.admitted, small.admitted)
      << "admitted bytes grew with the installed meters";
  EXPECT_LT(large.admitted, kLarge * sizeof(MeterState *))
      << "a four-operation transaction admitted a capacity-sized index";
}

// Many small transactions on a large set change only their meters: every
// other meter keeps its state and policy, a reader on the generation before a
// transaction keeps seeing that generation's meters, and erases' removal
// stages take out only their meter.
TEST_F(MeterResourceTest, SmallTransactionsLeaveEveryOtherMeterIntact) {
  constexpr uint32_t kLarge = 65536;
  constexpr uint32_t kInstalled = 8000;
  Recreate(kLarge);
  Install(1, kInstalled, TrTcm(500));
  std::vector<MeterState *> states(kLarge + 1);
  for (uint32_t id = 1; id <= kInstalled; ++id) {
    states[id] = published_.Read()->Lookup(MeterId(id));
  }
  auto policies = Policies();
  ASSERT_EQ(policies.size(), kInstalled);
  HoldGracePeriods();

  for (uint32_t t = 0; t < 40; ++t) {
    // Spread over leaves and inner nodes.
    const uint32_t changed = 1 + t * 197;
    const uint32_t erased = 2 + t * 197;
    const uint32_t added = kInstalled + 1 + t * 389;
    const MeterProfileSpec spec = TrTcm(700 + t);
    if (t == 20) {
      // Refused, alone or beside a valid upsert: no trace.
      Refusal(Upsert(changed, TrTcmSpec{1000, 2000, 500, 4000}));
      auto mixed = Apply({Upsert(changed, TrTcm(9999)), Upsert(0, spec)});
      EXPECT_EQ(mixed.outcome, Outcome::kRejected);
    }
    const MeterSet *old = published_.Read();
    ASSERT_EQ(Apply({Upsert(changed, spec), Upsert(added, TrTcm(500)),
                     Erase(erased)})
                  .outcome,
              Outcome::kApplied);
    const MeterSet *now = published_.Read();
    EXPECT_EQ(old->Lookup(MeterId(changed)), states[changed]);
    EXPECT_EQ(old->Lookup(MeterId(added)), nullptr);
    EXPECT_NE(now->Lookup(MeterId(changed)), states[changed]);
    EXPECT_NE(now->Lookup(MeterId(added)), nullptr);
    EXPECT_EQ(now->Lookup(MeterId(erased)), states[erased])
        << "readable until its removal stage";
    states[changed] = now->Lookup(MeterId(changed));
    states[added] = now->Lookup(MeterId(added));
    states[erased] = nullptr;
    policies[changed] = spec;
    policies[added] = TrTcm(500);
    policies.erase(erased);
    PassGracePeriod();  // retires `old`, runs the erase's removal stage
    EXPECT_EQ(published_.Read()->Lookup(MeterId(erased)), nullptr);
  }

  const MeterSet *set = published_.Read();
  size_t wrong = 0;
  for (uint32_t id = 0; id <= kLarge; ++id) {
    wrong += set->Lookup(MeterId(id)) != states[id] ? 1 : 0;
  }
  EXPECT_EQ(wrong, 0u) << "meters the transactions did not name changed";
  EXPECT_EQ(Policies(), policies);
  EXPECT_EQ(resource_->LiveCount(), policies.size());
  EXPECT_EQ(set->size(), policies.size());
  EXPECT_EQ(resource_->bytes(), growth_.grown - growth_.shrunk);
}

// Every refusal leaves the published generation the same object.
TEST_F(MeterResourceTest, RefusalsLeaveThePublishedGeneration) {
  ASSERT_EQ(Apply({Upsert(1, TrTcm(500))}).outcome, Outcome::kApplied);
  const MeterSet *before = published_.Read();

  Refusal(Upsert(0, TrTcm(500)));
  Refusal(Upsert(kCapacity + 1, TrTcm(500)));
  Refusal(Erase(3));
  Refusal(Op::Upsert("mt/meters", EncodeKey(MeterId(2)), std::any(7)));
  Refusal(Upsert(2, TrTcmSpec{1000, 2000, 500, 4000}));
  // A valid upsert refused for another operation publishes nothing either.
  auto mixed = Apply({Upsert(2, TrTcm(500)), Upsert(0, TrTcm(500))});
  EXPECT_EQ(mixed.outcome, Outcome::kRejected);

  EXPECT_EQ(published_.Read(), before);
  EXPECT_FALSE(Has(2));
  EXPECT_EQ(Visited(), (std::vector<uint32_t>{1}));
}

// An erase is absent to the control side at once, readable by packets until
// its removal stage, and its id cannot be upserted until then.
TEST_F(MeterResourceTest, EraseIsDeferredToTheRemovalStage) {
  ASSERT_EQ(Apply({Upsert(1, TrTcm(500)), Upsert(2, TrTcm(500))}).outcome,
            Outcome::kApplied);
  HoldGracePeriods();
  ASSERT_EQ(Apply({Erase(2)}).outcome, Outcome::kApplied);
  EXPECT_FALSE(Has(2));
  EXPECT_TRUE(resource_->Retiring(MeterId(2)));
  EXPECT_EQ(Visited(), (std::vector<uint32_t>{1}));
  EXPECT_NE(published_.Read()->Lookup(MeterId(2)), nullptr)
      << "still readable until the removal stage";
  Refusal(Upsert(2, TrTcm(500)));
  Refusal(Erase(2));

  PassGracePeriod();
  EXPECT_FALSE(resource_->Retiring(MeterId(2)));
  EXPECT_EQ(published_.Read()->Lookup(MeterId(2)), nullptr);
  EXPECT_NE(published_.Read()->Lookup(MeterId(1)), nullptr);
  EXPECT_EQ(resource_->LiveCount(), 1u);
  ASSERT_EQ(Apply({Upsert(2, TrTcm(900))}).outcome, Outcome::kApplied)
      << "the id is free again";
  EXPECT_TRUE(Has(2));
}

// The erase admits its removal stage's generation at Reserve: refused there,
// the erase is refused and nothing changes.
TEST_F(MeterResourceTest, EraseRefusedWhenItsRemovalStageDoesNotFit) {
  ASSERT_EQ(Apply({Upsert(1, TrTcm(500))}).outcome, Outcome::kApplied);
  const MeterSet *before = published_.Read();
  growth_.refuse = true;
  EXPECT_THROW((void)resource_->Reserve(Erase(1)), utils::MemoryRefused);
  growth_.refuse = false;
  EXPECT_TRUE(Has(1));
  EXPECT_FALSE(resource_->Retiring(MeterId(1)));
  EXPECT_EQ(published_.Read(), before);
  // An admitted erase that aborts gives its prepay back (and the glibc node
  // that marked it retiring).
  const size_t shrunk = growth_.shrunk - growth_.glibc_shrunk;
  auto reserved = resource_->Reserve(Erase(1));
  ASSERT_TRUE(reserved);
  reserved->staged->Abort();
  EXPECT_EQ(growth_.shrunk - growth_.glibc_shrunk - shrunk,
            MeterSet::Bytes(kCapacity));
  EXPECT_TRUE(Has(1));
}

// The erase's Reserve prepays its removal stage's edit; the stage spends that
// prepay, and its DPDK allocations are not checked against the memory limit
// again (D-207, utils::Headroom::kPrepaid). At the limit -- where on normal
// pages each allocation would be refused, and the stage retried every grace
// period until memory use dropped -- the stage completes: the stage's other
// step (a counter slot beside the meter, as in the UPF appliance) runs once,
// the meter is gone, and its prepay is settled exactly once.
TEST_F(MeterResourceTest, RemovalStageCompletesAtTheLimit) {
  utils::MemoryNode();  // DPDK up: its backing is known
  struct CounterTag;
  using CounterId = dataplane::StrongId<CounterTag, uint32_t>;
  struct Counter {};
  dataplane::SlotTable<CounterId, Counter> counters(kCapacity);
  dataplane::SlotResource<CounterId, Counter> counters_res("cnt/counters",
                                                           counters);
  ASSERT_TRUE(engine_.Register(&counters_res));
  auto counter = [](uint32_t id) {
    return Op::Upsert("cnt/counters", EncodeKey(CounterId(id)),
                      std::any(Counter{}));
  };
  auto erase_counter = [](uint32_t id) {
    return Op::Erase("cnt/counters", EncodeKey(CounterId(id)));
  };
  ASSERT_EQ(Apply({counter(1), counter(2), Upsert(1, TrTcm(500)),
                   Upsert(2, TrTcm(500))})
                .outcome,
            Outcome::kApplied);
  HoldGracePeriods();
  // Both rank 0, so one stage: the counter's slot, then the meter's edit.
  ASSERT_EQ(Apply({erase_counter(1), Erase(1)}).outcome, Outcome::kApplied);

  utils::MemoryPolicy saved = utils::memory_policy();
  utils::MemoryPolicy tight;
  tight.limit_bytes = 1;  // refuses every checked normal-page allocation
  tight.limit_source = "test";
  utils::SetMemoryPolicy(tight);
  domain_.Quiescent(kReader);
  dataplane::TransactionEngine::Result r;
  // Allocates nothing itself; runs the pending stage first.
  EXPECT_NO_THROW(r = Apply({erase_counter(2)}));
  utils::SetMemoryPolicy(std::move(saved));
  EXPECT_EQ(r.outcome, Outcome::kApplied);
  EXPECT_EQ(counters.size(), 0u);
  EXPECT_FALSE(resource_->Retiring(MeterId(1)));
  EXPECT_FALSE(Has(1));
  EXPECT_EQ(published_.Read()->Lookup(MeterId(1)), nullptr);
  EXPECT_EQ(Visited(), (std::vector<uint32_t>{2}));
  EXPECT_EQ(resource_->bytes(), growth_.grown - growth_.shrunk)
      << "the stage's prepay was not settled exactly once";
  EXPECT_EQ(Apply({counter(1), Upsert(1, TrTcm(900))}).outcome,
            Outcome::kApplied)
      << "both ids are free again";

  // The counters leave the engine before their table goes.
  domain_.Offline(kReader);
  domain_.Unregister(kReader);
  reader_online_ = false;
  while (engine_.ReclaimRetired() != 0) {
  }
  domain_.Drain();
  EXPECT_TRUE(engine_.Unregister("cnt/counters"));
}

// BP-7 (FM-3): a transaction's erases share one removal step, which erases
// every id from the builder and builds and publishes one generation -- not
// one generation per erased meter.
TEST_F(MeterResourceTest, AnEraseTransactionPublishesOneGeneration) {
  constexpr uint32_t kMeters = 1000;
  Recreate(1024);
  Install(1, kMeters, TrTcm(500));
  HoldGracePeriods();
  std::vector<Op> erases;
  for (uint32_t id = 1; id <= kMeters; id++) {
    erases.push_back(Erase(id));
  }
  ASSERT_EQ(Apply(std::move(erases)).outcome, Outcome::kApplied);
  const uint64_t before = resource_->generations();
  PassGracePeriod();
  RecordProperty("removal_generations",
                 static_cast<int>(resource_->generations() - before));
  EXPECT_EQ(resource_->generations() - before, 1u)
      << "generations published by the removal of " << kMeters << " meters";
  EXPECT_EQ(resource_->LiveCount(), 0u);
  EXPECT_EQ(published_.Read()->Lookup(MeterId(kMeters)), nullptr);
  EXPECT_FALSE(resource_->Retiring(MeterId(1)));
}

// The retiring ids and each meter's control block are control-plane state
// no worker reads (D-206, class C): glibc, every node admitted exactly as
// malloc's chunk (utils::MallocChunkBytes) through the owner's lease, and
// given back when the meter goes. The policies as written cost nothing of
// their own (D-207): read-back takes them from the interned profiles.
TEST_F(MeterResourceTest, ControlNodesAreAdmittedExactly) {
  // What one meter state's control block takes, under the same allocators.
  size_t state_block = 0;
  {
    CountingGrowth probe;
    MeterSetBuilder builder(kCapacity);
    builder.set_memory_growth(&probe);
    ASSERT_TRUE(builder.Add(MeterId(1), TrTcm(500), MeterSharing::kShared));
    const size_t first = probe.glibc_grown;  // the profile is interned now
    ASSERT_TRUE(builder.Add(MeterId(2), TrTcm(500), MeterSharing::kShared));
    state_block = probe.glibc_grown - first;
  }
  ASSERT_GE(state_block, utils::kMallocSmallestChunk);

  ASSERT_EQ(Apply({Upsert(1, TrTcm(500))}).outcome, Outcome::kApplied);
  while (engine_.ReclaimRetired() != 0) {
  }
  const size_t before = growth_.glibc_net();
  constexpr uint32_t kMeters = kCapacity - 1;
  Install(2, 1 + kMeters, TrTcm(500));
  while (engine_.ReclaimRetired() != 0) {
  }
  EXPECT_EQ(growth_.glibc_net() - before, kMeters * state_block);
  EXPECT_EQ(resource_->bytes(), growth_.grown - growth_.shrunk);

  std::vector<Op> erases;
  for (uint32_t id = 2; id <= 1 + kMeters; ++id) {
    erases.push_back(Erase(id));
  }
  ASSERT_EQ(engine_.ApplyBatched(erases).outcome, Outcome::kApplied);
  while (engine_.ReclaimRetired() != 0) {
  }
  domain_.Drain();  // the generations that held the erased states
  EXPECT_EQ(resource_->LiveCount(), 1u);
  EXPECT_EQ(growth_.glibc_net(), before);
  EXPECT_EQ(resource_->bytes(), growth_.grown - growth_.shrunk);
}

// BESS's codec: bess.pb.MeterIdKey -> bess.pb.MeterPolicyValue, the module's
// wire form; read-back yields what decodes to the same entries.
TEST_F(MeterResourceTest, CodecRoundTrips) {
  ASSERT_EQ(Apply({Upsert(3, TrTcm(500))}).outcome, Outcome::kApplied);
  auto codec = MakeMeterResourceCodec(*resource_);
  EXPECT_EQ(codec->key_type(), "bess.pb.MeterIdKey");
  EXPECT_EQ(codec->value_type(), "bess.pb.MeterPolicyValue");

  bess::pb::MeterIdKey zero;
  auto refused = codec->Key("type.googleapis.com/bess.pb.MeterIdKey",
                            zero.SerializeAsString());
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), "meter id 0 is invalid");

  int entries = 0;
  codec->Entries([&](const google::protobuf::Message &key,
                     const google::protobuf::Message &value) {
    entries++;
    auto k = codec->Key("type.googleapis.com/bess.pb.MeterIdKey",
                        key.SerializeAsString());
    ASSERT_TRUE(k);
    EXPECT_EQ(*k, EncodeKey(MeterId(3)));
    auto v = codec->Value("type.googleapis.com/bess.pb.MeterPolicyValue",
                          value.SerializeAsString());
    ASSERT_TRUE(v);
    EXPECT_EQ(std::any_cast<MeterProfileSpec>(*v), TrTcm(500));
  });
  EXPECT_EQ(entries, 1);
  EXPECT_FALSE(MeterPolicyFromPb(bess::pb::MeterPolicyValue()));
}

}  // namespace
}  // namespace bess::meter
