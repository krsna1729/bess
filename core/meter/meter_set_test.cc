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

// K5: MeterSet generations -- ids, profile interning, state that survives
// unrelated updates, the batch surface, and RCU publication.

#include "meter/meter_set.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>

#include "control/runtime_state.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"

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
  EXPECT_EQ(2u, set->profile_count());
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
  EXPECT_EQ(0u, gen2->profile_count());

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
  rcu::RcuDomain &domain = control::runtime().rcu();
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

}  // namespace
}  // namespace bess::meter
