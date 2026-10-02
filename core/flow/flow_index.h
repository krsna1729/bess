// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_FLOW_INDEX_H_
#define BESS_FLOW_FLOW_INDEX_H_

#include <bit>
#include <cstddef>
#include <cstdint>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace bess::flow::detail {

// The directory of a worker-owned flow table: a hash of the key to a 32-bit
// key id. Decision D-052 records why this table exists next to rte_hash and
// CuckooMap.
//
//  * Fixed size. Memory is handed in at construction and never grows; there
//    is no resize, no rehash and no allocation on any operation. It holds at
//    most `max_keys` ids, and is sized so that it always has room for them.
//  * One cache line per probe. A bucket is 64 bytes: eight 16-bit tags, eight
//    32-bit ids, and an overflow count. A lookup loads the home bucket,
//    compares the eight tags at once, and compares full keys (through the
//    caller's functor, so hash and equality inline) only for tags that match.
//    A miss touches one line unless the home bucket has overflowed.
//  * Overflow chains, not cuckoo displacement. When the home bucket is full an
//    insert moves on to the next bucket and counts the pass in the buckets it
//    skipped; erase takes the counts back, so churn leaves no tombstones. A
//    lookup stops at the first bucket whose count is zero.
//  * No atomics and no locks: one thread uses it at a time.
//
// It stores ids, not keys; the owner keeps the keys and resolves an id to its
// key inside the functor passed to Find().
struct alignas(64) Bucket {
  static constexpr size_t kSlots = 8;
  uint16_t tags[kSlots];  // 0 marks an empty slot
  uint32_t ids[kSlots];
  uint8_t overflow;  // inserts that skipped this (full) bucket; 255 sticks
  uint8_t pad[15];
};
static_assert(sizeof(Bucket) == 64);

// Which slots of `tags` equal `tag`: bit i set for tags[i] == tag.
inline uint32_t MatchTagsScalar(const uint16_t *tags, uint16_t tag) noexcept {
  uint32_t mask = 0;
  for (size_t i = 0; i < Bucket::kSlots; i++) {
    mask |= static_cast<uint32_t>(tags[i] == tag) << i;
  }
  return mask;
}

inline uint32_t MatchTags(const uint16_t *tags, uint16_t tag) noexcept {
#if defined(__SSE2__)
  const __m128i have = _mm_load_si128(reinterpret_cast<const __m128i *>(tags));
  const __m128i eq = _mm_cmpeq_epi16(have, _mm_set1_epi16(static_cast<short>(tag)));
  // Narrow the sixteen byte lanes (two per tag) to one bit per tag.
  return static_cast<uint32_t>(
             _mm_movemask_epi8(_mm_packs_epi16(eq, eq))) &
         0xffu;
#else
  return MatchTagsScalar(tags, tag);
#endif
}

// Weak or structured user hashes (std::hash<uint64_t> is the identity) must
// not decide bucket placement directly: two multiplies and a fold spread every
// input bit over the whole word.
inline uint64_t Mix(uint64_t h) noexcept {
  h = (h ^ (h >> 32)) * 0xd6e8feb86659fd93ull;
  h ^= h >> 32;
  h *= 0x9e3779b97f4a7c15ull;
  return h;
}

class FlowIndex {
 public:
  static constexpr uint32_t kNone = 0xffffffffu;

  // Average entries per bucket the index is sized for at full occupancy (of
  // eight): 50% load. Deleting never moves an entry back, so under steady
  // churn entries sit further from home than in a fresh table, and a lookup
  // that misses walks on until it reaches a bucket that never overflowed.
  // Simulated with random keys at steady churn, the average chain a missing
  // lookup walks is 1.24 buckets at 4 entries per bucket, 2.1 at 5, 3.4 at
  // 5.5 and 7.2 at 6 (a hit: 1.04, 1.14, 1.23, 1.42), so the index is sized
  // at 4 and trades 16 bytes per key for a one-line miss.
  static constexpr size_t kEntriesPerBucket = 4;

  // Buckets for `max_keys` ids; at least one.
  static constexpr size_t BucketsFor(size_t max_keys) noexcept {
    return (max_keys + kEntriesPerBucket - 1) / kEntriesPerBucket +
           (max_keys == 0 ? 1 : 0);
  }

  struct Hashed {
    uint32_t bucket;
    uint16_t tag;  // never 0
  };

  FlowIndex() = default;
  // `buckets` must be zeroed, 64-byte aligned and hold `count` (>= 1) buckets.
  FlowIndex(Bucket *buckets, uint32_t count) noexcept
      : buckets_(buckets), count_(count) {}

  // Splits a user hash into a home bucket (the high word scaled onto the
  // bucket count, so any count works) and a tag (independent bits). The hash
  // is mixed first: however weak or structured, it must not decide placement
  // directly. A multiplicative hash of keys that differ in few bits is a
  // lattice, not noise (measured: a counter in the top 16 bits of a 32-byte
  // key made lookups walk 1.3 buckets against 1.05 for random hashes), and
  // std::hash<uint64_t> is the identity.
  Hashed Split(uint64_t user_hash) const noexcept {
    const uint64_t m = Mix(user_hash);
    const auto bucket = static_cast<uint32_t>(((m >> 32) * count_) >> 32);
    auto tag = static_cast<uint16_t>(m >> 8);
    tag = static_cast<uint16_t>(tag + (tag == 0));
    return {bucket, tag};
  }

  void Prefetch(Hashed h) const noexcept {
    __builtin_prefetch(&buckets_[h.bucket], 0, 3);
  }

  // The first id for which eq(id) holds among those whose tag matches, or
  // kNone.
  template <typename Eq>
  uint32_t Find(Hashed h, Eq &&eq) const noexcept {
    uint32_t b = h.bucket;
    for (uint32_t step = 0; step < count_; step++) {
      const Bucket &bucket = buckets_[b];
      for (uint32_t m = MatchTags(bucket.tags, h.tag); m != 0; m &= m - 1) {
        const uint32_t id = bucket.ids[std::countr_zero(m)];
        if (eq(id)) {
          return id;
        }
      }
      if (bucket.overflow == 0) {
        return kNone;
      }
      b = (b + 1 == count_) ? 0 : b + 1;
    }
    return kNone;
  }

  // The id of the first tag match in the home bucket, for prefetching what it
  // names; kNone if none.
  uint32_t FirstCandidate(Hashed h) const noexcept {
    const Bucket &bucket = buckets_[h.bucket];
    const uint32_t m = MatchTags(bucket.tags, h.tag);
    return m != 0 ? bucket.ids[std::countr_zero(m)] : kNone;
  }

  // Adds `id` (not already present for this key: the caller looked first).
  // False only if every bucket is full, which a correctly sized index cannot
  // reach.
  bool Insert(Hashed h, uint32_t id) noexcept {
    uint32_t b = h.bucket;
    for (uint32_t step = 0; step < count_; step++) {
      Bucket &bucket = buckets_[b];
      const uint32_t empty = MatchTags(bucket.tags, 0);
      if (empty != 0) {
        const int slot = std::countr_zero(empty);
        bucket.tags[slot] = h.tag;
        bucket.ids[slot] = id;
        // The buckets this id skipped now lead on to it.
        uint32_t skipped = h.bucket;
        for (uint32_t s = 0; s < step; s++) {
          Bucket &passed = buckets_[skipped];
          if (passed.overflow != 255) {
            passed.overflow++;
          }
          skipped = (skipped + 1 == count_) ? 0 : skipped + 1;
        }
        return true;
      }
      b = (b + 1 == count_) ? 0 : b + 1;
    }
    return false;
  }

  // Removes `id`, which was inserted under the same hash. False if absent.
  bool Erase(Hashed h, uint32_t id) noexcept {
    uint32_t b = h.bucket;
    for (uint32_t step = 0; step < count_; step++) {
      Bucket &bucket = buckets_[b];
      for (uint32_t m = MatchTags(bucket.tags, h.tag); m != 0; m &= m - 1) {
        const int slot = std::countr_zero(m);
        if (bucket.ids[slot] != id) {
          continue;
        }
        bucket.tags[slot] = 0;
        uint32_t passed = h.bucket;
        for (uint32_t s = 0; s < step; s++) {
          Bucket &skipped = buckets_[passed];
          if (skipped.overflow != 255) {
            skipped.overflow--;
          }
          passed = (passed + 1 == count_) ? 0 : passed + 1;
        }
        return true;
      }
      if (bucket.overflow == 0) {
        return false;
      }
      b = (b + 1 == count_) ? 0 : b + 1;
    }
    return false;
  }

  uint32_t bucket_count() const noexcept { return count_; }

  // Test and benchmark insight: how many chained buckets a lookup of this
  // hash visits at most (1 = the home bucket decides).
  uint32_t ProbeLength(Hashed h) const noexcept {
    uint32_t b = h.bucket;
    uint32_t steps = 1;
    while (steps < count_ && buckets_[b].overflow != 0) {
      b = (b + 1 == count_) ? 0 : b + 1;
      steps++;
    }
    return steps;
  }

 private:
  Bucket *buckets_ = nullptr;
  uint32_t count_ = 0;
};

}  // namespace bess::flow::detail

#endif  // BESS_FLOW_FLOW_INDEX_H_
