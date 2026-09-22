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

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "dataplane/action_id.h"
#include "dataplane/object_table.h"
#include "dataplane/strong_id.h"

namespace {

using bess::dataplane::ActionId;
using bess::dataplane::kInvalidActionId;
using bess::dataplane::ObjectTable;
using bess::dataplane::ObjectTableBuilder;
using bess::dataplane::StrongId;

// A second id type, to pin that different ids are different types.
struct NextHopIdTag;
using NextHopId = StrongId<NextHopIdTag, uint32_t>;

// A wider id, the escape hatch for anyone who genuinely needs one.
struct WideIdTag;
using WideId = StrongId<WideIdTag, uint64_t>;

// -- StrongId: the compile-time contract ------------------------------------

static_assert(sizeof(ActionId) == sizeof(uint32_t));
static_assert(alignof(ActionId) == alignof(uint32_t));
static_assert(std::is_trivially_copyable_v<ActionId>);
static_assert(std::is_standard_layout_v<ActionId>);
static_assert(std::is_trivially_destructible_v<ActionId>);
static_assert(sizeof(WideId) == sizeof(uint64_t));

// No implicit conversion in either direction, and none between id types.
static_assert(!std::is_convertible_v<uint32_t, ActionId>);
static_assert(!std::is_convertible_v<ActionId, uint32_t>);
static_assert(!std::is_convertible_v<ActionId, NextHopId>);
static_assert(!std::is_convertible_v<NextHopId, ActionId>);
static_assert(!std::is_same_v<ActionId, NextHopId>);

// Different id types are not comparable to each other.
static_assert(!std::equality_comparable_with<ActionId, NextHopId>);
static_assert(!std::three_way_comparable_with<ActionId, NextHopId>);

// Same type: equality and ordering work, and are constexpr.
static_assert(ActionId(1) == ActionId(1));
static_assert(ActionId(1) != ActionId(2));
static_assert(ActionId(1) < ActionId(2));
static_assert(kInvalidActionId == ActionId{});

TEST(StrongIdTest, ZeroIsInvalidAndValuesArePreserved) {
  EXPECT_EQ(0u, kInvalidActionId.value());
  EXPECT_EQ(0u, ActionId{}.value());
  EXPECT_EQ(0u, ActionId(0).value());

  EXPECT_EQ(1u, ActionId(1).value());
  EXPECT_EQ(4294967295u, ActionId(4294967295u).value());

  // The escape hatch: a 64-bit id keeps its full value.
  EXPECT_EQ(0x1'0000'0001ull, WideId(0x1'0000'0001ull).value());
}

// -- ObjectTable -------------------------------------------------------------

struct Action {
  int gate = 0;
  std::string name;
};

using Table = ObjectTable<ActionId, Action>;

TEST(ObjectTableTest, BasicLookup) {
  ObjectTableBuilder<ActionId, Action> builder(8);
  ASSERT_TRUE(builder.Set(ActionId(1), Action{10, "a"}));
  ASSERT_TRUE(builder.Set(ActionId(2), Action{20, "b"}));
  EXPECT_EQ(2u, builder.size());

  std::unique_ptr<const Table> table = std::move(builder).Build();
  ASSERT_NE(nullptr, table);

  const Action *a = table->Lookup(ActionId(1));
  const Action *b = table->Lookup(ActionId(2));
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  EXPECT_EQ(10, a->gate);
  EXPECT_EQ("a", a->name);
  EXPECT_EQ(20, b->gate);
  EXPECT_EQ("b", b->name);
  EXPECT_EQ(2u, table->size());
  EXPECT_EQ(8u, table->capacity());
}

TEST(ObjectTableTest, InvalidIdIsNotAnObject) {
  ObjectTableBuilder<ActionId, Action> builder(4);
  ASSERT_TRUE(builder.Set(ActionId(1), Action{1, "a"}));
  std::unique_ptr<const Table> table = std::move(builder).Build();

  EXPECT_EQ(nullptr, table->Lookup(kInvalidActionId));
  EXPECT_EQ(nullptr, table->Lookup(ActionId(0)));
  EXPECT_FALSE(table->Contains(kInvalidActionId));
}

TEST(ObjectTableTest, OutOfRangeIdIsNotAnObject) {
  ObjectTableBuilder<ActionId, Action> builder(4);
  ASSERT_TRUE(builder.Set(ActionId(4), Action{4, "d"}));
  std::unique_ptr<const Table> table = std::move(builder).Build();

  EXPECT_NE(nullptr, table->Lookup(ActionId(4)));  // last usable slot
  EXPECT_EQ(nullptr, table->Lookup(ActionId(5)));  // one past the capacity
  EXPECT_EQ(nullptr, table->Lookup(ActionId(1u << 31)));
}

TEST(ObjectTableTest, HoleIsEmptyButNotReused) {
  ObjectTableBuilder<ActionId, Action> builder(8);
  ASSERT_TRUE(builder.Set(ActionId(1), Action{1, "a"}));
  ASSERT_TRUE(builder.Set(ActionId(2), Action{2, "b"}));
  ASSERT_TRUE(builder.Set(ActionId(3), Action{3, "c"}));

  // No compaction and no renumbering.
  ASSERT_TRUE(builder.Erase(ActionId(2)));
  EXPECT_FALSE(builder.Contains(ActionId(2)));
  EXPECT_EQ(2u, builder.size());

  std::unique_ptr<const Table> table = std::move(builder).Build();
  EXPECT_NE(nullptr, table->Lookup(ActionId(1)));
  EXPECT_EQ(nullptr, table->Lookup(ActionId(2))) << "an erased slot is empty";
  EXPECT_NE(nullptr, table->Lookup(ActionId(3)))
      << "surviving ids must not move";
  EXPECT_EQ(3u, table->Lookup(ActionId(3))->gate);
}

TEST(ObjectTableTest, EraseDoesNotHandTheIdToAnyoneElse) {
  ObjectTableBuilder<ActionId, Action> builder(8);
  ASSERT_TRUE(builder.Set(ActionId(2), Action{2, "b"}));
  ASSERT_TRUE(builder.Erase(ActionId(2)));

  // Nothing in the builder reuses id 2; the caller chooses ids, and reuse is a
  // semantic decision that belongs to whoever allocates them.
  std::unique_ptr<const Table> table = std::move(builder).Build();
  EXPECT_EQ(nullptr, table->Lookup(ActionId(2)));
  EXPECT_EQ(0u, table->size());

  // Explicitly reusing it is allowed -- and is then the caller's responsibility.
  ObjectTableBuilder<ActionId, Action> second(8);
  ASSERT_TRUE(second.Set(ActionId(2), Action{22, "b2"}));
  std::unique_ptr<const Table> reused = std::move(second).Build();
  ASSERT_NE(nullptr, reused->Lookup(ActionId(2)));
  EXPECT_EQ(22, reused->Lookup(ActionId(2))->gate);
}

TEST(ObjectTableTest, ReplacementKeepsTheId) {
  ObjectTableBuilder<ActionId, Action> first(16);
  ASSERT_TRUE(first.Set(ActionId(10), Action{1, "v1"}));
  std::unique_ptr<const Table> gen1 = std::move(first).Build();

  ObjectTableBuilder<ActionId, Action> second(16);
  ASSERT_TRUE(second.Set(ActionId(10), Action{2, "v2"}));
  std::unique_ptr<const Table> gen2 = std::move(second).Build();

  ASSERT_NE(nullptr, gen1->Lookup(ActionId(10)));
  ASSERT_NE(nullptr, gen2->Lookup(ActionId(10)));
  EXPECT_EQ(1, gen1->Lookup(ActionId(10))->gate) << "generations are immutable";
  EXPECT_EQ(2, gen2->Lookup(ActionId(10))->gate)
      << "the id is stable, the object behind it changed";
}

TEST(ObjectTableTest, CapacityIsEnforced) {
  ObjectTableBuilder<ActionId, Action> builder(2);
  EXPECT_TRUE(builder.Set(ActionId(1), Action{1, "a"}));
  EXPECT_TRUE(builder.Set(ActionId(2), Action{2, "b"}));
  EXPECT_FALSE(builder.Set(ActionId(3), Action{3, "c"}))
      << "beyond capacity must be rejected, not silently grown";
  EXPECT_FALSE(builder.Set(ActionId(0), Action{0, "invalid"}));
  EXPECT_FALSE(builder.Erase(ActionId(3)));
  EXPECT_EQ(2u, builder.capacity());
  EXPECT_EQ(2u, builder.size());
}

TEST(ObjectTableTest, MoveOnlyObjectsAreSupported) {
  struct MoveOnly {
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(const MoveOnly &) = delete;
    MoveOnly &operator=(const MoveOnly &) = delete;
    MoveOnly(MoveOnly &&) = default;
    MoveOnly &operator=(MoveOnly &&) = default;
    int value;
  };

  ObjectTableBuilder<ActionId, MoveOnly> builder(4);
  ASSERT_TRUE(builder.Emplace(ActionId(1), 42));
  std::unique_ptr<const ObjectTable<ActionId, MoveOnly>> table =
      std::move(builder).Build();
  ASSERT_NE(nullptr, table->Lookup(ActionId(1)));
  EXPECT_EQ(42, table->Lookup(ActionId(1))->value);
}

namespace {
struct DestructionCounter {
  static int alive;
  static int destroyed;

  explicit DestructionCounter(int v) : value(v) { alive++; }
  ~DestructionCounter() {
    alive--;
    destroyed++;
  }
  DestructionCounter(const DestructionCounter &) = delete;
  DestructionCounter &operator=(const DestructionCounter &) = delete;
  DestructionCounter(DestructionCounter &&other) noexcept
      : value(other.value) {
    alive++;
  }

  int value;
};

int DestructionCounter::alive = 0;
int DestructionCounter::destroyed = 0;
}  // namespace

TEST(ObjectTableTest, EveryObjectIsDestroyedExactlyOnce) {
  DestructionCounter::alive = 0;
  DestructionCounter::destroyed = 0;

  {
    ObjectTableBuilder<ActionId, DestructionCounter> builder(8);
    for (uint32_t i = 1; i <= 5; i++) {
      ASSERT_TRUE(builder.Emplace(ActionId(i), static_cast<int>(i)));
    }
    ASSERT_TRUE(builder.Erase(ActionId(3)));  // destroys one immediately
    EXPECT_EQ(4, DestructionCounter::alive);

    std::unique_ptr<const ObjectTable<ActionId, DestructionCounter>> table =
        std::move(builder).Build();
    EXPECT_EQ(4, DestructionCounter::alive);
    EXPECT_EQ(1, DestructionCounter::destroyed);

    // The table owns the rest until it goes away.
  }

  EXPECT_EQ(0, DestructionCounter::alive);
  EXPECT_EQ(5, DestructionCounter::destroyed);
}

// -- Batch lookup ------------------------------------------------------------

TEST(ObjectTableBatchTest, MatchesScalarLookupForEveryCase) {
  ObjectTableBuilder<ActionId, Action> builder(16);
  ASSERT_TRUE(builder.Set(ActionId(1), Action{1, "one"}));
  ASSERT_TRUE(builder.Set(ActionId(2), Action{2, "two"}));
  ASSERT_TRUE(builder.Set(ActionId(5), Action{5, "five"}));
  ASSERT_TRUE(builder.Set(ActionId(16), Action{16, "sixteen"}));
  ASSERT_TRUE(builder.Erase(ActionId(2)));
  std::unique_ptr<const Table> table = std::move(builder).Build();

  const std::vector<ActionId> ids = {
      ActionId(1),           // valid
      kInvalidActionId,      // invalid (zero)
      ActionId(2),           // hole
      ActionId(16),          // last slot
      ActionId(17),          // out of range
      ActionId(5),           // valid
      ActionId(1u << 20),    // far out of range
  };

  for (size_t batch : {size_t{1}, size_t{8}, size_t{32}}) {
    std::vector<const Action *> results(ids.size(), nullptr);

    // Resolve the same sequence in batches of `batch`.
    const std::span<const ActionId> id_span(ids);
    const std::span<const Action *> result_span(results);
    for (size_t start = 0; start < ids.size(); start += batch) {
      const size_t count = std::min(batch, ids.size() - start);
      table->LookupBatch(id_span.subspan(start, count),
                         result_span.subspan(start, count));
    }

    for (size_t i = 0; i < ids.size(); i++) {
      EXPECT_EQ(table->Lookup(ids[i]), results[i])
          << "batch size " << batch << ", element " << i;
    }
  }
}

TEST(ObjectTableBatchTest, ShorterResultSpanIsNotOverrun) {
  ObjectTableBuilder<ActionId, Action> builder(4);
  ASSERT_TRUE(builder.Set(ActionId(1), Action{1, "one"}));
  std::unique_ptr<const Table> table = std::move(builder).Build();

  const std::array<ActionId, 4> ids = {ActionId(1), ActionId(1), ActionId(1),
                                       ActionId(1)};
  std::array<const Action *, 2> results = {nullptr, nullptr};
  table->LookupBatch(ids, results);
  EXPECT_NE(nullptr, results[0]);
  EXPECT_NE(nullptr, results[1]);
}

// -- Introspection -----------------------------------------------------------

TEST(ObjectTableTest, ReportsSizeCapacityAndStorage) {
  ObjectTableBuilder<ActionId, Action> builder(64);
  ASSERT_TRUE(builder.Set(ActionId(1), Action{1, "a"}));
  ASSERT_TRUE(builder.Set(ActionId(64), Action{64, "z"}));
  std::unique_ptr<const Table> table = std::move(builder).Build();

  EXPECT_EQ(64u, table->capacity());
  EXPECT_EQ(2u, table->size());
  EXPECT_GE(table->storage_bytes(), 64u * sizeof(Action));
}

}  // namespace
