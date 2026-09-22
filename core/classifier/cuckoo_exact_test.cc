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
#include <cstdint>
#include <optional>

#include "classifier/byte_key.h"
#include "classifier/classifier.h"
#include "classifier/cuckoo_exact.h"
#include "classifier/typed_exact.h"

// ---------------------------------------------------------------------------
// FlowKey — a simple typed key used across classifier backend tests.
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

// Register FlowKey with the classifier key infrastructure.
namespace bess::classifier {

template <>
struct KeyTraits<FlowKey> {
  static constexpr bool canonical_representation = false;
  using hash_type = FlowKeyHash;
  using equal_type = FlowKeyEqual;
};

}  // namespace bess::classifier

namespace {

using bess::classifier::BatchExactBackend;
using bess::classifier::CuckooExactBackend;
using bess::classifier::ExactBackendKind;
using bess::classifier::ExactTable;
using bess::classifier::MeasurableBackend;
using bess::classifier::ScalarExactBackend;

// ---------------------------------------------------------------------------
// Concept assertions — checked at compile time, no runtime cost.
// ---------------------------------------------------------------------------

using Backend = CuckooExactBackend<FlowKey, uint32_t>;

static_assert(ScalarExactBackend<Backend, FlowKey>,
              "CuckooExactBackend must satisfy ScalarExactBackend");

static_assert(MeasurableBackend<Backend>,
              "CuckooExactBackend must satisfy MeasurableBackend");

static_assert(!BatchExactBackend<Backend, FlowKey>,
              "CuckooExactBackend must NOT satisfy BatchExactBackend: "
              "it has no native bulk lookup; ExactTable uses scalar fallback");

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(CuckooExactBackend, InsertAndLookup) {
  Backend backend;

  EXPECT_EQ(0u, backend.size());

  // Insert a known entry.
  ASSERT_TRUE(backend.insert(FlowKey{7, 80}, uint32_t{42}));
  EXPECT_EQ(1u, backend.size());

  // Hit — returns non-null pointer with the correct value.
  const uint32_t *hit = backend.lookup(FlowKey{7, 80});
  ASSERT_NE(nullptr, hit);
  EXPECT_EQ(42u, *hit);

  // Miss — key not present.
  EXPECT_EQ(nullptr, backend.lookup(FlowKey{8, 80}));
  EXPECT_EQ(nullptr, backend.lookup(FlowKey{7, 81}));

  // Remove the entry; subsequent lookup must return nullptr.
  EXPECT_TRUE(backend.remove(FlowKey{7, 80}));
  EXPECT_EQ(nullptr, backend.lookup(FlowKey{7, 80}));
  EXPECT_EQ(0u, backend.size());

  // Removing an absent key returns false.
  EXPECT_FALSE(backend.remove(FlowKey{7, 80}));
}

TEST(CuckooExactBackend, BackendInfo) {
  Backend backend;

  {
    const auto info = backend.info();
    EXPECT_EQ(ExactBackendKind::kCuckoo, info.kind);
    EXPECT_EQ(0u, info.rule_count);
    EXPECT_EQ(sizeof(FlowKey), info.key_size);
    EXPECT_EQ(sizeof(uint32_t), info.result_size);
  }

  ASSERT_TRUE(backend.insert(FlowKey{1, 1}, 100u));
  EXPECT_EQ(1u, backend.info().rule_count);

  ASSERT_TRUE(backend.insert(FlowKey{2, 2}, 200u));
  EXPECT_EQ(2u, backend.info().rule_count);
}

TEST(CuckooExactBackend, ScalarExactBackendConcept) {
  // Validated by the static_assert above; this test documents the requirement
  // and ensures the static_assert is not dead-code-eliminated.
  static_assert(ScalarExactBackend<Backend, FlowKey>);
}

TEST(CuckooExactBackend, MeasurableBackendConcept) {
  static_assert(MeasurableBackend<Backend>);
}

TEST(CuckooExactBackend, NotBatchExactBackend) {
  static_assert(!BatchExactBackend<Backend, FlowKey>);
}

// ExactTable with CuckooExactBackend exercises the scalar-fallback path in
// lookup_batch (since CuckooExactBackend does not satisfy BatchExactBackend).
TEST(CuckooExactBackend, UsedInExactTable) {
  using CuckooBackend = CuckooExactBackend<FlowKey, uint32_t>;
  using Table = ExactTable<FlowKey, uint32_t, CuckooBackend>;

  CuckooBackend backend;
  ASSERT_TRUE(backend.insert(FlowKey{7, 80}, uint32_t{99}));

  Table table(std::move(backend));
  EXPECT_EQ(1u, table.size());

  // Two keys: one hit, one miss.
  const std::array<FlowKey, 2> keys = {FlowKey{7, 80}, FlowKey{9, 99}};
  // lookup_result is const uint32_t* (pointer variant — CuckooExactBackend
  // returns const Result*).
  using LR = Table::lookup_result;
  std::array<LR, 2> results{};

  const uint64_t hits = table.lookup_batch(keys, results);

  // Bit 0 set: key[0] was a hit.
  EXPECT_NE(0u, hits & (uint64_t{1} << 0));
  ASSERT_NE(nullptr, results[0]);
  EXPECT_EQ(99u, *results[0]);

  // Bit 1 clear: key[1] was a miss.
  EXPECT_EQ(0u, hits & (uint64_t{1} << 1));
  EXPECT_EQ(nullptr, results[1]);
}

TEST(CuckooExactBackend, BuildRuntimeCuckooBackendPopulatesAndDispatches) {
  using gate_idx_t = uint16_t;
  using bess::classifier::Byte;
  using bess::classifier::ConstBytes;
  using bess::classifier::RuntimeExactRule;
  using bess::classifier::BuildRuntimeCuckooBackend;

  const std::array<Byte, 4> k1 = {Byte{1}, Byte{2}, Byte{3}, Byte{4}};
  const std::array<Byte, 4> k2 = {Byte{5}, Byte{6}, Byte{7}, Byte{8}};
  const std::array<Byte, 4> miss = {Byte{9}, Byte{9}, Byte{9}, Byte{9}};

  const std::array<RuntimeExactRule<gate_idx_t>, 2> rules = {
      RuntimeExactRule<gate_idx_t>{.key = ConstBytes(k1), .result = 7},
      RuntimeExactRule<gate_idx_t>{.key = ConstBytes(k2), .result = 42},
  };

  auto backend_res = BuildRuntimeCuckooBackend<gate_idx_t>(4, rules);
  ASSERT_TRUE(backend_res.has_value());
  auto backend = std::move(*backend_res);
  ASSERT_TRUE(backend);
  EXPECT_EQ(2u, backend.info().rule_count);
  EXPECT_EQ(4u, backend.info().key_size);
  EXPECT_EQ(0u, backend.info().storage_bytes);  // storage_bytes truthful / not misleading

  // Packed keys input: k1, miss, k2 (stride = 4)
  std::array<Byte, 3 * 4> packed_keys{};
  std::memcpy(packed_keys.data(), k1.data(), 4);
  std::memcpy(packed_keys.data() + 4, miss.data(), 4);
  std::memcpy(packed_keys.data() + 8, k2.data(), 4);

  std::array<gate_idx_t, 3> results{};
  uint64_t hits = backend.lookup_batch(ConstBytes(packed_keys), 4, results);

  // Bits 0 and 2 set -> 0b101 = 5
  EXPECT_EQ(0x5ull, hits);
  EXPECT_EQ(7u, results[0]);
  EXPECT_EQ(42u, results[2]);

  // Rule key size mismatch returns error
  const std::array<Byte, 3> bad_key = {Byte{1}, Byte{2}, Byte{3}};
  const std::array<RuntimeExactRule<gate_idx_t>, 1> bad_rules = {
      RuntimeExactRule<gate_idx_t>{.key = ConstBytes(bad_key), .result = 99},
  };
  auto bad_res = BuildRuntimeCuckooBackend<gate_idx_t>(4, bad_rules);
  EXPECT_FALSE(bad_res.has_value());
}

TEST(CuckooExactBackend, BuildRuntimeCuckooBackendRejectsDuplicateKeys) {
  using gate_idx_t = uint16_t;
  using bess::classifier::Byte;
  using bess::classifier::ConstBytes;
  using bess::classifier::RuntimeExactRule;
  using bess::classifier::BuildRuntimeCuckooBackend;

  const std::array<Byte, 4> k1 = {Byte{1}, Byte{2}, Byte{3}, Byte{4}};

  const std::array<RuntimeExactRule<gate_idx_t>, 2> rules = {
      RuntimeExactRule<gate_idx_t>{.key = ConstBytes(k1), .result = 7},
      RuntimeExactRule<gate_idx_t>{.key = ConstBytes(k1), .result = 42},
  };
  auto dup_res = BuildRuntimeCuckooBackend<gate_idx_t>(4, rules);
  EXPECT_FALSE(dup_res.has_value());
}

TEST(CuckooExactBackend, RuntimeCuckooKeyHasNoPerEntrySize) {
  // The logical key length lives in the state/functors, not in every entry:
  // an 8-byte logical key occupies an 8-byte, naturally aligned entry.
  static_assert(sizeof(bess::classifier::detail::RuntimeCuckooKey<8>) == 8);
  static_assert(sizeof(bess::classifier::detail::RuntimeCuckooKey<16>) == 16);
  static_assert(alignof(bess::classifier::detail::RuntimeCuckooKey<8>) >=
                alignof(uint64_t));
}

TEST(CuckooExactBackend, StatefulFunctorsBoundToLogicalSize) {
  using bess::classifier::detail::RuntimeCuckooEqual;
  using bess::classifier::detail::RuntimeCuckooHash;
  using bess::classifier::detail::RuntimeCuckooKey;

  RuntimeCuckooKey<8> a{};
  RuntimeCuckooKey<8> b{};
  for (size_t i = 0; i < 8; i++) {
    a.bytes[i] = b.bytes[i] = static_cast<std::byte>(i + 1);
  }
  // Trailing garbage beyond the logical size must not affect equality.
  RuntimeCuckooEqual<8> eq4{4};
  EXPECT_TRUE(eq4(a, b));

  // ... but logical bytes do.
  b.bytes[3] = static_cast<std::byte>(0xFF);
  EXPECT_FALSE(eq4(a, b));
  b.bytes[3] = static_cast<std::byte>(4);

  // Hash covers exactly the logical bytes.
  RuntimeCuckooHash<8> h4{4};
  RuntimeCuckooHash<8> h8{8};
  EXPECT_EQ(h4(a), h4(b));
  b.bytes[7] = static_cast<std::byte>(0xFF);
  EXPECT_EQ(h4(a), h4(b));
  EXPECT_NE(h8(a), h8(b));
}

}  // namespace
