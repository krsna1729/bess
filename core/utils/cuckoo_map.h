// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
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

// Streamlined hash table implementation, with emphasis on lookup performance.
// Key and value sizes are fixed. Lookup is thread-safe, but update is not.
//
// Note: If you want to use a custom hash function, it should be a reasonably
// good one. If more than 8 (2 * kEntriesPerBucket) key values collide with
// the same hash value, Insert() may fail returning nullptr.

#ifndef BESS_UTILS_CUCKOOMAP_H_
#define BESS_UTILS_CUCKOOMAP_H_

#include <algorithm>
#include <functional>
#include <limits>
#include <stack>
#include <type_traits>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "../debug.h"
#include "../dataplane/batch_tuning.h"
#include "common.h"

namespace bess {
namespace utils {

typedef uint32_t HashResult;
typedef uint32_t EntryIndex;

// A Hash table implementation using cuckoo hashing
//
// Example usage:
//
//  CuckooMap<uint32_t, uint64_t> cuckoo;
//  cuckoo.Insert(1, 99);
//  std::pair<uint32_t, uint64_t>* result = cuckoo.Find(1)
//  std::cout << "key: " << result->first << ", value: "
//    << result->second << std::endl;
//
// The output should be "key: 1, value: 99"
//
// For more examples, please refer to cuckoo_map_test.cc

template <typename K, typename V, typename H = std::hash<K>,
          typename E = std::equal_to<K>>
class CuckooMap {
 public:
  typedef std::pair<K, V> Entry;
  struct LookupStats {
    size_t primary_hits = 0;
    size_t secondary_hits = 0;
    size_t misses = 0;
  };
  class iterator {
   public:
    using difference_type = std::ptrdiff_t;
    using value_type = Entry;
    using pointer = Entry*;
    using reference = Entry&;
    using iterator_category = std::forward_iterator_tag;

    iterator(CuckooMap& map, size_t bucket, size_t slot)
        : map_(map), bucket_idx_(bucket), slot_idx_(slot) {
      while (bucket_idx_ < map_.buckets_.size() &&
             map_.buckets_[bucket_idx_].hash_values[slot_idx_] == 0) {
        slot_idx_++;
        if (slot_idx_ == kEntriesPerBucket) {
          slot_idx_ = 0;
          bucket_idx_++;
        }
      }
    }

    iterator& operator++() {  // Pre-increment
      do {
        slot_idx_++;
        if (slot_idx_ == kEntriesPerBucket) {
          slot_idx_ = 0;
          bucket_idx_++;
        }
      } while (bucket_idx_ < map_.buckets_.size() &&
               map_.buckets_[bucket_idx_].hash_values[slot_idx_] == 0);
      return *this;
    }

    iterator operator++(int) {  // Pre-increment
      iterator tmp(*this);
      do {
        slot_idx_++;
        if (slot_idx_ == kEntriesPerBucket) {
          slot_idx_ = 0;
          bucket_idx_++;
        }
      } while (bucket_idx_ < map_.buckets_.size() &&
               map_.buckets_[bucket_idx_].hash_values[slot_idx_] == 0);
      return tmp;
    }

    bool operator==(const iterator& rhs) const {
      return &map_ == &rhs.map_ && bucket_idx_ == rhs.bucket_idx_ &&
             slot_idx_ == rhs.slot_idx_;
    }

    bool operator!=(const iterator& rhs) const {
      return &map_ != &rhs.map_ || bucket_idx_ != rhs.bucket_idx_ ||
             slot_idx_ != rhs.slot_idx_;
    }

    reference operator*() {
      EntryIndex idx = map_.buckets_[bucket_idx_].entry_indices[slot_idx_];
      return map_.entries_[idx];
    }

    pointer operator->() {
      EntryIndex idx = map_.buckets_[bucket_idx_].entry_indices[slot_idx_];
      return &map_.entries_[idx];
    }

   private:
    CuckooMap& map_;
    size_t bucket_idx_;
    size_t slot_idx_;
  };

  CuckooMap(size_t reserve_buckets = kInitNumBucket,
            size_t reserve_entries = kInitNumEntries)
      : bucket_mask_(reserve_buckets - 1),
        num_entries_(0),
        buckets_(reserve_buckets),
        entries_(reserve_entries),
        free_entry_indices_() {
    // the number of buckets must be a power of 2
    CHECK_EQ(align_ceil_pow2(reserve_buckets), reserve_buckets);

    for (int i = reserve_entries - 1; i >= 0; --i) {
      free_entry_indices_.push(i);
    }
  }

  // Not allowing copying for now
  CuckooMap(CuckooMap&) = delete;
  CuckooMap& operator=(CuckooMap&) = delete;

  // Allow move
  CuckooMap(CuckooMap&&) = default;
  CuckooMap& operator=(CuckooMap&&) = default;

  iterator begin() { return iterator(*this, 0, 0); }
  iterator end() { return iterator(*this, buckets_.size(), 0); }

  template <typename... Args>
  Entry* DoEmplace(const K& key, const H& hasher, const E& eq, Args&&... args) {
    Entry* entry;
    HashResult primary = Hash(key, hasher);

    EntryIndex idx = FindWithHash(primary, key, eq);
    if (idx != kInvalidEntryIdx) {
      entry = &entries_[idx];
      new (&entry->second) V(std::forward<Args>(args)...);
      return entry;
    }

    HashResult secondary = HashSecondary(primary);

    int trials = 0;

    while ((entry = EmplaceEntry(primary, secondary, key, hasher,
                                 std::forward<Args>(args)...)) == nullptr) {
      if (++trials >= 3) {
        LOG_FIRST_N(WARNING, 1)
            << "CuckooMap: Excessive hash colision detected:\n"
            << bess::debug::DumpStack();
        return nullptr;
      }

      // expand the table as the last resort
      ExpandBuckets<std::conditional_t<std::is_move_constructible<V>::value,
                                       V&&, const V&>>(hasher, eq);
    }
    return entry;
  }

  // Insert/update a key value pair
  // On success returns a pointer to the inserted entry, nullptr otherwise.
  // NOTE: when Insert() returns nullptr, the copy/move constructor of `V` may
  // not be called.
  Entry* Insert(const K& key, const V& value, const H& hasher = H(),
                const E& eq = E()) {
    return DoEmplace(key, hasher, eq, value);
  }

  Entry* Insert(const K& key, V&& value, const H& hasher = H(),
                const E& eq = E()) {
    return DoEmplace(key, hasher, eq, std::move(value));
  }

  // Emplace/update-in-place a key value pair
  // On success returns a pointer to the inserted entry, nullptr otherwise.
  // NOTE: when Emplace() returns nullptr, the constructor of `V` may not be
  // called.
  template <typename... Args>
  Entry* Emplace(const K& key, Args&&... args) {
    return DoEmplace(key, H(), E(), std::forward<Args>(args)...);
  }

  // Find the pointer to the stored value by the key.
  // Return nullptr if not exist.
  Entry* Find(const K& key, const H& hasher = H(), const E& eq = E()) {
    // Blame Effective C++ for this
    return const_cast<Entry*>(
        static_cast<
            const typename std::remove_reference<decltype(*this)>::type&>(*this)
            .Find(key, hasher, eq));
  }

  // const version of Find()
  const Entry* Find(const K& key, const H& hasher = H(),
                    const E& eq = E()) const {
    EntryIndex idx = FindWithHash(Hash(key, hasher), key, eq);
    if (idx == kInvalidEntryIdx) {
      return nullptr;
    }

    const Entry* ret = &entries_[idx];
    promise(ret != nullptr);
    return ret;
  }

  // Heterogeneous lookup with a probe type distinct from the stored key.
  template <typename Probe, typename ProbeHash, typename StoredProbeEqual>
  const Entry* FindAs(const Probe& probe, const ProbeHash& hasher,
                      const StoredProbeEqual& eq) const {
    return FindPrehashedAs(static_cast<HashResult>(hasher(probe)), probe, eq);
  }

  // Heterogeneous lookup when the caller already computed the raw hash.
  template <typename Probe, typename StoredProbeEqual>
  const Entry* FindPrehashedAs(HashResult hash, const Probe& probe,
                               const StoredProbeEqual& eq) const {
    EntryIndex idx = FindWithHash(NormalizeHash(hash), probe, eq);
    if (idx == kInvalidEntryIdx) {
      return nullptr;
    }
    return &entries_[idx];
  }

  // Batch pipelining hooks: a batch lookup can hash every key and prefetch its
  // primary bucket, then (once buckets are cached) prefetch each candidate
  // entry, before probing -- so one batch's cache misses overlap instead of
  // each key paying its dependent bucket -> entry misses in turn. Both are
  // hints only; skipping them changes nothing but speed.
  void PrefetchBucketPrehashed(HashResult hash) const {
    __builtin_prefetch(&buckets_[NormalizeHash(hash) & bucket_mask_], 0, 3);
  }
  // (utils/ stays free of dataplane/ headers, hence the raw builtin here.)

  void PrefetchEntryPrehashed(HashResult hash) const {
    const HashResult primary = NormalizeHash(hash);
    const Bucket& bucket = buckets_[primary & bucket_mask_];
    for (int i = 0; i < kEntriesPerBucket; i++) {
      if (bucket.hash_values[i] == primary) {
        __builtin_prefetch(&entries_[bucket.entry_indices[i]], 0, 3);
        return;
      }
    }
  }

  // Instrumented heterogeneous lookup for benchmark diagnostics.
  template <typename Probe, typename StoredProbeEqual>
  const Entry* FindPrehashedAsWithStats(HashResult hash, const Probe& probe,
                                         const StoredProbeEqual& eq,
                                         LookupStats& stats) const {
    EntryIndex idx =
        FindWithHashStats(NormalizeHash(hash), probe, eq, stats);
    if (idx == kInvalidEntryIdx) {
      return nullptr;
    }
    return &entries_[idx];
  }

  // Remove the stored entry by the key
  // Return false if not exist.
  bool Remove(const K& key, const H& hasher = H(), const E& eq = E()) {
    HashResult pri = Hash(key, hasher);
    if (RemoveFromBucket(pri, pri & bucket_mask_, key, eq)) {
      return true;
    }
    HashResult sec = HashSecondary(pri);
    if (RemoveFromBucket(pri, sec & bucket_mask_, key, eq)) {
      return true;
    }
    return false;
  }

  void Clear() {
    buckets_.clear();
    entries_.clear();

    // std::stack doesn't have a clear() method. Strange.
    while (!free_entry_indices_.empty()) {
      free_entry_indices_.pop();
    }

    num_entries_ = 0;
    bucket_mask_ = kInitNumBucket - 1;
    buckets_.resize(kInitNumBucket);
    entries_.resize(kInitNumEntries);

    for (int i = kInitNumEntries - 1; i >= 0; --i) {
      free_entry_indices_.push(i);
    }
  }

  // Return the number of stored entries
  size_t Count() const { return num_entries_; }

  // Warms the primary bucket of every key in a batch, when the table is big
  // enough for that to pay (dataplane::ResolveLookupBody: beyond L1d; a probe
  // walks bucket -> entry and branches on which slot matched). A plain loop of
  // Find() calls after it then finds its buckets in cache. Does nothing for a
  // small table. It only prefetches, so it stays correct however the table
  // changes afterwards -- including inserts that reallocate.
  template <typename KeyRange>
  void PrefetchBatch(const KeyRange& keys, const H& hasher = H()) const {
    const bess::dataplane::LookupBody body = bess::dataplane::ResolveLookupBody(
        bess::dataplane::LookupBody::kAuto,
        {.table_bytes = MemoryBytes(),
         .dependent_lines = 2,
         .branches_on_loaded_data = true});
    if (body != bess::dataplane::LookupBody::kStaged) {
      return;
    }
    for (const K& key : keys) {
      PrefetchBucketPrehashed(static_cast<HashResult>(hasher(key)));
    }
  }

  // Bytes of bucket and entry storage: what a lookup's misses spread over.
  size_t MemoryBytes() const {
    return buckets_.size() * sizeof(Bucket) + entries_.size() * sizeof(Entry);
  }

 protected:
  // Tunable macros
  static const int kInitNumBucket = 4;
  static const int kInitNumEntries = 16;
  static const int kEntriesPerBucket = 4;  // 4-way set associative

  // 4^kMaxCuckooPath buckets will be considered to make a empty slot,
  // before giving up and expand the table.
  // Higher number will yield better occupancy, but the worst case performance
  // of insertion will grow exponentially, so be careful.
  static const int kMaxCuckooPath = 3;

  /* non-tunable macros */
  static const EntryIndex kInvalidEntryIdx =
      std::numeric_limits<EntryIndex>::max();

  struct Bucket {
    HashResult hash_values[kEntriesPerBucket];
    EntryIndex entry_indices[kEntriesPerBucket];

    Bucket() : hash_values(), entry_indices() {}
  };

  // Push an unused entry index back to the  stack
  void PushFreeEntryIndex(EntryIndex idx) { free_entry_indices_.push(idx); }

  // Pop a free entry index from stack and return the index
  EntryIndex PopFreeEntryIndex() {
    if (free_entry_indices_.empty()) {
      ExpandEntries();
    }
    size_t idx = free_entry_indices_.top();
    free_entry_indices_.pop();
    return idx;
  }

  // Try to add (key, value) to the bucket indexed by bucket_idx
  // Return the pointer to the entry if success. Otherwise return nullptr.
  template <typename... Args>
  Entry* EmplaceInBucket(HashResult bucket_idx, const K& key, const H& hasher,
                         Args&&... args) {
    Bucket& bucket = buckets_[bucket_idx];
    int slot_idx = FindEmptySlot(bucket);
    if (slot_idx == -1) {
      return nullptr;
    }

    EntryIndex free_idx = PopFreeEntryIndex();

    bucket.hash_values[slot_idx] = Hash(key, hasher);
    bucket.entry_indices[slot_idx] = free_idx;

    Entry& entry = entries_[free_idx];
    entry.first = key;
    new (&entry.second) V(std::forward<Args>(args)...);

    num_entries_++;
    return &entry;
  }

  // Remove key from the bucket indexed by bucket_idx
  // Return true if success.
  bool RemoveFromBucket(HashResult primary, HashResult bucket_idx, const K& key,
                        const E& eq) {
    Bucket& bucket = buckets_[bucket_idx];

    int slot_idx = FindSlot(bucket, primary, key, eq);
    if (slot_idx == -1) {
      return false;
    }

    bucket.hash_values[slot_idx] = 0;

    EntryIndex idx = bucket.entry_indices[slot_idx];
    entries_[idx] = Entry();
    PushFreeEntryIndex(idx);

    num_entries_--;
    return true;
  }

  // Find key from the bucket indexed by bucket_idx
  // Return the index of the entry if success. Otherwise return nullptr.
  template <typename Probe, typename StoredProbeEqual>
  EntryIndex GetFromBucket(HashResult primary, HashResult bucket_idx,
                           const Probe& probe,
                           const StoredProbeEqual& eq) const {
    const Bucket& bucket = buckets_[bucket_idx];

    int slot_idx = FindSlot(bucket, primary, probe, eq);
    if (slot_idx == -1) {
      return kInvalidEntryIdx;
    }

    // this promise gains 5% performance improvement
    EntryIndex idx = bucket.entry_indices[slot_idx];
    promise(idx != kInvalidEntryIdx);
    return idx;
  }

  // Try to add the entry (key, value)
  // Return the pointer to the entry if success. Otherwise return nullptr.
  template <typename... Args>
  Entry* EmplaceEntry(HashResult primary, HashResult secondary, const K& key,
                      const H& hasher, Args&&... args) {
    HashResult primary_bucket_index, secondary_bucket_index;
    Entry* entry = nullptr;
  again:
    primary_bucket_index = primary & bucket_mask_;
    if ((entry = EmplaceInBucket(primary_bucket_index, key, hasher,
                                 std::forward<Args>(args)...)) != nullptr) {
      return entry;
    }

    secondary_bucket_index = secondary & bucket_mask_;
    if ((entry = EmplaceInBucket(secondary_bucket_index, key, hasher,
                                 std::forward<Args>(args)...)) != nullptr) {
      return entry;
    }

    if (MakeSpace(primary_bucket_index, 0, hasher) >= 0) {
      goto again;
    }

    if (MakeSpace(secondary_bucket_index, 0, hasher) >= 0) {
      goto again;
    }

    return nullptr;
  }

  // Return an empty slot index in the bucket
  int FindEmptySlot(const Bucket& bucket) const {
    for (int i = 0; i < kEntriesPerBucket; i++) {
      if (bucket.hash_values[i] == 0) {
        return i;
      }
    }
    return -1;
  }

  // Return the slot index in the bucket that matches the primary hash_value
  // and the actual probe. Return -1 if not found.
  template <typename Probe, typename StoredProbeEqual>
  int FindSlot(const Bucket& bucket, HashResult primary, const Probe& probe,
               const StoredProbeEqual& eq) const {
    for (int i = 0; i < kEntriesPerBucket; i++) {
      if (bucket.hash_values[i] == primary) {
        EntryIndex idx = bucket.entry_indices[i];
        const Entry& entry = entries_[idx];

        if (likely(Eq(entry.first, probe, eq))) {
          return i;
        }
      }
    }
    return -1;
  }

  // Recursively try making an empty slot in the bucket
  // Returns a slot index in [0, kEntriesPerBucket) for successful operation,
  // or -1 if failed.
  int MakeSpace(HashResult index, int depth, const H& hasher) {
    if (depth >= kMaxCuckooPath) {
      return -1;
    }

    Bucket& bucket = buckets_[index];

    for (int i = 0; i < kEntriesPerBucket; i++) {
      EntryIndex idx = bucket.entry_indices[i];
      const K& key = entries_[idx].first;
      HashResult pri = Hash(key, hasher);
      HashResult sec = HashSecondary(pri);

      HashResult alt_index;

      // this entry is in its primary bucket?
      if (pri == bucket.hash_values[i]) {
        alt_index = sec & bucket_mask_;
      } else if (sec == bucket.hash_values[i]) {
        alt_index = pri & bucket_mask_;
      } else {
        return -1;
      }

      int j = FindEmptySlot(buckets_[alt_index]);
      if (j == -1) {
        j = MakeSpace(alt_index, depth + 1, hasher);
      }
      if (j >= 0) {
        Bucket& alt_bucket = buckets_[alt_index];
        alt_bucket.hash_values[j] = bucket.hash_values[i];
        alt_bucket.entry_indices[j] = bucket.entry_indices[i];
        bucket.hash_values[i] = 0;
        return i;
      }
    }

    return -1;
  }

  // Get the entry given the primary hash value of the key.
  // Returns the pointer to the entry or nullptr if failed.
  template <typename Probe, typename StoredProbeEqual>
  EntryIndex FindWithHash(HashResult primary, const Probe& probe,
                          const StoredProbeEqual& eq) const {
    EntryIndex ret =
        GetFromBucket(primary, primary & bucket_mask_, probe, eq);
    if (ret != kInvalidEntryIdx) {
      return ret;
    }
    return GetFromBucket(primary, HashSecondary(primary) & bucket_mask_, probe,
                         eq);
  }

  template <typename Probe, typename StoredProbeEqual>
  EntryIndex FindWithHashStats(HashResult primary, const Probe& probe,
                               const StoredProbeEqual& eq,
                               LookupStats& stats) const {
    EntryIndex ret =
        GetFromBucket(primary, primary & bucket_mask_, probe, eq);
    if (ret != kInvalidEntryIdx) {
      stats.primary_hits++;
      return ret;
    }
    ret = GetFromBucket(primary, HashSecondary(primary) & bucket_mask_, probe,
                        eq);
    if (ret != kInvalidEntryIdx) {
      stats.secondary_hits++;
    } else {
      stats.misses++;
    }
    return ret;
  }

  // Secondary hash value
  static HashResult HashSecondary(HashResult primary) {
    HashResult tag = primary >> 12;
    return primary ^ ((tag + 1) * 0x5bd1e995);
  }

  static HashResult NormalizeHash(HashResult hash) {
    return hash | (1u << 31);
  }

  // Primary hash value. Should always be non-zero (= not empty)
  template <typename Probe, typename Hasher>
  static HashResult Hash(const Probe& probe, const Hasher& hasher) {
    return NormalizeHash(static_cast<HashResult>(hasher(probe)));
  }

  template <typename Stored, typename Probe, typename Equal>
  static bool Eq(const Stored& lhs, const Probe& rhs, const Equal& eq) {
    return eq(lhs, rhs);
  }

  // Resize the space of entries. Grow less aggressively than buckets.
  void ExpandEntries() {
    size_t old_size = entries_.size();
    size_t new_size = old_size + old_size / 2;

    entries_.resize(new_size);

    for (EntryIndex i = new_size - 1; i >= old_size; --i) {
      free_entry_indices_.push(i);
    }
  }

  // Resize the space of buckets, and rehash existing entries
  template <typename VV>
  void ExpandBuckets(const H& hasher, const E& eq) {
    CuckooMap<K, V, H, E> bigger(buckets_.size() * 2, entries_.size());

    for (auto& e : *this) {
      // While very unlikely, this DoEmplace() may cause recursive expansion
      Entry* ret =
          bigger.DoEmplace(e.first, hasher, eq, std::forward<VV>(e.second));
      if (!ret) {
        return;
      }
    }

    bucket_mask_ = std::move(bigger.bucket_mask_);
    num_entries_ = bigger.num_entries_;
    buckets_ = std::move(bigger.buckets_);
    entries_ = std::move(bigger.entries_);
    free_entry_indices_ = std::move(bigger.free_entry_indices_);
  }

  // # of buckets == mask + 1
  HashResult bucket_mask_;

  // # of entries
  size_t num_entries_;

  // bucket and entry arrays grow independently
  std::vector<Bucket> buckets_;
  std::vector<Entry> entries_;

  // Stack of free entries
  std::stack<EntryIndex> free_entry_indices_;
};

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_CUCKOOMAP_H_
