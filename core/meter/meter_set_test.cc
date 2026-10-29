// SPDX-License-Identifier: BSD-3-Clause

// K5: MeterSet generations -- ids, profile interning, state that survives
// unrelated updates, the batch surface, and RCU publication.

#include "meter/meter_set.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <vector>

#include "framework/memory_scope.h"
#include "runtime/runtime_state.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"
#include "utils/dpdk_memory.h"
#include "utils/dpdk_slab.h"

namespace bess::meter {
namespace {

// One byte per second and a 1000-byte committed bucket: nothing refills while
// a test runs, so a drained meter stays drained.
constexpr TrTcmSpec kSlow{1, 1000, 1, 1000};
constexpr TrTcmSpec kSlowBig{1, 5000, 1, 5000};

void MustAdd(MeterSetBuilder &builder, uint32_t id,
             const MeterProfileSpec &spec = kSlow,
             MeterSharing sharing = MeterSharing::kWorkerExclusive) {
  auto added = builder.Add(MeterId(id), spec, sharing);
  ASSERT_TRUE(added.has_value()) << MeterErrorName(added.error());
}

// Drains a meter's committed and peak buckets as of now.
void Drain(MeterState &meter) {
  const uint64_t now = MeterNow();
  while (meter.Check(now, 100) != MeterColor::kRed) {
  }
}

TEST(MeterSetBuilderTest, IdRules) {
  MeterSetBuilder builder(4);
  EXPECT_EQ(MeterError::kInvalidId,
            builder.Add(kInvalidMeterId, kSlow, MeterSharing::kShared).error());
  EXPECT_EQ(MeterError::kInvalidId,
            builder.Add(MeterId(5), kSlow, MeterSharing::kShared).error());
  MustAdd(builder, 4);
  EXPECT_EQ(MeterError::kDuplicateId,
            builder.Add(MeterId(4), kSlow, MeterSharing::kShared).error());
  EXPECT_EQ(MeterError::kUnknownId, builder.Erase(MeterId(3)).error());
  EXPECT_EQ(MeterError::kInvalidId, builder.Erase(MeterId(9)).error());
  EXPECT_EQ(MeterError::kUnknownId,
            builder.Reconfigure(MeterId(1), kSlow).error());
  EXPECT_EQ(MeterError::kUnknownId, builder.Reset(MeterId(1)).error());
  EXPECT_EQ(1u, builder.size());
  EXPECT_EQ(4u, builder.capacity());
}

TEST(MeterSetBuilderTest, InvalidSpecLeavesBuilderUnchanged) {
  MeterSetBuilder builder(4);
  EXPECT_EQ(MeterError::kPeakBelowCommitted,
            builder.Add(MeterId(1), TrTcmSpec{10, 10, 5, 10},
                        MeterSharing::kShared)
                .error());
  EXPECT_FALSE(builder.Contains(MeterId(1)));

  MustAdd(builder, 1);
  auto set = builder.Build();
  const MeterState *before = set->Lookup(MeterId(1));
  EXPECT_EQ(MeterError::kZeroBursts,
            builder.Reconfigure(MeterId(1), SrTcmSpec{10, 0, 0}).error());
  auto after = builder.Build();
  EXPECT_EQ(before, after->Lookup(MeterId(1)));
  EXPECT_EQ(&before->profile(), &after->Lookup(MeterId(1))->profile());
}

TEST(MeterSetTest, LookupAndProfileInterning) {
  MeterSetBuilder builder(8);
  MustAdd(builder, 1);
  MustAdd(builder, 2);
  MustAdd(builder, 3, kSlowBig);
  auto set = builder.Build();

  EXPECT_EQ(3u, set->size());
  EXPECT_EQ(8u, set->capacity());
  EXPECT_EQ(nullptr, set->Lookup(kInvalidMeterId));
  EXPECT_EQ(nullptr, set->Lookup(MeterId(4)));
  EXPECT_EQ(nullptr, set->Lookup(MeterId(100)));

  MeterState *a = set->Lookup(MeterId(1));
  MeterState *b = set->Lookup(MeterId(2));
  MeterState *c = set->Lookup(MeterId(3));
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  ASSERT_NE(nullptr, c);
  EXPECT_EQ(&a->profile(), &b->profile()) << "identical specs share one profile";
  EXPECT_NE(&a->profile(), &c->profile());
  EXPECT_NE(a, b) << "every meter has its own state";
}

// The property that makes meters different from other published objects:
// publishing a new generation for an unrelated change keeps every surviving
// meter's buckets exactly where they were.
TEST(MeterSetTest, StateSurvivesUnrelatedGenerations) {
  MeterSetBuilder builder(8);
  MustAdd(builder, 1);
  auto gen1 = builder.Build();
  Drain(*gen1->Lookup(MeterId(1)));

  MustAdd(builder, 2);
  ASSERT_TRUE(builder.Reconfigure(MeterId(1), kSlow).has_value());  // same spec
  auto gen2 = builder.Build();

  EXPECT_EQ(gen1->Lookup(MeterId(1)), gen2->Lookup(MeterId(1)));
  EXPECT_EQ(MeterColor::kRed, gen2->Lookup(MeterId(1))->Check(MeterNow(), 100));
  EXPECT_EQ(MeterColor::kGreen,
            gen2->Lookup(MeterId(2))->Check(MeterNow(), 100));
}

TEST(MeterSetTest, ReconfigureAndResetStartFreshState) {
  MeterSetBuilder builder(8);
  MustAdd(builder, 1);
  MustAdd(builder, 2);
  auto gen1 = builder.Build();
  Drain(*gen1->Lookup(MeterId(1)));
  Drain(*gen1->Lookup(MeterId(2)));

  ASSERT_TRUE(builder.Reconfigure(MeterId(1), kSlowBig).has_value());
  ASSERT_TRUE(builder.Reset(MeterId(2)).has_value());
  auto gen2 = builder.Build();

  MeterState *m1 = gen2->Lookup(MeterId(1));
  MeterState *m2 = gen2->Lookup(MeterId(2));
  EXPECT_NE(gen1->Lookup(MeterId(1)), m1);
  EXPECT_NE(&gen1->Lookup(MeterId(1))->profile(), &m1->profile());
  EXPECT_NE(gen1->Lookup(MeterId(2)), m2);
  EXPECT_EQ(&gen1->Lookup(MeterId(2))->profile(), &m2->profile());
  EXPECT_EQ(MeterColor::kGreen, m1->Check(MeterNow(), 5000));
  EXPECT_EQ(MeterColor::kGreen, m2->Check(MeterNow(), 1000));

  // The old generation still runs against its own (old) state.
  EXPECT_EQ(MeterColor::kRed, gen1->Lookup(MeterId(1))->Check(MeterNow(), 1));
}

TEST(MeterSetTest, EraseAndReaddIsANewMeter) {
  MeterSetBuilder builder(8);
  MustAdd(builder, 1);
  auto gen1 = builder.Build();
  Drain(*gen1->Lookup(MeterId(1)));

  ASSERT_TRUE(builder.Erase(MeterId(1)).has_value());
  auto gen2 = builder.Build();
  EXPECT_EQ(nullptr, gen2->Lookup(MeterId(1)));
  EXPECT_EQ(0u, gen2->size());

  MustAdd(builder, 1);
  auto gen3 = builder.Build();
  EXPECT_NE(gen1->Lookup(MeterId(1)), gen3->Lookup(MeterId(1)));
  EXPECT_EQ(MeterColor::kGreen,
            gen3->Lookup(MeterId(1))->Check(MeterNow(), 100));
}

// Slab slots are reused only after every generation referencing the old state
// is gone -- while one lives, a re-added meter must land somewhere else, or an
// old-generation reader would be updating the new meter's buckets.
TEST(MeterSetTest, SlabSlotReusedOnlyAfterLastGeneration) {
  MeterSetBuilder builder(4);
  MustAdd(builder, 1);
  auto gen1 = builder.Build();
  MeterState *const original = gen1->Lookup(MeterId(1));

  ASSERT_TRUE(builder.Erase(MeterId(1)).has_value());
  MustAdd(builder, 1);
  auto gen2 = builder.Build();
  EXPECT_NE(original, gen2->Lookup(MeterId(1)))
      << "slot reused while gen1 still maps it";

  ASSERT_TRUE(builder.Erase(MeterId(1)).has_value());
  gen2.reset();
  gen1.reset();  // `original` now has no owner and its slot is free
  MustAdd(builder, 1);
  auto gen3 = builder.Build();
  MeterState *const reused = gen3->Lookup(MeterId(1));
  // gen2's state was released first, then `original` (gen1's): the free list
  // is LIFO, so `original`'s slot is the one handed out again.
  EXPECT_EQ(original, reused);
  EXPECT_EQ(MeterColor::kGreen, reused->Check(MeterNow(), 100))
      << "a recycled slot starts with full buckets";
}

TEST(MeterSetTest, StatesFromOneSlabAreContiguous) {
  MeterSetBuilder builder(8);
  for (uint32_t id = 1; id <= 8; id++) {
    MustAdd(builder, id);
  }
  auto set = builder.Build();
  for (uint32_t id = 2; id <= 8; id++) {
    EXPECT_EQ(set->Lookup(MeterId(id - 1)) + 1, set->Lookup(MeterId(id)));
  }
}

// A generation owns what it maps: it keeps working after the builder that
// produced it -- and every later generation -- is gone.
TEST(MeterSetTest, GenerationOutlivesBuilder) {
  std::unique_ptr<const MeterSet> set;
  {
    MeterSetBuilder builder(4);
    MustAdd(builder, 1, SrTcmSpec{1, 300, 0});
    set = builder.Build();
    ASSERT_TRUE(builder.Erase(MeterId(1)).has_value());
    auto later = builder.Build();
  }
  MeterState *meter = set->Lookup(MeterId(1));
  ASSERT_NE(nullptr, meter);
  EXPECT_EQ(MeterColor::kGreen, meter->Check(MeterNow(), 300));
  EXPECT_EQ(MeterColor::kRed, meter->Check(MeterNow(), 1));
}

// The index is shared, not copied (D-199): a generation and a clone start
// with the builder's nodes, and an edit copies only its id's path. So no
// edit -- on either side, across leaves and inner nodes, an erase that
// empties a leaf included -- may change what a generation or the other side
// maps.
TEST(MeterSetTest, EditsNeverChangeASharedIndex) {
  constexpr size_t kCapacity = 70000;  // two inner levels above the leaves
  MeterSetBuilder builder(kCapacity);
  for (uint32_t id = 1; id <= kCapacity; id += 37) {
    MustAdd(builder, id);
  }
  const size_t installed = builder.size();
  auto gen1 = builder.Build();
  std::vector<MeterState *> before(kCapacity + 1);
  for (uint32_t id = 0; id <= kCapacity; id++) {
    before[id] = gen1->Lookup(MeterId(id));
  }

  // The clone: a reconfigure, a reset, the erase of both meters of ids
  // 64..127's leaf, and two adds far apart.
  auto clone = builder.Clone();
  ASSERT_TRUE(clone->Reconfigure(MeterId(38), kSlowBig).has_value());
  ASSERT_TRUE(clone->Reset(MeterId(149)).has_value());
  ASSERT_TRUE(clone->Erase(MeterId(75)).has_value());
  ASSERT_TRUE(clone->Erase(MeterId(112)).has_value());
  MustAdd(*clone, 2);
  MustAdd(*clone, 69999);
  // The builder, other edits of the same meters.
  ASSERT_TRUE(builder.Reconfigure(MeterId(75), kSlowBig).has_value());
  ASSERT_TRUE(builder.Erase(MeterId(38)).has_value());
  MustAdd(builder, 3);

  auto cloned = clone->Build();
  auto built = builder.Build();
  EXPECT_EQ(installed, cloned->size());
  EXPECT_EQ(installed, built->size());

  // Every id the other side changed is as gen1 had it.
  auto unchanged_except = [&](const MeterSet &set,
                              std::initializer_list<uint32_t> changed) {
    size_t wrong = 0;
    for (uint32_t id = 0; id <= kCapacity; id++) {
      if (std::find(changed.begin(), changed.end(), id) == changed.end() &&
          set.Lookup(MeterId(id)) != before[id]) {
        wrong++;
      }
    }
    return wrong;
  };
  EXPECT_EQ(0u, unchanged_except(*gen1, {}));
  EXPECT_EQ(0u, unchanged_except(*cloned, {38, 149, 75, 112, 2, 69999}));
  EXPECT_EQ(0u, unchanged_except(*built, {75, 38, 3}));

  ASSERT_NE(nullptr, cloned->Lookup(MeterId(38)));
  EXPECT_NE(before[38], cloned->Lookup(MeterId(38)));
  EXPECT_TRUE(cloned->Lookup(MeterId(38))->profile().spec() ==
              MeterProfileSpec(kSlowBig));
  ASSERT_NE(nullptr, cloned->Lookup(MeterId(149)));
  EXPECT_NE(before[149], cloned->Lookup(MeterId(149)));
  EXPECT_EQ(nullptr, cloned->Lookup(MeterId(75)));
  EXPECT_EQ(nullptr, cloned->Lookup(MeterId(112)));
  EXPECT_NE(nullptr, cloned->Lookup(MeterId(2)));
  EXPECT_NE(nullptr, cloned->Lookup(MeterId(69999)));
  EXPECT_EQ(nullptr, cloned->Lookup(MeterId(3)));

  ASSERT_NE(nullptr, built->Lookup(MeterId(75)));
  EXPECT_NE(before[75], built->Lookup(MeterId(75)));
  EXPECT_EQ(nullptr, built->Lookup(MeterId(38)));
  EXPECT_NE(nullptr, built->Lookup(MeterId(3)));
  EXPECT_EQ(nullptr, built->Lookup(MeterId(2)));
  EXPECT_EQ(nullptr, built->Lookup(MeterId(69999)));

  // The builders agree with their generations.
  EXPECT_FALSE(clone->Contains(MeterId(75)));
  EXPECT_TRUE(builder.Contains(MeterId(75)));
  EXPECT_TRUE(clone->Contains(MeterId(38)));
  EXPECT_FALSE(builder.Contains(MeterId(38)));
}

TEST(MeterSetTest, CheckBatchReportsResolvedPositions) {
  MeterSetBuilder builder(8);
  MustAdd(builder, 1, SrTcmSpec{1, 150, 0});
  MustAdd(builder, 2, SrTcmSpec{1, 150, 0});
  MustAdd(builder, 3);
  ASSERT_TRUE(builder.Erase(MeterId(3)).has_value());
  auto set = builder.Build();

  const std::array<MeterId, 6> ids = {MeterId(1), kInvalidMeterId, MeterId(2),
                                      MeterId(3), MeterId(1), MeterId(99)};
  const std::array<uint32_t, 6> bytes = {100, 100, 100, 100, 100, 100};
  std::array<MeterColor, 6> colors;
  colors.fill(MeterColor::kYellow);  // sentinel: no algorithm here yields it

  const uint64_t mask = set->CheckBatch(ids, bytes, colors, MeterNow());
  EXPECT_EQ(0b010101u, mask);
  EXPECT_EQ(MeterColor::kGreen, colors[0]);
  EXPECT_EQ(MeterColor::kYellow, colors[1]);
  EXPECT_EQ(MeterColor::kGreen, colors[2]);
  EXPECT_EQ(MeterColor::kYellow, colors[3]);
  EXPECT_EQ(MeterColor::kRed, colors[4]) << "meter 1 was used twice";
  EXPECT_EQ(MeterColor::kYellow, colors[5]);
}

TEST(MeterSetTest, CheckBatchColorAware) {
  MeterSetBuilder builder(4);
  MustAdd(builder, 1, kSlowBig);
  auto set = builder.Build();

  const std::array<MeterId, 3> ids = {MeterId(1), MeterId(1), MeterId(2)};
  const std::array<uint32_t, 3> bytes = {100, 100, 100};
  std::array<MeterColor, 3> colors = {MeterColor::kRed, MeterColor::kGreen,
                                      MeterColor::kYellow};
  const uint64_t mask =
      set->CheckBatchColorAware(ids, bytes, colors, MeterNow());
  EXPECT_EQ(0b011u, mask);
  EXPECT_EQ(MeterColor::kRed, colors[0]);
  EXPECT_EQ(MeterColor::kGreen, colors[1]);
  EXPECT_EQ(MeterColor::kYellow, colors[2]) << "unresolved: input kept";
}

// Publication through the runtime's RCU domain, with an online reader: the
// retired generation -- and the state only it references -- is reclaimed once
// the reader passes a quiescent state, while the surviving meter's state
// carries on in the new generation.
TEST(MeterSetPublicationTest, PublishRetiresOldGeneration) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  const uint32_t reader = 1;
  ASSERT_TRUE(domain.Register(reader).has_value());
  domain.Online(reader);

  MeterSetBuilder builder(8);
  MustAdd(builder, 1);
  MustAdd(builder, 2);
  rcu::RcuPtr<MeterSet> published(domain);
  published.Initialize(builder.Build());

  const MeterSet *seen = published.Read();
  MeterState *survivor = seen->Lookup(MeterId(1));
  Drain(*seen->Lookup(MeterId(1)));

  ASSERT_TRUE(builder.Erase(MeterId(2)).has_value());
  const rcu::GracePeriod token = published.Publish(builder.Build());
  EXPECT_FALSE(domain.IsComplete(token));

  domain.Quiescent(reader);
  EXPECT_TRUE(domain.IsComplete(token));
  domain.ReclaimReady();

  const MeterSet *current = published.Read();
  EXPECT_EQ(nullptr, current->Lookup(MeterId(2)));
  EXPECT_EQ(survivor, current->Lookup(MeterId(1)));
  EXPECT_EQ(MeterColor::kRed, current->Lookup(MeterId(1))->Check(MeterNow(), 1));

  domain.Offline(reader);
  domain.Unregister(reader);
  published.ResetQuiesced();
  domain.Drain();
}

// -- memory (D-198) -----------------------------------------------------------

TEST(MeterSetMemoryTest, BytesMatchesConstruction) {
  for (const size_t capacity : {size_t{1}, size_t{64}, size_t{4096}}) {
    MeterSetBuilder builder(capacity);
    EXPECT_EQ(bess::framework::MemoryCost<MeterSetBuilder>::Bytes(capacity),
              builder.bytes());
  }
}

// Admits every growth and records it, refusing once `allow` growths passed.
// Declared before the builders it serves: it must outlive them.
struct RecordingGrowth final : bess::utils::MemoryGrowth {
  std::vector<size_t> grown, grown_glibc;
  size_t net = 0, glibc_net = 0;
  size_t allow = SIZE_MAX;
  void Grow(size_t bytes, size_t glibc_bytes) override {
    if (grown.size() >= allow) {
      throw bess::utils::MemoryRefused(
          bess::utils::MemoryRefused::Reason::kNormalLimit, bytes, "refused");
    }
    grown.push_back(bytes);
    grown_glibc.push_back(glibc_bytes);
    net += bytes;
    glibc_net += glibc_bytes;
  }
  void Shrink(size_t bytes, size_t glibc_bytes) noexcept override {
    net -= bytes;
    glibc_net -= glibc_bytes;
  }
};

TEST(MeterSetMemoryTest, GrowthIsAdmittedAndGivenBack) {
  RecordingGrowth growth;
  constexpr size_t kCapacity = 16;
  MeterSetBuilder builder(kCapacity);
  builder.set_memory_growth(&growth);
  const size_t initial = builder.bytes();

  // A meter: its profile and state control block (glibc) and the first slab
  // chunk of states (DPDK).
  MustAdd(builder, 1);
  ASSERT_EQ(3u, growth.grown.size());
  EXPECT_EQ(bess::utils::ObjectSlab<MeterState>::chunk_bytes(),
            growth.grown[1]);
  EXPECT_EQ(0u, growth.grown_glibc[1]);
  EXPECT_EQ(growth.grown[0], growth.grown_glibc[0]);
  EXPECT_EQ(growth.grown[2], growth.grown_glibc[2]);
  EXPECT_EQ(initial + growth.net, builder.bytes());

  // A generation and a clone admit nothing: they share the index. The next
  // edit copies the node it shares (here the root leaf), once.
  auto generation = builder.Build();
  auto clone = builder.Clone();
  ASSERT_EQ(3u, growth.grown.size());
  MustAdd(builder, 2);
  ASSERT_EQ(5u, growth.grown.size()) << "a state control block and the leaf";
  EXPECT_EQ(detail::kMeterLeafBytes, growth.grown[4]);
  EXPECT_EQ(0u, growth.grown_glibc[4]);
  MustAdd(builder, 3);
  EXPECT_EQ(6u, growth.grown.size()) << "the leaf is the builder's now";
  EXPECT_EQ(initial + growth.net, builder.bytes());
  // What only they held is given back when they go.
  generation.reset();
  clone.reset();
  EXPECT_EQ(initial + growth.net, builder.bytes());

  // An erased meter's state goes with its last reference; its slab chunk
  // stays.
  const size_t glibc_with_meter = growth.glibc_net;
  ASSERT_TRUE(builder.Erase(MeterId(1)).has_value());
  EXPECT_LT(growth.glibc_net, glibc_with_meter);
  EXPECT_EQ(initial + growth.net, builder.bytes());
}

// Build admits nothing, and one edit of one meter admits at most
// MeterSet::Bytes(capacity) of index nodes (DPDK): exactly that when its
// whole path is shared and its leaf keeps other meters. An erase's removal
// stage is prepaid that much (D-199).
TEST(MeterSetMemoryTest, OneEditAdmitsAtMostOnePath) {
  constexpr size_t kCapacity = 65536;  // two inner levels
  RecordingGrowth growth;
  MeterSetBuilder builder(kCapacity);
  builder.set_memory_growth(&growth);
  for (uint32_t id = 1; id <= 300; id++) {
    MustAdd(builder, id);
  }
  // The index admitted since `from`, and that it is all DPDK memory.
  auto index_bytes = [&](size_t from) {
    size_t bytes = 0;
    for (size_t i = from; i < growth.grown.size(); i++) {
      bytes += growth.grown[i] - growth.grown_glibc[i];
    }
    return bytes;
  };

  size_t from = growth.grown.size();
  auto generation = builder.Build();
  auto again = builder.Build();
  EXPECT_EQ(from, growth.grown.size()) << "Build admitted";

  ASSERT_TRUE(builder.Erase(MeterId(1)).has_value());
  EXPECT_EQ(MeterSet::Bytes(kCapacity), index_bytes(from));
  from = growth.grown.size();
  ASSERT_TRUE(builder.Erase(MeterId(2)).has_value());
  EXPECT_EQ(from, growth.grown.size()) << "the path is the builder's now";
  // Another leaf under the same, now owned, inner nodes: the leaf only.
  ASSERT_TRUE(builder.Reset(MeterId(200)).has_value());
  EXPECT_EQ(detail::kMeterLeafBytes, index_bytes(from));
  EXPECT_LT(detail::kMeterLeafBytes, MeterSet::Bytes(kCapacity));
}

TEST(MeterSetMemoryTest, RefusedGrowthLeavesTheBuilderUnchanged) {
  RecordingGrowth growth;
  MeterSetBuilder builder(16);
  builder.set_memory_growth(&growth);
  const size_t initial = builder.bytes();

  // The profile refused.
  growth.allow = 0;
  EXPECT_THROW((void)builder.Add(MeterId(1), kSlow, MeterSharing::kShared),
               bess::utils::MemoryRefused);
  EXPECT_EQ(0u, growth.net);
  // The profile and the slab chunk admitted, the state refused: the meter
  // is not added and its slot goes back; the chunk stays with the slab, and
  // the profile's block with its interned weak reference.
  growth.allow = 2;
  EXPECT_THROW((void)builder.Add(MeterId(1), kSlow, MeterSharing::kShared),
               bess::utils::MemoryRefused);
  EXPECT_FALSE(builder.Contains(MeterId(1)));
  EXPECT_EQ(0u, builder.size());
  ASSERT_EQ(2u, growth.grown.size());
  EXPECT_EQ(growth.grown[0], growth.glibc_net);
  EXPECT_EQ(bess::utils::ObjectSlab<MeterState>::chunk_bytes() +
                growth.grown[0],
            growth.net);
  EXPECT_EQ(initial + growth.net, builder.bytes());

  // Build and Clone admit nothing, so a hook refusing everything still
  // builds and clones; the next edit's copy is refused and changes nothing.
  growth.allow = SIZE_MAX;
  MustAdd(builder, 1);
  growth.allow = growth.grown.size();
  auto generation = builder.Build();
  auto clone = builder.Clone();
  EXPECT_THROW({ [[maybe_unused]] auto erased = builder.Erase(MeterId(1)); },
               bess::utils::MemoryRefused);
  EXPECT_TRUE(builder.Contains(MeterId(1)));
  EXPECT_EQ(1u, builder.size());
  EXPECT_EQ(generation->Lookup(MeterId(1)), builder.Build()->Lookup(MeterId(1)));
  EXPECT_EQ(initial + growth.net, builder.bytes());

  growth.allow = SIZE_MAX;
  ASSERT_TRUE(builder.Erase(MeterId(1)).has_value());
  EXPECT_FALSE(builder.Contains(MeterId(1)));
  EXPECT_NE(nullptr, generation->Lookup(MeterId(1)));
  EXPECT_EQ(initial + growth.net, builder.bytes());
}

}  // namespace
}  // namespace bess::meter
