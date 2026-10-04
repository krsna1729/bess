// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_L2_MAC_TABLE_H_
#define BESS_L2_MAC_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

#include <sys/mman.h>

#include "dataplane/table_policy.h"
#include "utils/common.h"

namespace bess::l2 {

// A worker-owned exact-match table from a 64-bit key word to a 16-bit value
// plus an 8-bit flags field, specialised for the FDB (M14, D-064).
//
// A bucket is one cache line: four keys side by side (one vector compare
// probes them), then their values and flags. A hit costs one line, two when
// the key sits in its alternate bucket. Two-choice cuckoo hashing; an insert
// into two full buckets moves resident entries along a breadth-first path.
// Sized for at most 50% load, where that search practically always succeeds.
//
// kEmptyKey (all ones) marks a free slot and is never a valid key. A free
// slot's value and flags are 0 (Create zeroes them, Erase clears them), so a
// lookup of kEmptyKey -- which matches a free slot -- reads value 0.
//
// Each slot has a cold companion (`Cold`) that moves with it, off the lookup
// path; the FDB keeps the entry's aging timer there.
//
// Slots are named by index (bucket * kWays + way). One thread owns the table:
// moves are not ordered for concurrent readers. Allocation happens only in
// Create.
template <typename Cold>
class MacTable {
  static_assert(std::is_trivially_copyable_v<Cold> && std::is_trivially_destructible_v<Cold>);

 public:
  static constexpr size_t kWays = 4;
  static constexpr size_t kMaxBatch = 64;
  static constexpr uint64_t kEmptyKey = ~uint64_t{0};
  static constexpr uint32_t kNotFound = ~uint32_t{0};
  static constexpr size_t kHugePage = size_t{2} << 20;
  // The whole 64-bit word is the key (the FDB packs domain and MAC into it),
  // and a value is 16 bits.
  static constexpr size_t kMaxDomains = 65535;
  static constexpr uint32_t kMaxValue = 0xFFFF;

  using writers = dataplane::OwnerWrites;
  using readers = dataplane::OwnerReads;
  using growth = dataplane::Fixed;
  using Guard = dataplane::NoLock;
  Guard Lock() noexcept { return {}; }

  static std::unique_ptr<MacTable> Create(size_t capacity) {
    if (capacity == 0 || capacity > (size_t{1} << 28)) {
      return nullptr;
    }
    size_t buckets = 2;
    while (buckets * kWays < 2 * capacity) {
      buckets <<= 1;
    }
    std::unique_ptr<MacTable> t(new (std::nothrow) MacTable(capacity, buckets));
    if (t == nullptr) {
      return nullptr;
    }
    // A large table is 2 MiB-aligned and asks for transparent huge pages: a
    // lookup reads one random line, and with 4 KiB pages it also misses the
    // TLB.
    const size_t bytes = buckets * sizeof(Bucket);
    const size_t align = bytes >= kHugePage ? kHugePage : 64;
    t->buckets_ = static_cast<Bucket *>(
        std::aligned_alloc(align, (bytes + align - 1) & ~(align - 1)));
    t->cold_ = static_cast<Cold *>(std::malloc(buckets * kWays * sizeof(Cold)));
    if (t->buckets_ == nullptr || t->cold_ == nullptr) {
      return nullptr;
    }
    if (align == kHugePage) {
      (void)madvise(t->buckets_, bytes, MADV_HUGEPAGE);  // advisory
    }
    std::memset(static_cast<void *>(t->buckets_), 0, bytes);
    for (size_t b = 0; b < buckets; b++) {
      for (uint64_t &k : t->buckets_[b].keys) {
        k = kEmptyKey;
      }
    }
    for (size_t i = 0; i < buckets * kWays; i++) {
      new (&t->cold_[i]) Cold{};
    }
    return t;
  }

  ~MacTable() {
    std::free(buckets_);
    std::free(cold_);
  }
  MacTable(const MacTable &) = delete;
  MacTable &operator=(const MacTable &) = delete;

  // The slot holding `key`, or kNotFound. `key` must not be kEmptyKey.
  uint32_t Find(uint64_t key) const noexcept {
    const uint64_t h = Hash(key);
    const size_t b1 = h & mask_;
    const uint32_t s = Probe(b1, key);
    return s != kNotFound ? s : Probe(Alt(b1, h), key);
  }

  // Prefetches both candidate buckets of every key, then probes: one batch
  // overlaps its cache misses.
  void FindBatch(std::span<const uint64_t> keys, uint32_t *out) const noexcept {
    promise(keys.size() <= kMaxBatch);
    uint64_t hashes[kMaxBatch];
    for (size_t i = 0; i < keys.size(); i++) {
      const uint64_t h = Hash(keys[i]);
      hashes[i] = h;
      __builtin_prefetch(&buckets_[h & mask_]);
      __builtin_prefetch(&buckets_[Alt(h & mask_, h)]);
    }
    for (size_t i = 0; i < keys.size(); i++) {
      const size_t b1 = hashes[i] & mask_;
      const uint32_t s = Probe(b1, keys[i]);
      out[i] = s != kNotFound ? s : Probe(Alt(b1, hashes[i]), keys[i]);
    }
  }

  // The value for `key`, 0 for a miss (a free slot reads value 0).
  uint32_t Lookup(uint64_t key) const noexcept {
    const uint32_t s = Find(key);
    return s == kNotFound ? 0 : value(s);
  }

  // values[i] = the value for keys[i], 0 for a miss; bit i set for a hit.
  uint64_t LookupBatch(std::span<const uint64_t> keys, uint16_t *values) const noexcept {
    uint32_t slots[kMaxBatch];
    FindBatch(keys, slots);
    uint64_t hits = 0;
    for (size_t i = 0; i < keys.size(); i++) {
      const uint16_t v = slots[i] != kNotFound ? value(slots[i]) : 0;
      hits |= uint64_t{v != 0} << i;
      values[i] = v;
    }
    return hits;
  }

  // A slot for a new key with `value` and `flags`, cold Cold{} -- or
  // kNotFound if the table holds `capacity` entries or no free slot is
  // reachable. The key must be absent and not kEmptyKey. Inserting may move
  // other entries (their slot indices change).
  uint32_t Insert(uint64_t key, uint16_t value = 0, uint8_t flags = 0) noexcept {
    if (size_ >= capacity_) {
      return kNotFound;
    }
    const uint64_t h = Hash(key);
    const size_t b1 = h & mask_;
    const size_t b2 = Alt(b1, h);
    uint32_t s = FreeIn(b1);
    if (s == kNotFound) {
      s = FreeIn(b2);
    }
    if (s == kNotFound) {
      s = MakeRoom(b1, b2);
    }
    if (s == kNotFound) {
      return kNotFound;
    }
    Bucket &b = buckets_[s / kWays];
    b.keys[s % kWays] = key;
    b.values[s % kWays] = value;
    b.flags[s % kWays] = flags;
    cold_[s] = Cold{};
    size_++;
    return s;
  }

  // A free slot reads as value 0, flags 0.
  void Erase(uint32_t s) noexcept {
    buckets_[s / kWays].keys[s % kWays] = kEmptyKey;
    buckets_[s / kWays].values[s % kWays] = 0;
    buckets_[s / kWays].flags[s % kWays] = 0;
    cold_[s] = Cold{};
    size_--;
  }

  uint64_t key(uint32_t s) const noexcept { return buckets_[s / kWays].keys[s % kWays]; }
  uint16_t value(uint32_t s) const noexcept { return buckets_[s / kWays].values[s % kWays]; }
  void set_value(uint32_t s, uint16_t v) noexcept { buckets_[s / kWays].values[s % kWays] = v; }
  uint8_t flags(uint32_t s) const noexcept { return buckets_[s / kWays].flags[s % kWays]; }
  void set_flags(uint32_t s, uint8_t f) noexcept { buckets_[s / kWays].flags[s % kWays] = f; }
  Cold &cold(uint32_t s) noexcept { return cold_[s]; }

  // fn(slot) for every occupied slot. fn may Erase that slot (never Insert).
  template <typename Fn>
  void ForEach(Fn &&fn) noexcept {
    for (uint32_t s = 0; s < nbuckets_ * kWays; s++) {
      if (key(s) != kEmptyKey) {
        fn(s);
      }
    }
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  size_t memory_bytes() const noexcept {
    return nbuckets_ * (sizeof(Bucket) + kWays * sizeof(Cold)) + sizeof(*this);
  }

 private:
  struct alignas(64) Bucket {
    uint64_t keys[kWays];
    uint16_t values[kWays];
    uint8_t flags[kWays];
  };
  static_assert(sizeof(Bucket) == 64);

  MacTable(size_t capacity, size_t buckets)
      : capacity_(capacity), nbuckets_(buckets), mask_(buckets - 1) {}

  // splitmix64's finaliser: both 32-bit halves are independent enough to pick
  // two buckets. (A CRC is linear -- crc(seed, k) = crc(0, k) ^ c -- so two
  // CRCs give every key of a bucket the same alternate: measured, inserts
  // failed at 48% load.)
  static uint64_t Hash(uint64_t key) noexcept {
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ull;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebull;
    return key ^ (key >> 31);
  }
  // The alternate bucket; an involution (Alt(Alt(b)) == b), so a resident
  // entry's other bucket follows from its key and its present bucket.
  size_t Alt(size_t b, uint64_t h) const noexcept {
    return (b ^ ((h >> 32) | 1)) & mask_;
  }

  // Branch-free over the ways (the matching way is random; an early-exit loop
  // mispredicts on it) and over contiguous keys, so it compiles to a vector
  // compare.
  uint32_t Probe(size_t b, uint64_t key) const noexcept {
    const uint64_t *keys = buckets_[b].keys;
    unsigned match = 0;
    for (size_t w = 0; w < kWays; w++) {
      match |= static_cast<unsigned>(keys[w] == key) << w;
    }
    return match == 0 ? kNotFound
                      : static_cast<uint32_t>(b * kWays + __builtin_ctz(match));
  }
  uint32_t FreeIn(size_t b) const noexcept { return Probe(b, kEmptyKey); }

  void Move(uint32_t to, uint32_t from) noexcept {
    Bucket &d = buckets_[to / kWays];
    const Bucket &f = buckets_[from / kWays];
    d.keys[to % kWays] = f.keys[from % kWays];
    d.values[to % kWays] = f.values[from % kWays];
    d.flags[to % kWays] = f.flags[from % kWays];
    cold_[to] = cold_[from];
  }

  // Breadth-first search from the two full buckets for a bucket with a free
  // slot, then moves entries back along the path; returns the freed slot in
  // b1 or b2, or kNotFound if none lies within kMaxNodes buckets.
  uint32_t MakeRoom(size_t b1, size_t b2) noexcept {
    static constexpr size_t kMaxNodes = 256;
    struct Node {
      uint32_t bucket;
      int16_t parent;  // node index, -1 for a root
      uint8_t way;     // the way in the parent bucket whose entry moves here
    };
    Node nodes[kMaxNodes];
    size_t head = 0, tail = 0;
    nodes[tail++] = {static_cast<uint32_t>(b1), -1, 0};
    nodes[tail++] = {static_cast<uint32_t>(b2), -1, 0};
    while (head < tail) {
      const size_t at = head++;
      const size_t b = nodes[at].bucket;
      for (uint8_t w = 0; w < kWays; w++) {
        const size_t other = Alt(b, Hash(buckets_[b].keys[w]));
        const uint32_t free = FreeIn(other);
        if (free != kNotFound) {
          // This entry to `free`, then each ancestor's chosen entry into the
          // slot its child vacated.
          uint32_t dst = free;
          size_t node = at;
          uint8_t way = w;
          for (;;) {
            const uint32_t src = static_cast<uint32_t>(nodes[node].bucket * kWays + way);
            Move(dst, src);
            dst = src;
            if (nodes[node].parent < 0) {
              // Insert fills it at once.
              buckets_[dst / kWays].keys[dst % kWays] = kEmptyKey;
              return dst;
            }
            way = nodes[node].way;
            node = static_cast<size_t>(nodes[node].parent);
          }
        }
        // A path through one bucket twice would move one entry twice.
        bool on_path = false;
        for (int n = static_cast<int>(at); n >= 0; n = nodes[n].parent) {
          on_path |= nodes[n].bucket == other;
        }
        if (!on_path && tail < kMaxNodes) {
          nodes[tail++] = {static_cast<uint32_t>(other), static_cast<int16_t>(at), w};
        }
      }
    }
    return kNotFound;
  }

  size_t capacity_;
  size_t nbuckets_;
  size_t mask_;
  size_t size_ = 0;
  Bucket *buckets_ = nullptr;
  Cold *cold_ = nullptr;
};

}  // namespace bess::l2

#endif  // BESS_L2_MAC_TABLE_H_
