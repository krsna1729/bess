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
// * Neither the names of the copyright holders nor their contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>

#include "classifier/byte_key.h"
#include "classifier/cuckoo_exact.h"
#include "classifier/direct_exact.h"
#include "classifier/small_exact.h"
#include "classifier/typed_exact.h"

struct FlowKey {
  uint32_t tenant;
  uint16_t port;
};

struct FlowKeyHash {
  size_t operator()(const FlowKey &key) const noexcept {
    return (static_cast<size_t>(key.tenant) << 16) ^ key.port;
  }
};

struct FlowKeyEqual {
  bool operator()(const FlowKey &lhs, const FlowKey &rhs) const noexcept {
    return lhs.tenant == rhs.tenant && lhs.port == rhs.port;
  }
};

namespace bess::classifier {

template <>
struct KeyTraits<FlowKey> {
  static constexpr bool canonical_representation = false;
  using hash_type = FlowKeyHash;
  using equal_type = FlowKeyEqual;
};

}  // namespace bess::classifier

namespace {

using bess::classifier::ByteKey;
using bess::classifier::CanonicalByteKey;
using bess::classifier::ClassifierKey;
using bess::classifier::ExactTable;
using bess::classifier::InlineResult;
using bess::classifier::NativeToBigEndian;
using bess::classifier::ReferencedResult;
using bess::classifier::ResultSlot;
using bess::classifier::TypedClassifierKey;

struct PaddedKey {
  uint8_t tag;
  uint32_t value;
};

static_assert(CanonicalByteKey<ByteKey<16>>);
static_assert(TypedClassifierKey<FlowKey>);
static_assert(ClassifierKey<FlowKey>);
static_assert(!ClassifierKey<PaddedKey>);
static_assert(InlineResult<uint32_t>);
static_assert(!ReferencedResult<uint32_t>);
static_assert(!std::is_same_v<ResultSlot, bess::dataplane::ActionId>);

// The typed contract is the stateless, non-throwing model the backends actually
// instantiate: Hash{} and Equal{} called from noexcept packet-path methods.
// These assertions are the constraint, not a stylistic preference.
struct StatefulHash {
  size_t seed;

  explicit StatefulHash(size_t value) : seed(value) {}
  size_t operator()(const FlowKey &) const noexcept { return seed; }
};

struct ThrowingEqual {
  bool operator()(const FlowKey &, const FlowKey &) const { return true; }
};

struct StatefulEqual {
  size_t unused;

  explicit StatefulEqual(size_t value) : unused(value) {}
  bool operator()(const FlowKey &, const FlowKey &) const noexcept {
    return true;
  }
};

static_assert(bess::classifier::TypedKeyOperations<FlowKey, FlowKeyHash,
                                                   FlowKeyEqual>);
static_assert(!bess::classifier::TypedKeyOperations<FlowKey, StatefulHash,
                                                    FlowKeyEqual>);
static_assert(!bess::classifier::TypedKeyOperations<FlowKey, FlowKeyHash,
                                                    ThrowingEqual>);
static_assert(!bess::classifier::TypedKeyEquality<FlowKey, StatefulEqual>);

// TypedKeyEqual is the no-registration fallback and requires the author's own
// operator==, which FlowKey (registered through KeyTraits) does not define.
struct ComparableKey {
  uint32_t value;

  friend bool operator==(const ComparableKey &, const ComparableKey &) = default;
};

static_assert(bess::classifier::TypedKeyEquality<
              ComparableKey, bess::classifier::TypedKeyEqual<ComparableKey>>);

TEST(TypedExactTest, DefaultEqualityUsesAuthorOperator) {
  const bess::classifier::TypedKeyEqual<ComparableKey> equal;
  EXPECT_TRUE(equal(ComparableKey{7}, ComparableKey{7}));
  EXPECT_FALSE(equal(ComparableKey{7}, ComparableKey{8}));
}

struct FakeBackend {
  uint32_t result = 0;

  std::optional<uint32_t> lookup(const FlowKey &key) const noexcept {
    if (key.tenant != 7 || key.port != 80) {
      return std::nullopt;
    }
    return result;
  }

  size_t size() const noexcept { return 1; }
};

using FlowTable = ExactTable<FlowKey, uint32_t, FakeBackend>;
static_assert(sizeof(FlowTable) == sizeof(FakeBackend));

struct MoveOnlyBackend {
  explicit MoveOnlyBackend(uint32_t value)
      : result(std::make_unique<uint32_t>(value)) {}

  MoveOnlyBackend(MoveOnlyBackend &&) noexcept = default;
  MoveOnlyBackend &operator=(MoveOnlyBackend &&) noexcept = default;
  MoveOnlyBackend(const MoveOnlyBackend &) = delete;
  MoveOnlyBackend &operator=(const MoveOnlyBackend &) = delete;

  const uint32_t *lookup(const FlowKey &key) const noexcept {
    return key.tenant == 7 ? result.get() : nullptr;
  }

  size_t size() const noexcept { return 1; }

  std::unique_ptr<uint32_t> result;
};

using MoveOnlyFlowTable = ExactTable<FlowKey, uint32_t, MoveOnlyBackend>;

struct SlotBackend {
  ResultSlot slot = ResultSlot(9);

  const ResultSlot *lookup(const ByteKey<4> &) const noexcept { return &slot; }
  size_t size() const noexcept { return 1; }
};

using SlotTable = ExactTable<ByteKey<4>, ResultSlot, SlotBackend>;

struct EmptyBackend {
  static constexpr uint32_t kValue = 1;

  const uint32_t *lookup(const ByteKey<4> &) const noexcept { return &kValue; }
  size_t size() const noexcept { return 0; }
};

using EmptyTable = ExactTable<ByteKey<4>, uint32_t, EmptyBackend>;
static_assert(sizeof(EmptyTable) == sizeof(EmptyBackend));
static_assert(!std::is_polymorphic_v<FlowTable>);

TEST(TypedExactTest, UsesTypedKeyAndResultWithoutMetadata) {
  FlowTable table(FakeBackend{42});

  ASSERT_TRUE(table.lookup(FlowKey{7, 80}).has_value());
  EXPECT_EQ(42u, *table.lookup(FlowKey{7, 80}));
  EXPECT_FALSE(table.lookup(FlowKey{8, 80}).has_value());

  const std::array<FlowKey, 2> keys = {FlowKey{7, 80}, FlowKey{8, 80}};
  std::array<std::optional<uint32_t>, 2> results;
  const uint64_t hits = table.lookup_batch(keys, results);
  EXPECT_EQ(0x1ull, hits);
  EXPECT_EQ(42u, *results[0]);
  EXPECT_FALSE(results[1].has_value());
  EXPECT_EQ(1u, table.size());

  // Overload 2: direct Result output with hit mask
  std::array<uint32_t, 2> direct_results{};
  const uint64_t direct_hits = table.lookup_batch(keys, direct_results);
  EXPECT_EQ(0x1ull, direct_hits);
  EXPECT_EQ(42u, direct_results[0]);
}
TEST(TypedExactTest, SupportsMoveOnlyBackendsAndStrongSlots) {
  MoveOnlyFlowTable move_only(MoveOnlyBackend(17));
  ASSERT_NE(nullptr, move_only.lookup(FlowKey{7, 80}));
  EXPECT_EQ(17u, *move_only.lookup(FlowKey{7, 80}));

  SlotTable slots(SlotBackend{});
  const ResultSlot *slot = slots.lookup(ByteKey<4>{});
  ASSERT_NE(nullptr, slot);
  EXPECT_EQ(ResultSlot(9), *slot);
}

TEST(TypedExactTest, ModernRepresentationHelpersRoundTrip) {
  constexpr uint32_t value = 0x10203040u;
  EXPECT_EQ(value, NativeToBigEndian(NativeToBigEndian(value)));

  const auto bytes = bess::classifier::BitCast<std::array<std::byte, 4>>(value);
  EXPECT_EQ(value, bess::classifier::BitCast<uint32_t>(bytes));
}

// A backend with a native batch path must have that path reach the caller
// unchanged: ExactTable must not re-derive hits through scalar lookups.
struct NativeBatchBackend {
  mutable size_t batch_calls = 0;

  const uint32_t *lookup(const FlowKey &) const noexcept { return nullptr; }

  uint64_t lookup_batch(std::span<const FlowKey> keys,
                        std::span<uint32_t> results) const noexcept {
    batch_calls++;
    for (size_t i = 0; i < results.size(); i++) {
      results[i] = 0xABCD0000u + static_cast<uint32_t>(i);
    }
    return keys.size() >= 2 ? 0b101ull : 0ull;
  }

  size_t size() const noexcept { return 3; }
};

TEST(TypedExactTest, NativeBatchPathReachesCallerUnchanged) {
  using Table = ExactTable<FlowKey, uint32_t, NativeBatchBackend>;
  Table table(NativeBatchBackend{});

  const std::array<FlowKey, 3> keys = {
      FlowKey{1, 1}, FlowKey{2, 2}, FlowKey{3, 3}};
  std::array<uint32_t, 3> results{};
  const uint64_t hits = table.lookup_batch(keys, results);

  EXPECT_EQ(0b101ull, hits);
  EXPECT_EQ(0xABCD0000u, results[0]);
  EXPECT_EQ(0xABCD0002u, results[2]);
  EXPECT_EQ(1u, table.backend().batch_calls);
}

// A 32-byte decision: too large for InlineResult, and semantically unrelated to
// ResultSlot. Typed authors must be able to return it directly.
struct LargeDecision {
  uint32_t words[8] = {};
};

static_assert(!InlineResult<LargeDecision>);
static_assert(!std::is_same_v<LargeDecision, ResultSlot>);
static_assert(sizeof(LargeDecision) == 32);

TEST(TypedExactTest, LargeTypedResultNeedsNoIndirection) {
  using Backend =
      bess::classifier::CuckooExactBackend<FlowKey, LargeDecision,
                                           FlowKeyHash, FlowKeyEqual>;
  using Table = ExactTable<FlowKey, LargeDecision, Backend>;

  Backend backend;
  LargeDecision decision;
  decision.words[0] = 7;
  decision.words[7] = 11;
  ASSERT_TRUE(backend.insert(FlowKey{1, 2}, decision));

  Table table(std::move(backend));
  const std::array<FlowKey, 2> keys = {FlowKey{1, 2}, FlowKey{3, 4}};
  std::array<LargeDecision, 2> results{};
  const uint64_t hits = table.lookup_batch(keys, results);

  EXPECT_EQ(0x1ull, hits);
  EXPECT_EQ(7u, results[0].words[0]);
  EXPECT_EQ(11u, results[0].words[7]);
}

// The typed backends must agree on hits and results for identical rules and
// keys; backend choice is an author decision, not a semantic one.
template <typename Table>
std::array<uint32_t, 6> LookupProbe(Table &table) {
  const std::array<FlowKey, 6> keys = {
      FlowKey{0, 0}, FlowKey{1, 1}, FlowKey{2, 2},
      FlowKey{7, 7}, FlowKey{8, 8}, FlowKey{5, 5}};
  std::array<uint32_t, 6> results{};
  const uint64_t hits = table.lookup_batch(keys, results);
  results[0] = static_cast<uint32_t>(hits);
  return results;
}

TEST(TypedExactTest, CuckooAndSmallAgreeOnSameRules) {
  using CuckooBackend =
      bess::classifier::CuckooExactBackend<FlowKey, uint32_t, FlowKeyHash,
                                           FlowKeyEqual>;
  using SmallBackend =
      bess::classifier::SmallExactBackend<FlowKey, uint32_t, FlowKeyEqual>;

  CuckooBackend cuckoo;
  SmallBackend small;
  for (uint32_t i = 0; i < 5; i++) {
    const FlowKey key{i, static_cast<uint16_t>(i)};
    ASSERT_TRUE(cuckoo.insert(key, i * 10u));
    ASSERT_TRUE(small.insert(key, i * 10u));
  }
  ASSERT_TRUE(cuckoo.remove(FlowKey{2, 2}));
  ASSERT_TRUE(small.remove(FlowKey{2, 2}));

  ExactTable<FlowKey, uint32_t, CuckooBackend> cuckoo_table(std::move(cuckoo));
  ExactTable<FlowKey, uint32_t, SmallBackend> small_table(std::move(small));

  EXPECT_EQ(LookupProbe(cuckoo_table), LookupProbe(small_table));
}

TEST(TypedExactTest, SmallAndDirectAgreeOnBoundedDomain) {
  using Key = uint16_t;
  using SmallBackend = bess::classifier::SmallExactBackend<Key, uint32_t>;
  using DirectBackend = bess::classifier::DirectExactBackend<Key, uint32_t>;

  SmallBackend small;
  DirectBackend direct;
  for (uint32_t i = 0; i < 16; i++) {
    const uint16_t key = static_cast<uint16_t>(i * 2);
    ASSERT_TRUE(small.insert(key, i));
    ASSERT_TRUE(direct.insert(key, i));
  }

  ExactTable<Key, uint32_t, SmallBackend> small_table(std::move(small));
  ExactTable<Key, uint32_t, DirectBackend> direct_table(std::move(direct));

  const std::array<Key, 6> keys = {0, 1, 2, 15, 30, 31};
  std::array<uint32_t, 6> small_results{};
  std::array<uint32_t, 6> direct_results{};
  const uint64_t small_hits = small_table.lookup_batch(keys, small_results);
  const uint64_t direct_hits = direct_table.lookup_batch(keys, direct_results);

  EXPECT_EQ(small_hits, direct_hits);
  EXPECT_EQ(small_results, direct_results);
  EXPECT_EQ(0x15ull, small_hits);  // keys 0, 2, 30 hit; 1, 15, 31 miss
}

}  // namespace
