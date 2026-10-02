// SPDX-License-Identifier: BSD-3-Clause

#include "flow/flow_index.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "flow/flow_key.h"
#include "flow/flow_storage.h"

namespace bess::flow {
namespace {

using detail::Bucket;
using detail::FlowIndex;

// A key with a hole in it.
struct Padded {
  uint8_t proto;
  uint32_t addr;
};
struct Packed {
  uint32_t a;
  uint32_t b;
};
struct NotTrivial {
  NotTrivial(const NotTrivial &) {}
};

static_assert(FixedFlowKey<Padded>);
// has_unique_object_representations is false for a struct with padding.
static_assert(!ByteHashableFlowKey<Padded>);
static_assert(ByteHashableFlowKey<Packed>);
static_assert(ByteHashableFlowKey<uint64_t>);
static_assert(!FixedFlowKey<NotTrivial>);
static_assert(!FixedFlowKey<const Packed>);

// An author who zeroes the padding may opt in.
struct ZeroedPad {
  uint8_t proto;
  uint32_t addr;
};

}  // namespace

template <>
struct FlowKeyTraits<ZeroedPad> {
  static constexpr bool canonical_representation = true;
};

namespace {

static_assert(ByteHashableFlowKey<ZeroedPad>);
static_assert(FlowKeyOps<Packed, DefaultFlowHash<Packed>,
                         DefaultFlowEqual<Packed>>);

// The index never holds keys; this harness keeps them in a vector so a test
// can run the same Find/Insert/Erase protocol the table does.
struct Harness {
  explicit Harness(size_t max_keys)
      : storage(new Bucket[FlowIndex::BucketsFor(max_keys)]()),
        index(storage.get(), static_cast<uint32_t>(FlowIndex::BucketsFor(max_keys))) {}

  uint32_t Find(uint64_t key) const {
    return index.Find(index.Split(key), [&](uint32_t id) { return keys[id] == key; });
  }
  uint32_t Insert(uint64_t key) {
    const uint32_t id = static_cast<uint32_t>(keys.size());
    keys.push_back(key);
    EXPECT_TRUE(index.Insert(index.Split(key), id));
    return id;
  }
  bool Erase(uint32_t id) { return index.Erase(index.Split(keys[id]), id); }

  std::unique_ptr<Bucket[]> storage;
  FlowIndex index;
  std::vector<uint64_t> keys;
};

TEST(FlowIndexTest, BucketIsOneCacheLine) {
  EXPECT_EQ(64u, sizeof(Bucket));
  EXPECT_EQ(64u, alignof(Bucket));
}

// The SSE2 tag compare is the hot-path version; the scalar one is the
// definition. They must agree for every tag position and every pattern.
TEST(FlowIndexTest, SimdTagMatchEqualsScalarDefinition) {
  std::mt19937 rng(7);
  alignas(64) uint16_t tags[8];
  for (int trial = 0; trial < 20000; trial++) {
    // A small alphabet makes matches (including several per bucket) likely.
    for (auto &t : tags) t = static_cast<uint16_t>(rng() % 5);
    const auto probe = static_cast<uint16_t>(rng() % 5);
    ASSERT_EQ(detail::MatchTagsScalar(tags, probe),
              detail::MatchTags(tags, probe));
  }
  for (uint32_t pattern = 0; pattern < 256; pattern++) {
    for (int i = 0; i < 8; i++) tags[i] = (pattern >> i & 1) ? 0xffff : 0x8000;
    ASSERT_EQ(pattern, detail::MatchTags(tags, 0xffff));
  }
}

TEST(FlowIndexTest, SplitStaysInRangeAndTagIsNeverEmpty) {
  for (uint32_t buckets : {1u, 2u, 3u, 7u, 1000u, 65536u, 1000003u}) {
    Bucket *none = nullptr;
    FlowIndex index(none, buckets);
    std::mt19937_64 rng(buckets);
    for (int i = 0; i < 20000; i++) {
      const uint64_t h = (i % 3 == 0) ? rng() : (i % 3 == 1 ? uint64_t{static_cast<uint32_t>(i)} : 0);
      const auto hashed = index.Split(h);
      ASSERT_LT(hashed.bucket, buckets);
      ASSERT_NE(0u, hashed.tag) << "tag 0 means an empty slot";
    }
  }
}

TEST(FlowIndexTest, FindInsertEraseRoundTrip) {
  Harness h(100);
  EXPECT_EQ(FlowIndex::kNone, h.Find(42));
  const uint32_t a = h.Insert(42);
  const uint32_t b = h.Insert(43);
  EXPECT_EQ(a, h.Find(42));
  EXPECT_EQ(b, h.Find(43));
  EXPECT_TRUE(h.Erase(a));
  EXPECT_FALSE(h.Erase(a)) << "erasing twice must report absence";
  EXPECT_EQ(FlowIndex::kNone, h.Find(42));
  EXPECT_EQ(b, h.Find(43));
}

// Sized for N keys, the index takes N keys whatever the hash does with them:
// sequential integers through the identity are the classic bad input.
TEST(FlowIndexTest, HoldsItsDesignedKeyCountForStructuredKeys) {
  for (size_t n : {1u, 5u, 8u, 9u, 100u, 4096u, 100000u}) {
    for (int shape = 0; shape < 3; shape++) {
      Harness h(n);
      for (size_t i = 0; i < n; i++) {
        const uint64_t key = shape == 0   ? i
                             : shape == 1 ? (uint64_t{i} << 32)
                                          : i * 4096;  // low bits constant
        h.Insert(key);
      }
      for (uint32_t id = 0; id < n; id++) {
        ASSERT_EQ(id, h.Find(h.keys[id])) << "n=" << n << " shape=" << shape;
      }
      if (n >= 4096) {
        // Probe lengths: the home bucket should decide almost every lookup.
        uint64_t total = 0;
        uint32_t worst = 0;
        for (uint32_t id = 0; id < n; id++) {
          const uint32_t len = h.index.ProbeLength(h.index.Split(h.keys[id]));
          total += len;
          worst = std::max(worst, len);
        }
        EXPECT_LT(static_cast<double>(total) / n, 1.15) << "shape " << shape;
        EXPECT_LE(worst, 6u) << "shape " << shape;
      }
    }
  }
}

// Structured keys through the default hash and the index's mixing: sequential
// counters in one half of the key, in the other, and in both at once.
TEST(FlowIndexTest, DefaultHashSpreadsStructuredKeys) {
  constexpr size_t kN = 100000;
  const DefaultFlowHash<Packed> hash;
  for (int shape = 0; shape < 3; shape++) {
    std::vector<Packed> keys;
    for (uint32_t i = 0; i < kN; i++) {
      keys.push_back(shape == 0 ? Packed{i, 0} : shape == 1 ? Packed{0, i} : Packed{i, i});
    }
    std::unique_ptr<Bucket[]> storage(new Bucket[FlowIndex::BucketsFor(kN)]());
    FlowIndex index(storage.get(), static_cast<uint32_t>(FlowIndex::BucketsFor(kN)));
    for (uint32_t id = 0; id < kN; id++) {
      ASSERT_TRUE(index.Insert(index.Split(hash(keys[id])), id));
    }
    uint64_t total = 0;
    for (uint32_t id = 0; id < kN; id++) {
      const auto hashed = index.Split(hash(keys[id]));
      ASSERT_EQ(id, index.Find(hashed, [&](uint32_t c) { return keys[c].a == keys[id].a && keys[c].b == keys[id].b; }));
      total += index.ProbeLength(hashed);
    }
    EXPECT_LT(static_cast<double>(total) / kN, 1.12) << "shape " << shape;
  }
}

// A counter placed at every byte offset of a key must spread, not only a
// counter in the low bytes.
TEST(FlowIndexTest, DefaultHashSpreadsACounterAtAnyByteOffset) {
  struct Wide {
    uint64_t w[4];
  };
  static_assert(ByteHashableFlowKey<Wide>);
  const DefaultFlowHash<Wide> hash;
  for (int word = 0; word < 4; word++) {
    for (int shift = 0; shift < 64; shift += 8) {
      const size_t n = std::min<size_t>(50000, size_t{1} << std::min(16, 64 - shift));
      std::vector<Wide> keys(n);
      for (size_t i = 0; i < n; i++) {
        keys[i] = Wide{};
        keys[i].w[word] = uint64_t{i} << shift;
      }
      std::unique_ptr<Bucket[]> storage(new Bucket[FlowIndex::BucketsFor(n)]());
      FlowIndex index(storage.get(), static_cast<uint32_t>(FlowIndex::BucketsFor(n)));
      for (uint32_t id = 0; id < n; id++) {
        ASSERT_TRUE(index.Insert(index.Split(hash(keys[id])), id));
      }
      uint64_t total = 0;
      for (uint32_t id = 0; id < n; id++) {
        total += index.ProbeLength(index.Split(hash(keys[id])));
      }
      EXPECT_LT(static_cast<double>(total) / static_cast<double>(n), 1.12)
          << "word " << word << " shift " << shift;
    }
  }
}

// A hash that sends every key to one bucket is the hash-flood case: far more
// keys than one bucket holds chain on, the 8-bit overflow counters saturate,
// and the table must still find, erase and reuse everything.
TEST(FlowIndexTest, SaturatedOverflowCountsStayCorrect) {
  Harness h(2000);
  // Insert directly with one hash for every key.
  const auto hashed = h.index.Split(0x1234);
  std::vector<uint32_t> ids;
  for (uint32_t i = 0; i < 1500; i++) {
    h.keys.push_back(i);
    ASSERT_TRUE(h.index.Insert(hashed, i));
    ids.push_back(i);
  }
  auto find = [&](uint32_t key) {
    return h.index.Find(hashed, [&](uint32_t id) { return h.keys[id] == key; });
  };
  for (uint32_t i = 0; i < 1500; i++) ASSERT_EQ(i, find(i));
  EXPECT_EQ(FlowIndex::kNone, find(99999));
  // Erase the middle, then everything: lookups of the survivors stay right.
  for (uint32_t i = 500; i < 1000; i++) ASSERT_TRUE(h.index.Erase(hashed, i));
  for (uint32_t i = 0; i < 500; i++) ASSERT_EQ(i, find(i));
  for (uint32_t i = 1000; i < 1500; i++) ASSERT_EQ(i, find(i));
  for (uint32_t i = 500; i < 1000; i++) ASSERT_EQ(FlowIndex::kNone, find(i));
  for (uint32_t i = 0; i < 500; i++) ASSERT_TRUE(h.index.Erase(hashed, i));
  for (uint32_t i = 1000; i < 1500; i++) ASSERT_TRUE(h.index.Erase(hashed, i));
  EXPECT_EQ(FlowIndex::kNone, find(7));
  // The table is reusable after the flood.
  for (uint32_t i = 0; i < 1500; i++) {
    h.keys.push_back(100000 + i);
    ASSERT_TRUE(h.index.Insert(hashed, static_cast<uint32_t>(h.keys.size() - 1)));
  }
}

// Erase leaves no tombstones: after heavy churn each bucket's overflow count
// equals exactly the number of live entries that were placed beyond it, so a
// count never lingers for an entry that is gone. (Entries do not move back
// when an earlier slot frees, so chains are longer than in a fresh table; the
// sizing constant in flow_index.h accounts for it, and the bound below holds
// it to the simulated value.)
TEST(FlowIndexTest, ChurnKeepsOverflowCountsExactAndChainsShort) {
  constexpr size_t kLive = 20000;
  Harness churned(kLive);
  std::mt19937_64 rng(11);
  std::vector<uint32_t> live;
  for (size_t i = 0; i < kLive; i++) {
    live.push_back(churned.Insert(rng()));
  }
  for (int round = 0; round < 400000; round++) {
    const size_t victim = rng() % live.size();
    ASSERT_TRUE(churned.Erase(live[victim]));
    live[victim] = churned.Insert(rng());
  }

  const uint32_t buckets = churned.index.bucket_count();
  std::vector<int> expected(buckets, 0);
  for (uint32_t b = 0; b < buckets; b++) {
    for (size_t slot = 0; slot < Bucket::kSlots; slot++) {
      if (churned.storage[b].tags[slot] == 0) continue;
      const uint32_t id = churned.storage[b].ids[slot];
      uint32_t walk = churned.index.Split(churned.keys[id]).bucket;
      while (walk != b) {
        expected[walk]++;
        walk = (walk + 1 == buckets) ? 0 : walk + 1;
      }
    }
  }
  for (uint32_t b = 0; b < buckets; b++) {
    ASSERT_EQ(expected[b], churned.storage[b].overflow) << "bucket " << b;
  }

  uint64_t total = 0;
  for (uint32_t id : live) {
    ASSERT_EQ(id, churned.Find(churned.keys[id]));
    total += churned.index.ProbeLength(churned.index.Split(churned.keys[id]));
  }
  EXPECT_LT(static_cast<double>(total) / live.size(), 1.5)
      << "a missing lookup should walk about one bucket at the designed load";
}

TEST(FlowIndexTest, DefaultHashAgreesWithEqualityAndSeparatesInputs) {
  const DefaultFlowHash<Packed> hash;
  const DefaultFlowEqual<Packed> equal;
  EXPECT_EQ(hash({1, 2}), hash({1, 2}));
  EXPECT_NE(hash({1, 2}), hash({2, 1}));
  EXPECT_TRUE(equal({1, 2}, {1, 2}));
  EXPECT_FALSE(equal({1, 2}, {1, 3}));

  // Every byte of a key reaches the hash, whatever its width.
  std::mt19937_64 rng(3);
  struct Wide {
    std::byte b[37];
  };
  static_assert(ByteHashableFlowKey<Wide>);
  const DefaultFlowHash<Wide> wide_hash;
  Wide base{};
  for (auto &x : base.b) x = static_cast<std::byte>(rng());
  for (size_t i = 0; i < sizeof(Wide); i++) {
    Wide changed = base;
    changed.b[i] ^= std::byte{0x10};
    EXPECT_NE(wide_hash(base), wide_hash(changed)) << "byte " << i;
  }
}

}  // namespace
}  // namespace bess::flow
