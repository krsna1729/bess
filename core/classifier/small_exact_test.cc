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

#include "classifier/backend.h"
#include "classifier/byte_key.h"
#include "classifier/small_exact.h"
#include "classifier/typed_exact.h"

// ---------------------------------------------------------------------------
// Test key type matching typed_exact_test.cc conventions
// ---------------------------------------------------------------------------

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

struct NaturalKey {
  uint32_t value;

  friend bool operator==(const NaturalKey &, const NaturalKey &) = default;
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

using bess::classifier::BackendInfo;
using bess::classifier::BatchExactBackend;
using bess::classifier::ExactBackendKind;
using bess::classifier::MeasurableBackend;
using bess::classifier::SmallExactBackend;
using bess::classifier::ScalarExactBackend;
using bess::classifier::SortedFlatBackend;

// ---------------------------------------------------------------------------
// Concept assertions
// ---------------------------------------------------------------------------

static_assert(BatchExactBackend<SmallExactBackend<FlowKey, uint32_t>, FlowKey,
                                uint32_t>,
              "SmallExactBackend must satisfy BatchExactBackend");
static_assert(MeasurableBackend<SmallExactBackend<FlowKey, uint32_t>>,
              "SmallExactBackend must satisfy MeasurableBackend");
static_assert(ScalarExactBackend<SmallExactBackend<NaturalKey, uint32_t>,
                                 NaturalKey>);

TEST(SmallExact, AcceptsNaturalKeyWithoutTraits) {
  SmallExactBackend<NaturalKey, uint32_t> backend;
  ASSERT_TRUE(backend.insert(NaturalKey{7}, 42u));
  const uint32_t *result = backend.lookup(NaturalKey{7});
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(42u, *result);
}

// ---------------------------------------------------------------------------
// SmallExactBackend tests
// ---------------------------------------------------------------------------

TEST(SmallExact, InsertLookupRemove) {
  SmallExactBackend<FlowKey, uint32_t> backend;

  ASSERT_TRUE(backend.insert({.tenant = 1, .port = 80}, 100u));
  ASSERT_TRUE(backend.insert({.tenant = 2, .port = 443}, 200u));
  ASSERT_TRUE(backend.insert({.tenant = 3, .port = 8080}, 300u));
  EXPECT_EQ(backend.size(), 3u);

  const uint32_t *r1 = backend.lookup({.tenant = 1, .port = 80});
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(*r1, 100u);

  const uint32_t *r2 = backend.lookup({.tenant = 2, .port = 443});
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(*r2, 200u);

  const uint32_t *r3 = backend.lookup({.tenant = 3, .port = 8080});
  ASSERT_NE(r3, nullptr);
  EXPECT_EQ(*r3, 300u);

  // Remove middle entry; miss after removal.
  EXPECT_TRUE(backend.remove({.tenant = 2, .port = 443}));
  EXPECT_EQ(backend.size(), 2u);
  EXPECT_EQ(backend.lookup({.tenant = 2, .port = 443}), nullptr);

  // Others still reachable.
  EXPECT_NE(backend.lookup({.tenant = 1, .port = 80}), nullptr);
  EXPECT_NE(backend.lookup({.tenant = 3, .port = 8080}), nullptr);

  // Remove non-existent returns false.
  EXPECT_FALSE(backend.remove({.tenant = 99, .port = 0}));
}

TEST(SmallExact, LookupBatchHitMask) {
  SmallExactBackend<FlowKey, uint32_t> backend;
  backend.insert({.tenant = 10, .port = 1}, 10u);
  backend.insert({.tenant = 20, .port = 2}, 20u);
  backend.insert({.tenant = 30, .port = 3}, 30u);

  // Keys: hit, miss, hit, miss
  const std::array<FlowKey, 4> keys = {{
      {.tenant = 10, .port = 1},   // hit → bit 0
      {.tenant = 99, .port = 99},  // miss
      {.tenant = 20, .port = 2},   // hit → bit 2
      {.tenant = 0,  .port = 0},   // miss
  }};
  std::array<uint32_t, 4> results{};

  uint64_t mask = backend.lookup_batch(keys, results);

  // Bits 0 and 2 set; 1 and 3 clear.
  EXPECT_EQ(mask, 0b0101u);
  EXPECT_EQ(results[0], 10u);
  EXPECT_EQ(results[2], 20u);
}

TEST(SmallExact, OverwriteExisting) {
  SmallExactBackend<FlowKey, uint32_t> backend;
  ASSERT_TRUE(backend.insert({.tenant = 1, .port = 1}, 42u));
  EXPECT_EQ(backend.size(), 1u);

  // Second insert same key → overwrite, size stays 1.
  ASSERT_TRUE(backend.insert({.tenant = 1, .port = 1}, 99u));
  EXPECT_EQ(backend.size(), 1u);

  const uint32_t *r = backend.lookup({.tenant = 1, .port = 1});
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(*r, 99u);
}

TEST(SmallExact, MaxRulesEnforced) {
  SmallExactBackend<FlowKey, uint32_t> backend;

  // Fill exactly kMaxRules entries.
  for (uint32_t i = 0; i < SmallExactBackend<FlowKey, uint32_t>::kMaxRules; ++i) {
    ASSERT_TRUE(backend.insert({.tenant = i, .port = static_cast<uint16_t>(i)},
                               i * 10u))
        << "insert failed at i=" << i;
  }
  EXPECT_EQ(backend.size(), (SmallExactBackend<FlowKey, uint32_t>::kMaxRules));

  // One more unique key must be rejected.
  EXPECT_FALSE(backend.insert(
      {.tenant = 9999, .port = 9999}, 0u));

  // Overwriting an existing key at capacity is still allowed.
  EXPECT_TRUE(backend.insert({.tenant = 0, .port = 0}, 777u));
  EXPECT_EQ(backend.size(), (SmallExactBackend<FlowKey, uint32_t>::kMaxRules));
}

// ---------------------------------------------------------------------------
// SortedFlatBackend tests
// ---------------------------------------------------------------------------

// Provide a total order for FlowKey so SortedFlatBackend can binary-search.
struct FlowKeyCompare {
  bool operator()(const FlowKey &lhs, const FlowKey &rhs) const noexcept {
    if (lhs.tenant != rhs.tenant) return lhs.tenant < rhs.tenant;
    return lhs.port < rhs.port;
  }
};

using SortedFlat =
    SortedFlatBackend<FlowKey, uint32_t, FlowKeyCompare, FlowKeyEqual>;

TEST(SortedFlat, InsertAndBinarySearch) {
  SortedFlat backend;

  // Insert 10 entries in reverse order to verify sorted storage.
  for (uint32_t i = 10; i >= 1; --i) {
    backend.insert({.tenant = i, .port = static_cast<uint16_t>(i)}, i * 7u);
  }
  EXPECT_EQ(backend.size(), 10u);

  // Every inserted key must be found with correct value.
  for (uint32_t i = 1; i <= 10; ++i) {
    const uint32_t *r =
        backend.lookup({.tenant = i, .port = static_cast<uint16_t>(i)});
    ASSERT_NE(r, nullptr) << "key " << i << " not found";
    EXPECT_EQ(*r, i * 7u);
  }

  // Key not inserted must miss.
  EXPECT_EQ(backend.lookup({.tenant = 99, .port = 99}), nullptr);

  // Remove one; miss afterward; others intact.
  EXPECT_TRUE(backend.remove({.tenant = 5, .port = 5}));
  EXPECT_EQ(backend.size(), 9u);
  EXPECT_EQ(backend.lookup({.tenant = 5, .port = 5}), nullptr);
  EXPECT_NE(backend.lookup({.tenant = 4, .port = 4}), nullptr);
  EXPECT_NE(backend.lookup({.tenant = 6, .port = 6}), nullptr);

  // Verify BackendInfo reports kSmall kind.
  BackendInfo info = backend.info();
  EXPECT_EQ(info.kind, ExactBackendKind::kSmall);
  EXPECT_EQ(info.rule_count, 9u);
}

}  // namespace
