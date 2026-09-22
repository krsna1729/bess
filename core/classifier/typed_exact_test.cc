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
  table.lookup_batch(keys, results);
  EXPECT_EQ(42u, *results[0]);
  EXPECT_FALSE(results[1].has_value());
  EXPECT_EQ(1u, table.size());
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

}  // namespace
