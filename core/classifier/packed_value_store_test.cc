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

#include "classifier/packed_value_store.h"

namespace {

using bess::classifier::ConstBytes;
using bess::classifier::PackedValueStore;
using bess::classifier::ResultSlot;

// ---------------------------------------------------------------------------
// Slot 0 is always invalid / miss
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, Slot0AlwaysEmpty_EmptyStore) {
  PackedValueStore store(4);
  EXPECT_TRUE(store.lookup(ResultSlot(0)).empty());
}

TEST(PackedValueStoreTest, Slot0AlwaysEmpty_NonEmptyStore) {
  PackedValueStore store(4);
  const std::array<std::byte, 4> val{std::byte{1}, std::byte{2}, std::byte{3},
                                     std::byte{4}};
  store.Add(val);
  EXPECT_TRUE(store.lookup(ResultSlot(0)).empty());
}

// ---------------------------------------------------------------------------
// Add returns consecutive 1-based slots
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, AddReturnsConsecutiveSlots) {
  PackedValueStore store(2);
  const std::array<std::byte, 2> v1{std::byte{0xAA}, std::byte{0xBB}};
  const std::array<std::byte, 2> v2{std::byte{0xCC}, std::byte{0xDD}};
  const std::array<std::byte, 2> v3{std::byte{0xEE}, std::byte{0xFF}};

  EXPECT_EQ(ResultSlot(1), store.Add(v1));
  EXPECT_EQ(ResultSlot(2), store.Add(v2));
  EXPECT_EQ(ResultSlot(3), store.Add(v3));
  EXPECT_EQ(3u, store.size());
}

// ---------------------------------------------------------------------------
// Out-of-range slot returns empty
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, OutOfRangeSlotReturnsEmpty) {
  PackedValueStore store(4);
  const std::array<std::byte, 4> val{};
  store.Add(val);  // only slot 1 exists

  EXPECT_TRUE(store.lookup(ResultSlot(99)).empty());
  EXPECT_TRUE(store.lookup(ResultSlot(2)).empty());
}

// ---------------------------------------------------------------------------
// Round-trip: 1-byte value_size
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, RoundTrip1Byte) {
  PackedValueStore store(1);

  const std::array<std::byte, 1> a{std::byte{0x11}};
  const std::array<std::byte, 1> b{std::byte{0x22}};
  const std::array<std::byte, 1> c{std::byte{0x33}};

  const ResultSlot s1 = store.Add(a);
  const ResultSlot s2 = store.Add(b);
  const ResultSlot s3 = store.Add(c);

  ASSERT_EQ(ResultSlot(1), s1);
  ASSERT_EQ(ResultSlot(2), s2);
  ASSERT_EQ(ResultSlot(3), s3);

  EXPECT_EQ(std::byte{0x11}, store.lookup(s1)[0]);
  EXPECT_EQ(std::byte{0x22}, store.lookup(s2)[0]);
  EXPECT_EQ(std::byte{0x33}, store.lookup(s3)[0]);
}

// ---------------------------------------------------------------------------
// Round-trip: 4-byte value_size
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, RoundTrip4Byte) {
  PackedValueStore store(4);

  const std::array<std::byte, 4> v1{std::byte{1}, std::byte{2}, std::byte{3},
                                     std::byte{4}};
  const std::array<std::byte, 4> v2{std::byte{5}, std::byte{6}, std::byte{7},
                                     std::byte{8}};

  const ResultSlot s1 = store.Add(v1);
  const ResultSlot s2 = store.Add(v2);

  const ConstBytes r1 = store.lookup(s1);
  const ConstBytes r2 = store.lookup(s2);

  ASSERT_EQ(4u, r1.size());
  ASSERT_EQ(4u, r2.size());

  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(v1[i], r1[i]) << "slot1 byte " << i;
    EXPECT_EQ(v2[i], r2[i]) << "slot2 byte " << i;
  }
}

// ---------------------------------------------------------------------------
// Round-trip: 13-byte value_size (non-power-of-two)
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, RoundTrip13Byte) {
  PackedValueStore store(13);

  std::array<std::byte, 13> v1{};
  std::array<std::byte, 13> v2{};
  for (size_t i = 0; i < 13; ++i) {
    v1[i] = static_cast<std::byte>(i + 1);
    v2[i] = static_cast<std::byte>(i + 100);
  }

  const ResultSlot s1 = store.Add(v1);
  const ResultSlot s2 = store.Add(v2);

  const ConstBytes r1 = store.lookup(s1);
  const ConstBytes r2 = store.lookup(s2);

  ASSERT_EQ(13u, r1.size());
  ASSERT_EQ(13u, r2.size());

  for (size_t i = 0; i < 13; ++i) {
    EXPECT_EQ(v1[i], r1[i]) << "slot1 byte " << i;
    EXPECT_EQ(v2[i], r2[i]) << "slot2 byte " << i;
  }
}

// ---------------------------------------------------------------------------
// value_size and storage_bytes invariants
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, ValueSizeAndStorageBytes) {
  PackedValueStore store(7);

  EXPECT_EQ(7u, store.value_size());
  EXPECT_EQ(0u, store.storage_bytes());
  EXPECT_EQ(0u, store.size());

  const std::array<std::byte, 7> val{};
  store.Add(val);
  EXPECT_EQ(7u, store.storage_bytes());
  EXPECT_EQ(1u, store.size());
  EXPECT_EQ(store.size() * store.value_size(), store.storage_bytes());

  store.Add(val);
  EXPECT_EQ(14u, store.storage_bytes());
  EXPECT_EQ(2u, store.size());
  EXPECT_EQ(store.size() * store.value_size(), store.storage_bytes());
}

// ---------------------------------------------------------------------------
// Mismatch returns slot 0 (wrong size input)
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, SizeMismatchReturnsSlot0) {
  PackedValueStore store(4);
  const std::array<std::byte, 3> short_val{};
  // Should return the invalid slot (0)
  EXPECT_EQ(ResultSlot(0), store.Add(short_val));
  // Store must remain empty after the failed add
  EXPECT_EQ(0u, store.size());
}

// ---------------------------------------------------------------------------
// Zero value_size: rejected and Add returns slot 0
// ---------------------------------------------------------------------------

TEST(PackedValueStoreTest, ZeroValueSizeRejected) {
  PackedValueStore store(0);

  EXPECT_FALSE(store.valid());
  EXPECT_EQ(0u, store.value_size());
  EXPECT_EQ(0u, store.size());
  EXPECT_EQ(0u, store.storage_bytes());

  // lookup on slot 0 must be empty
  EXPECT_TRUE(store.lookup(ResultSlot(0)).empty());

  // Zero-width store rejects all Adds and returns ResultSlot(0)
  const ConstBytes empty_val{};
  EXPECT_EQ(ResultSlot(0), store.Add(empty_val));
  EXPECT_EQ(0u, store.size());
}

}  // namespace
