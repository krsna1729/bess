// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
//   contributors may be used to endorse or promote products derived from this
//   software without specific prior written permission.
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
#include <cstring>
#include <vector>

#include "classifier/classifier.h"
#include "classifier/packed_value_store.h"
#include "classifier/rte_hash_exact.h"
#include "classifier/typed_exact.h"

namespace {

using bess::classifier::BackendInfo;
using bess::classifier::BatchExactBackend;
using bess::classifier::Byte;
using bess::classifier::ConstBytes;
using bess::classifier::ExactBackendKind;
using bess::classifier::MeasurableBackend;
using bess::classifier::PackedValueStore;
using bess::classifier::ResultSlot;
using bess::classifier::RteHashDataBackend;
using bess::classifier::RteHashPositionBackend;
using bess::classifier::ScalarExactBackend;

// ---------------------------------------------------------------------------
// Concept assertions
// ---------------------------------------------------------------------------

static_assert(ScalarExactBackend<RteHashPositionBackend, ConstBytes>);
static_assert(BatchExactBackend<RteHashPositionBackend, ConstBytes, int32_t>);
static_assert(MeasurableBackend<RteHashPositionBackend>);

static_assert(ScalarExactBackend<RteHashDataBackend<uint16_t>, ConstBytes>);
static_assert(BatchExactBackend<RteHashDataBackend<uint16_t>, ConstBytes,
                                uint16_t>);
static_assert(MeasurableBackend<RteHashDataBackend<uint16_t>>);

static_assert(ScalarExactBackend<RteHashDataBackend<uint32_t>, ConstBytes>);
static_assert(BatchExactBackend<RteHashDataBackend<uint32_t>, ConstBytes,
                                uint32_t>);
static_assert(MeasurableBackend<RteHashDataBackend<uint32_t>>);

// ---------------------------------------------------------------------------
// Position Backend Tests
// ---------------------------------------------------------------------------

TEST(RteHashPositionBackendTest, AddAndLookupScalar) {
  RteHashPositionBackend backend(4, 64);
  ASSERT_TRUE(backend.valid());

  const std::array<Byte, 4> key1 = {Byte{1}, Byte{2}, Byte{3}, Byte{4}};
  const std::array<Byte, 4> key2 = {Byte{5}, Byte{6}, Byte{7}, Byte{8}};
  const std::array<Byte, 4> miss = {Byte{9}, Byte{9}, Byte{9}, Byte{9}};

  int32_t pos1 = backend.add_key(key1);
  EXPECT_GE(pos1, 0);

  int32_t pos2 = backend.add_key(key2);
  EXPECT_GE(pos2, 0);
  EXPECT_NE(pos1, pos2);

  EXPECT_EQ(2u, backend.size());

  // Scalar lookup hits
  auto res1 = backend.lookup(key1);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(pos1, *res1);

  auto res2 = backend.lookup(key2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(pos2, *res2);

  // Scalar lookup miss
  auto res_miss = backend.lookup(miss);
  EXPECT_FALSE(res_miss.has_value());

  // Backend info
  BackendInfo info = backend.info();
  EXPECT_EQ(ExactBackendKind::kRteHash, info.kind);
  EXPECT_EQ(2u, info.rule_count);
  EXPECT_EQ(4u, info.key_size);

  // Deletion
  EXPECT_TRUE(backend.del_key(key1));
  EXPECT_EQ(1u, backend.size());
  EXPECT_FALSE(backend.lookup(key1).has_value());
  EXPECT_TRUE(backend.lookup(key2).has_value());
}

TEST(RteHashPositionBackendTest, LookupBatchHitMask) {
  RteHashPositionBackend backend(4, 64);
  ASSERT_TRUE(backend.valid());

  const std::array<Byte, 4> k0 = {Byte{10}, Byte{0}, Byte{0}, Byte{1}};
  const std::array<Byte, 4> k1 = {Byte{10}, Byte{0}, Byte{0}, Byte{2}};
  const std::array<Byte, 4> k2 = {Byte{10}, Byte{0}, Byte{0}, Byte{3}};
  const std::array<Byte, 4> miss = {Byte{10}, Byte{0}, Byte{0}, Byte{99}};

  int32_t p0 = backend.add_key(k0);
  int32_t p1 = backend.add_key(k1);
  int32_t p2 = backend.add_key(k2);

  ASSERT_GE(p0, 0);
  ASSERT_GE(p1, 0);
  ASSERT_GE(p2, 0);

  // Batch with: hit(k0), miss, hit(k2), hit(k1)
  const std::array<ConstBytes, 4> keys = {k0, miss, k2, k1};
  std::array<int32_t, 4> results{};

  uint64_t hit_mask = backend.lookup_batch(keys, results);

  // Expected hit mask: bit 0, 2, 3 set -> 0b1101 = 13 (0xd)
  EXPECT_EQ(0xdull, hit_mask);
  EXPECT_EQ(p0, results[0]);
  EXPECT_LT(results[1], 0);  // miss position is negative
  EXPECT_EQ(p2, results[2]);
  EXPECT_EQ(p1, results[3]);
}

// ---------------------------------------------------------------------------
// Data Backend Tests (gate_idx_t semantic result)
// ---------------------------------------------------------------------------

TEST(RteHashDataBackendTest, AddAndLookupScalar) {
  using gate_idx_t = uint16_t;
  RteHashDataBackend<gate_idx_t> backend(4, 64);
  ASSERT_TRUE(backend.valid());

  const std::array<Byte, 4> key1 = {Byte{1}, Byte{2}, Byte{3}, Byte{4}};
  const std::array<Byte, 4> key2 = {Byte{5}, Byte{6}, Byte{7}, Byte{8}};
  const std::array<Byte, 4> miss = {Byte{9}, Byte{9}, Byte{9}, Byte{9}};

  ASSERT_TRUE(backend.add_key_data(key1, gate_idx_t{7}));
  ASSERT_TRUE(backend.add_key_data(key2, gate_idx_t{42}));
  EXPECT_EQ(2u, backend.size());

  // Scalar lookup hits
  auto res1 = backend.lookup(key1);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(7u, *res1);

  auto res2 = backend.lookup(key2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(42u, *res2);

  // Miss
  auto res_miss = backend.lookup(miss);
  EXPECT_FALSE(res_miss.has_value());

  // Deletion
  EXPECT_TRUE(backend.del_key(key1));
  EXPECT_EQ(1u, backend.size());
  EXPECT_FALSE(backend.lookup(key1).has_value());
}

TEST(RteHashDataBackendTest, LookupBatchBulkData) {
  using gate_idx_t = uint16_t;
  RteHashDataBackend<gate_idx_t> backend(4, 64);
  ASSERT_TRUE(backend.valid());

  const std::array<Byte, 4> k0 = {Byte{192}, Byte{168}, Byte{1}, Byte{1}};
  const std::array<Byte, 4> k1 = {Byte{192}, Byte{168}, Byte{1}, Byte{2}};
  const std::array<Byte, 4> miss = {Byte{10}, Byte{0}, Byte{0}, Byte{1}};

  ASSERT_TRUE(backend.add_key_data(k0, gate_idx_t{100}));
  ASSERT_TRUE(backend.add_key_data(k1, gate_idx_t{200}));

  // Batch: k0 (hit), miss, k1 (hit)
  const std::array<ConstBytes, 3> keys = {k0, miss, k1};
  std::array<gate_idx_t, 3> results{};

  uint64_t hit_mask = backend.lookup_batch(keys, results);

  // Bits 0 and 2 set -> 0b101 = 5
  EXPECT_EQ(0x5ull, hit_mask);
  EXPECT_EQ(100u, results[0]);
  EXPECT_EQ(200u, results[2]);
}

// ---------------------------------------------------------------------------
// Position Mode with PackedValueStore Integration Test
// ---------------------------------------------------------------------------

TEST(RteHashPositionBackendTest, PositionMappedThroughPackedValueStore) {
  RteHashPositionBackend backend(4, 64);
  ASSERT_TRUE(backend.valid());

  // 6-byte arbitrary value payload
  PackedValueStore store(6);

  const std::array<Byte, 4> k0 = {Byte{1}, Byte{0}, Byte{0}, Byte{0}};
  const std::array<Byte, 6> v0 = {Byte{0xaa}, Byte{0xbb}, Byte{0xcc},
                                  Byte{0xdd}, Byte{0xee}, Byte{0xff}};

  ResultSlot s0 = store.Add(v0);
  ASSERT_NE(ResultSlot(0), s0);

  int32_t pos = backend.add_key(k0);
  ASSERT_GE(pos, 0);

  // Position is mapped to slot in an internal translation array
  std::vector<ResultSlot> pos_to_slot;
  if (pos >= static_cast<int32_t>(pos_to_slot.size())) {
    pos_to_slot.resize(pos + 1);
  }
  pos_to_slot[pos] = s0;

  // Packet path lookup:
  auto lookup_pos = backend.lookup(k0);
  ASSERT_TRUE(lookup_pos.has_value());
  ResultSlot resolved_slot = pos_to_slot[*lookup_pos];
  ConstBytes resolved_val = store.lookup(resolved_slot);

  EXPECT_EQ(v0.size(), resolved_val.size());
  EXPECT_EQ(0, std::memcmp(v0.data(), resolved_val.data(), v0.size()));
}

TEST(RteHashPositionBackendTest, LookupBatchPackedStride) {
  RteHashPositionBackend backend(4, 64);
  ASSERT_TRUE(backend.valid());

  const std::array<Byte, 4> k0 = {Byte{10}, Byte{0}, Byte{0}, Byte{1}};
  const std::array<Byte, 4> k1 = {Byte{10}, Byte{0}, Byte{0}, Byte{2}};
  const std::array<Byte, 4> miss = {Byte{10}, Byte{0}, Byte{0}, Byte{99}};

  int32_t p0 = backend.add_key(k0);
  int32_t p1 = backend.add_key(k1);
  ASSERT_GE(p0, 0);
  ASSERT_GE(p1, 0);

  // Packed keys with stride 8 (key_len 4 + 4 padding): k0, miss, k1.
  std::array<Byte, 3 * 8> packed{};
  std::memcpy(packed.data(), k0.data(), 4);
  std::memcpy(packed.data() + 8, miss.data(), 4);
  std::memcpy(packed.data() + 16, k1.data(), 4);
  std::array<int32_t, 3> results{};

  uint64_t hits =
      backend.lookup_batch_packed(ConstBytes(packed), 8, results);
  EXPECT_EQ(0x5ull, hits);
  EXPECT_EQ(p0, results[0]);
  EXPECT_EQ(p1, results[2]);
}

TEST(RteHashDataBackendTest, LookupBatchPackedStride) {
  using gate_idx_t = uint16_t;
  RteHashDataBackend<gate_idx_t> backend(4, 64);
  ASSERT_TRUE(backend.valid());

  const std::array<Byte, 4> k0 = {Byte{192}, Byte{168}, Byte{1}, Byte{1}};
  const std::array<Byte, 4> k1 = {Byte{192}, Byte{168}, Byte{1}, Byte{2}};
  const std::array<Byte, 4> miss = {Byte{10}, Byte{0}, Byte{0}, Byte{1}};

  ASSERT_TRUE(backend.add_key_data(k0, gate_idx_t{100}));
  ASSERT_TRUE(backend.add_key_data(k1, gate_idx_t{200}));

  std::array<Byte, 3 * 8> packed{};
  std::memcpy(packed.data(), k0.data(), 4);
  std::memcpy(packed.data() + 8, miss.data(), 4);
  std::memcpy(packed.data() + 16, k1.data(), 4);
  std::array<gate_idx_t, 3> results{};

  uint64_t hits =
      backend.lookup_batch_packed(ConstBytes(packed), 8, results);
  EXPECT_EQ(0x5ull, hits);
  EXPECT_EQ(100u, results[0]);
  EXPECT_EQ(200u, results[2]);
}

}  // namespace
