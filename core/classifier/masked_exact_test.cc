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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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
#include <initializer_list>
#include <span>
#include <vector>

#include "classifier/classifier.h"
#include "classifier/masked_exact.h"

namespace {

using bess::classifier::Byte;
using bess::classifier::ClassifierErrorCode;
using bess::classifier::ConstBytes;
using bess::classifier::MaskedBackendInfo;
using bess::classifier::RuntimeMaskedBackend;
using bess::classifier::RuntimeMaskedRule;
using bess::classifier::WildcardBackendKind;

using Rule = RuntimeMaskedRule<uint32_t>;
using Backend = RuntimeMaskedBackend<uint32_t>;

// Rule values and masks are borrowed spans, so their storage must outlive the
// build. This owner makes that structural instead of a convention the tests
// have to remember: byte buffers live in `values_`/`masks_`, and each buffer's
// address is stable from the moment it is pushed (growing the outer vector
// moves the inner vector objects, not their heap buffers).
class RuleSet {
 public:
  RuleSet &Add(std::initializer_list<uint8_t> value,
               std::initializer_list<uint8_t> mask, int64_t priority,
               uint32_t result) {
    values_.push_back(Bytes(value));
    masks_.push_back(Bytes(mask));
    rules_.push_back(Rule{.value = ConstBytes(values_.back()),
                          .mask = ConstBytes(masks_.back()),
                          .priority = priority,
                          .result = result});
    return *this;
  }

  [[nodiscard]] std::span<const Rule> rules() const {
    return std::span<const Rule>(rules_);
  }

 private:
  static std::vector<Byte> Bytes(std::initializer_list<uint8_t> values) {
    std::vector<Byte> out;
    out.reserve(values.size());
    for (uint8_t value : values) {
      out.push_back(static_cast<Byte>(value));
    }
    return out;
  }

  std::vector<std::vector<Byte>> values_;
  std::vector<std::vector<Byte>> masks_;
  std::vector<Rule> rules_;
};

TEST(MaskedExactTest, SingleTupleExactMask) {
  // A full mask degenerates to exact matching.
  RuleSet rules;
  rules.Add({0x12, 0x34}, {0xff, 0xff}, 1, 100u);

  auto built = Backend::Build(2, rules.rules());
  ASSERT_TRUE(built.has_value());
  const Backend &backend = *built;
  EXPECT_EQ(1u, backend.tuple_count());
  EXPECT_EQ(1u, backend.rule_count());

  const std::array<std::byte, 4> keys = {Byte{0x12}, Byte{0x34}, Byte{0x12},
                                         Byte{0x35}};
  std::array<uint32_t, 2> results{};
  const uint64_t hits = backend.lookup_batch(ConstBytes(keys), 2, results);

  EXPECT_EQ(0x1ull, hits);
  EXPECT_EQ(100u, results[0]);
}

TEST(MaskedExactTest, WildcardBitsIgnorePacketBits) {
  // Mask covers the high nibble only: the low nibble is a wildcard.
  RuleSet rules;
  rules.Add({0x10}, {0xf0}, 1, 7u);

  auto built = Backend::Build(1, rules.rules());
  ASSERT_TRUE(built.has_value());
  const Backend &backend = *built;

  const std::array<std::byte, 4> keys = {Byte{0x1f}, Byte{0x00}, Byte{0x10},
                                         Byte{0xff}};
  std::array<uint32_t, 4> results = {0xdead, 0xbeef, 0xdead, 0xbeef};
  const uint64_t hits = backend.lookup_batch(ConstBytes(keys), 1, results);

  // 0x1f and 0x10 mask to 0x10; 0x00 masks to 0x00 and 0xff to 0xf0.
  EXPECT_EQ(0b0101ull, hits);
  EXPECT_EQ(7u, results[0]);
  EXPECT_EQ(7u, results[2]);
  EXPECT_EQ(0xbeefu, results[3]);
}

TEST(MaskedExactTest, RejectsNonCanonicalRule) {
  // value has bits set outside the mask.
  RuleSet rules;
  rules.Add({0xff}, {0xf0}, 1, 1u);

  auto built = Backend::Build(1, rules.rules());
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(ClassifierErrorCode::kInvalidPlan, built.error().code);
  ASSERT_TRUE(built.error().field_index.has_value());
  EXPECT_EQ(0u, *built.error().field_index);
}

TEST(MaskedExactTest, RejectsWrongRuleLength) {
  RuleSet rules;
  rules.Add({0x00, 0x00}, {0xff}, 1, 1u);

  auto built = Backend::Build(2, rules.rules());
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(ClassifierErrorCode::kInvalidPlan, built.error().code);
}

TEST(MaskedExactTest, RejectsEmptyAndOversizedKeys) {
  RuleSet rules;
  EXPECT_EQ(ClassifierErrorCode::kEmptyKey,
            Backend::Build(0, rules.rules()).error().code);
  EXPECT_EQ(ClassifierErrorCode::kKeyOutOfBounds,
            Backend::Build(65, rules.rules()).error().code);
}

TEST(MaskedExactTest, HigherPriorityWinsAcrossTuples) {
  // Two different masks both match 0x0f; the higher priority must win even
  // though the lower-priority tuple comes first.
  RuleSet rules;
  rules.Add({0x00}, {0xf0}, 1, 10u);
  rules.Add({0x0f}, {0xff}, 50, 20u);

  auto built = Backend::Build(1, rules.rules());
  ASSERT_TRUE(built.has_value());
  const Backend &backend = *built;
  EXPECT_EQ(2u, backend.tuple_count());

  const std::array<std::byte, 1> keys = {Byte{0x0f}};
  std::array<uint32_t, 1> results{};
  const uint64_t hits = backend.lookup_batch(ConstBytes(keys), 1, results);

  EXPECT_EQ(0x1ull, hits);
  EXPECT_EQ(20u, results[0]);
}

TEST(MaskedExactTest, EqualPriorityPrefersLaterRule) {
  // Same priority, two masks, both matching. The later rule's ordinal decides,
  // so swapping the input order must swap the winner: the outcome is not a
  // function of tuple iteration order.
  RuleSet wildcard_first;
  wildcard_first.Add({0x00}, {0xf0}, 5, 111u);
  wildcard_first.Add({0x0f}, {0xff}, 5, 222u);

  RuleSet exact_first;
  exact_first.Add({0x0f}, {0xff}, 5, 222u);
  exact_first.Add({0x00}, {0xf0}, 5, 111u);

  const std::array<std::byte, 1> keys = {Byte{0x0f}};

  auto later_wins = Backend::Build(1, wildcard_first.rules());
  ASSERT_TRUE(later_wins.has_value());
  EXPECT_EQ(2u, later_wins->tuple_count());
  std::array<uint32_t, 1> results{};
  EXPECT_EQ(0x1ull, later_wins->lookup_batch(ConstBytes(keys), 1, results));
  EXPECT_EQ(222u, results[0]);

  auto earlier_wins = Backend::Build(1, exact_first.rules());
  ASSERT_TRUE(earlier_wins.has_value());
  std::array<uint32_t, 1> swapped{};
  EXPECT_EQ(0x1ull, earlier_wins->lookup_batch(ConstBytes(keys), 1, swapped));
  EXPECT_EQ(111u, swapped[0]);
}

TEST(MaskedExactTest, DuplicateMaskAndValueKeepsBetterRank) {
  RuleSet rules;
  rules.Add({0x07}, {0xff}, 9, 1u);
  rules.Add({0x07}, {0xff}, 9, 2u);  // same rank, later ordinal wins

  auto built = Backend::Build(1, rules.rules());
  ASSERT_TRUE(built.has_value());
  EXPECT_EQ(1u, built->tuple_count());
  // Two rules submitted, one distinct (mask, value): the reported count is the
  // effective stored count.
  EXPECT_EQ(1u, built->rule_count());

  const std::array<std::byte, 1> keys = {Byte{0x07}};
  std::array<uint32_t, 1> results{};
  const uint64_t hits = built->lookup_batch(ConstBytes(keys), 1, results);
  EXPECT_EQ(0x1ull, hits);
  EXPECT_EQ(2u, results[0]);
}

TEST(MaskedExactTest, PaddedRowStrideIsAccepted) {
  // Rows may be wider than the logical key; only the leading key_size bytes are
  // examined.
  RuleSet rules;
  rules.Add({0x12}, {0xff}, 1, 5u);

  auto built = Backend::Build(1, rules.rules());
  ASSERT_TRUE(built.has_value());

  // Stride 4, key size 1: the padding bytes must not participate.
  const std::array<std::byte, 8> keys = {
      Byte{0x12}, Byte{0xaa}, Byte{0xbb}, Byte{0xcc},
      Byte{0x12}, Byte{0xdd}, Byte{0xee}, Byte{0xff},
  };
  std::array<uint32_t, 2> results{};
  const uint64_t hits = built->lookup_batch(ConstBytes(keys), 4, results);

  EXPECT_EQ(0x3ull, hits);
  EXPECT_EQ(5u, results[0]);
  EXPECT_EQ(5u, results[1]);
}

TEST(MaskedExactTest, AllMissLeavesResultsUntouched) {
  RuleSet rules;
  rules.Add({0xaa}, {0xff}, 1, 5u);

  auto built = Backend::Build(1, rules.rules());
  ASSERT_TRUE(built.has_value());

  const std::array<std::byte, 2> keys = {Byte{0x00}, Byte{0x01}};
  std::array<uint32_t, 2> results = {0xdead, 0xbeef};
  const uint64_t hits = built->lookup_batch(ConstBytes(keys), 1, results);

  EXPECT_EQ(0u, hits);
  EXPECT_EQ(0xdeadu, results[0]);
  EXPECT_EQ(0xbeefu, results[1]);
}

TEST(MaskedExactTest, NonPrefixMaskMatchesArbitraryBits) {
  // A mask that is not a prefix: bits 0 and 3 of a 4-byte key.
  RuleSet rules;
  rules.Add({0x01, 0x00, 0x00, 0x08}, {0x01, 0x00, 0x00, 0x08}, 1, 77u);

  auto built = Backend::Build(4, rules.rules());
  ASSERT_TRUE(built.has_value());

  const std::array<std::byte, 8> keys = {
      Byte{0x01}, Byte{0x00}, Byte{0x00}, Byte{0x08},  // hit
      Byte{0x01}, Byte{0x00}, Byte{0x00}, Byte{0x00},  // miss (bit 3 clear)
  };
  std::array<uint32_t, 2> results{};
  const uint64_t hits = built->lookup_batch(ConstBytes(keys), 4, results);

  EXPECT_EQ(0x1ull, hits);
  EXPECT_EQ(77u, results[0]);
}

TEST(MaskedExactTest, SupportsMoreThanEightTuples) {
  // The module's eight-mask ceiling is wire compatibility, not substrate
  // policy: twelve distinct masks must all be usable.
  RuleSet rules;
  for (uint8_t i = 0; i < 12; i++) {
    rules.Add({0x00}, {static_cast<uint8_t>(i + 1)}, 1, 100u + i);
  }

  auto built = Backend::Build(1, rules.rules());
  ASSERT_TRUE(built.has_value());
  EXPECT_EQ(12u, built->tuple_count());
}

TEST(MaskedExactTest, InfoReportsTupleSpaceMetrics) {
  RuleSet rules;
  rules.Add({0x01}, {0xff}, 1, 1u);
  rules.Add({0x02}, {0xff}, 1, 2u);

  auto built = Backend::Build(1, rules.rules());
  ASSERT_TRUE(built.has_value());
  const MaskedBackendInfo info = built->info();

  EXPECT_EQ(WildcardBackendKind::kTupleSpace, info.kind);
  EXPECT_EQ(1u, info.tuple_count);
  EXPECT_EQ(2u, info.rule_count);
  EXPECT_EQ(1u, info.key_size);
  EXPECT_EQ(sizeof(uint32_t), info.result_size);
}

TEST(MaskedExactTest, WideKeysUseTheGenericMaskPath) {
  // 16-byte keys take the byte-wise masking kernel.
  std::vector<Byte> value(16, Byte{0});
  std::vector<Byte> mask(16, Byte{0xff});
  mask[0] = Byte{0xf0};
  const std::array<Rule, 1> rules = {
      Rule{.value = ConstBytes(value),
           .mask = ConstBytes(mask),
           .priority = 1,
           .result = 9u}};

  auto built = Backend::Build(16, rules);
  ASSERT_TRUE(built.has_value());

  std::array<std::byte, 32> keys{};
  keys[0] = Byte{0x0f};   // hit: high nibble matches, low nibble wildcard
  keys[16] = Byte{0x10};  // miss: masked high nibble differs
  std::array<uint32_t, 2> results{};
  const uint64_t hits = built->lookup_batch(ConstBytes(keys), 16, results);

  EXPECT_EQ(0x1ull, hits);
  EXPECT_EQ(9u, results[0]);
}


}  // namespace
