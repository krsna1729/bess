// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_L2_PACKED_MAC_TABLE_H_
#define BESS_L2_PACKED_MAC_TABLE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(BESS_PACKED_MAC_TABLE_TESTING)
#include <functional>
#endif
#include <memory>
#include <new>
#include <optional>
#include <chrono>
#include <random>
#include <span>
#include <type_traits>

#include <sys/mman.h>

#include "arch/cpu.h"
#include "arch/crc32c.h"
#include "arch/word_probe.h"
#include "dataplane/table_policy.h"
#include "utils/common.h"

namespace bess::l2 {

// Test hooks (BESS_PACKED_MAC_TABLE_TESTING, defined only by the table's own
// test): a callback between a move's two stores, one between a reader's two
// probes, and the bucket and move primitives the deterministic tests drive.
// Production code compiles without them.
#if defined(BESS_PACKED_MAC_TABLE_TESTING)
#define BESS_PMT_HOOK(hook, ...) \
  do {                           \
    if ((hook) != nullptr) {     \
      (hook)(__VA_ARGS__);       \
    }                            \
  } while (0)
#else
#define BESS_PMT_HOOK(hook, ...) ((void)0)
#endif

// A MAC table whose slot is one 64-bit word -- MAC (48 bits), value (14), a
// flag bit and an occupied bit -- so a reader on another thread sees a slot
// whole or not at all (table policy, user decision 1; l2_table's layout,
// D-017, with the FDB's learning on top).
//
// Keys are the FDB's key words for domain 0 (MakeKey: domain in the low 16
// bits, the MAC above): one bridge domain per table (kMaxDomains 1).
//
// Two-choice cuckoo, 4 slots (32 bytes) a bucket, at most 50% load, breadth-
// first move search on insert (as MacTable). Concurrency, by `Sync`:
// - OwnerWrites: one thread reads and writes; plain stores.
// - SingleWriter: one writer at a time (the caller serialises), lock-free
//   readers on any thread.
// - MultiWriter: SingleWriter plus the table's own spinlock (Lock/TryLock).
// Readers on other threads: every slot write is one release store of the
// whole word; a move writes the destination before clearing the source; and
// because an entry can move from its alternate bucket into its primary while
// a reader is between the two probes, a writer marks a move path with an odd
// sequence number. A reader re-checks the sequence only on a miss and retries
// if it changed, so a hit costs nothing extra.
//
// Each slot has a cold companion (`Cold`, the FDB's aging timer) that only the
// writer reads; it moves with its slot. Allocation happens only in Create.
template <typename Cold, typename Sync = dataplane::OwnerWrites>
class PackedMacTable {
  static_assert(std::is_trivially_copyable_v<Cold> && std::is_trivially_destructible_v<Cold>);
  static_assert(std::is_same_v<Sync, dataplane::OwnerWrites> ||
                std::is_same_v<Sync, dataplane::SingleWriter> ||
                std::is_same_v<Sync, dataplane::MultiWriter>);
  static constexpr bool kShared = !std::is_same_v<Sync, dataplane::OwnerWrites>;

 public:
  static constexpr size_t kWays = 4;
  static constexpr size_t kMaxBatch = 64;
  static constexpr uint32_t kNotFound = ~uint32_t{0};
  static constexpr size_t kMaxDomains = 1;
  static constexpr uint32_t kMaxValue = (1u << 14) - 1;
  static constexpr size_t kHugePage = size_t{2} << 20;
  // A reader's retries after a miss while moves run. The writer's move path is
  // short, but a writer that is preempted inside one must not stall readers:
  // past this many the miss stands (for L2Forward: the default gate, once).
  static constexpr unsigned kMaxRetries = 64;
  // An empty Cold costs nothing: no cold array.
  static constexpr bool kHasCold = !std::is_empty_v<Cold>;

  using writers = Sync;
  using readers =
      std::conditional_t<kShared, dataplane::AnyReader, dataplane::OwnerReads>;
  using growth = dataplane::Fixed;

  // The writer guard: empty unless the table has its own lock.
  class [[nodiscard]] LockGuard {
   public:
    explicit LockGuard(std::atomic<bool> *lock) noexcept : lock_(lock) {}
    LockGuard(LockGuard &&o) noexcept : lock_(o.lock_) { o.lock_ = nullptr; }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;
    LockGuard &operator=(LockGuard &&) = delete;
    ~LockGuard() {
      if (lock_ != nullptr) {
        lock_->store(false, std::memory_order_release);
      }
    }

   private:
    std::atomic<bool> *lock_;
  };
  using Guard = std::conditional_t<std::is_same_v<Sync, dataplane::MultiWriter>, LockGuard,
                                   dataplane::NoLock>;

  Guard Lock() noexcept {
    if constexpr (std::is_same_v<Sync, dataplane::MultiWriter>) {
      while (lock_.exchange(true, std::memory_order_acquire)) {
        while (lock_.load(std::memory_order_relaxed)) {
          arch::CpuRelax();
        }
      }
      return LockGuard(&lock_);
    } else {
      return {};
    }
  }
  // Never waits: empty if another writer holds the lock (packet-path learning
  // skips the learn; table policy decision 2).
  std::optional<Guard> TryLock() noexcept {
    if constexpr (std::is_same_v<Sync, dataplane::MultiWriter>) {
      if (lock_.load(std::memory_order_relaxed) ||
          lock_.exchange(true, std::memory_order_acquire)) {
        return std::nullopt;
      }
      return LockGuard(&lock_);
    } else {
      return Guard{};
    }
  }

  static std::unique_ptr<PackedMacTable> Create(size_t capacity) {
    if (capacity == 0 || capacity > (size_t{1} << 28)) {
      return nullptr;
    }
    size_t buckets = 2;
    while (buckets * kWays < 2 * capacity) {
      buckets <<= 1;
    }
    std::unique_ptr<PackedMacTable> t(
        new (std::nothrow) PackedMacTable(capacity, buckets, NewSeed()));
    if (t == nullptr) {
      return nullptr;
    }
    const size_t bytes = buckets * kWays * sizeof(uint64_t);
    const size_t align = bytes >= kHugePage ? kHugePage : 64;
    t->slots_ = static_cast<uint64_t *>(std::aligned_alloc(align, (bytes + align - 1) & ~(align - 1)));
    if constexpr (kHasCold) {
      t->cold_ = static_cast<Cold *>(std::malloc(buckets * kWays * sizeof(Cold)));
    }
    if (t->slots_ == nullptr || (kHasCold && t->cold_ == nullptr)) {
      return nullptr;
    }
    if (align == kHugePage) {
      (void)madvise(t->slots_, bytes, MADV_HUGEPAGE);  // advisory
    }
    std::memset(static_cast<void *>(t->slots_), 0, bytes);
    if constexpr (kHasCold) {
      for (size_t i = 0; i < buckets * kWays; i++) {
        new (&t->cold_[i]) Cold{};
      }
    }
    return t;
  }

  ~PackedMacTable() {
    std::free(slots_);
    std::free(cold_);  // null without a cold array
  }
  PackedMacTable(const PackedMacTable &) = delete;
  PackedMacTable &operator=(const PackedMacTable &) = delete;

  // -- readers (any thread when shared) ----------------------------------------

  // The value stored for `key`, 0 for a miss. Key words with a nonzero domain
  // never match.
  uint32_t Lookup(uint64_t key) const noexcept {
    const uint64_t want = Want(key);
    const uint64_t h = Hash(key);
    for (unsigned attempt = 0;; attempt++) {
      const uint32_t seq = kShared ? moves_.load(std::memory_order_acquire) : 0;
      const size_t b1 = h & mask_;
      uint64_t word = ProbeWord(b1, want);
      if (word == 0) {
        BESS_PMT_HOOK(between_probes_hook, key);
        if constexpr (kShared) {
          // The alternate after the primary: a move writes its destination
          // before clearing its source.
          std::atomic_thread_fence(std::memory_order_acquire);
        }
        word = ProbeWord(Alt(b1, h), want);
      }
      if (word != 0) {
        return ValueOf(word);
      }
      if (!kShared || Stable(seq) || attempt == kMaxRetries) {
        return 0;
      }
      arch::CpuRelax();
    }
  }

  // values[i] = the value for keys[i], 0 for a miss; bit i set for a hit.
  // The move sequence is read once for the batch and checked once after it,
  // only if something missed; an unstable batch looks its misses up again.
  template <typename V = uint16_t>
  uint64_t LookupBatch(std::span<const uint64_t> keys, V *values) const noexcept {
    promise(keys.size() <= kMaxBatch);
    const uint32_t seq = kShared ? moves_.load(std::memory_order_acquire) : 0;
    uint64_t hashes[kMaxBatch];
    for (size_t i = 0; i < keys.size(); i++) {
      const uint64_t h = Hash(keys[i]);
      hashes[i] = h;
      __builtin_prefetch(&slots_[(h & mask_) * kWays]);
      __builtin_prefetch(&slots_[Alt(h & mask_, h) * kWays]);
    }
    uint64_t hits = 0;
    for (size_t i = 0; i < keys.size(); i++) {
      const uint64_t want = Want(keys[i]);
      const size_t b1 = hashes[i] & mask_;
      uint64_t word = ProbeWord(b1, want);
      if (word == 0) {
        BESS_PMT_HOOK(between_probes_hook, keys[i]);
        if constexpr (kShared) {
          std::atomic_thread_fence(std::memory_order_acquire);
        }
        word = ProbeWord(Alt(b1, hashes[i]), want);
      }
      values[i] = V(static_cast<uint16_t>(ValueOf(word)));
      hits |= uint64_t{word != 0} << i;
    }
    if constexpr (kShared) {
      const uint64_t all = keys.size() == 64 ? ~uint64_t{0} : (uint64_t{1} << keys.size()) - 1;
      uint64_t misses = ~hits & all;
      if (misses != 0 && !Stable(seq)) {  // rare: a move ran during the batch
        for (; misses != 0; misses &= misses - 1) {
          const unsigned i = static_cast<unsigned>(__builtin_ctzll(misses));
          const uint32_t v = Lookup(keys[i]);
          values[i] = V(static_cast<uint16_t>(v));
          hits |= uint64_t{v != 0} << i;
        }
      }
    }
    return hits;
  }

  // -- writer side (under the guard) ----------------------------------------------

  // The slot holding `key`, or kNotFound. Writer side only: slots move.
  uint32_t Find(uint64_t key) const noexcept {
    const uint64_t want = Want(key);
    const uint64_t h = Hash(key);
    const size_t b1 = h & mask_;
    const uint32_t s = ProbeSlot(b1, want);
    return s != kNotFound ? s : ProbeSlot(Alt(b1, h), want);
  }
  void FindBatch(std::span<const uint64_t> keys, uint32_t *out) const noexcept {
    for (size_t i = 0; i < keys.size(); i++) {
      out[i] = Find(keys[i]);
    }
  }

  // A slot for a new key (absent, domain 0) with `value` (1..kMaxValue) and
  // `flags` (0 or 1), or kNotFound if full or no free slot is reachable. May
  // move other entries.
  uint32_t Insert(uint64_t key, uint16_t value, uint8_t flags) noexcept {
    // Value 0 reads as a miss; a nonzero domain does not fit.
    if (size_ >= capacity_ || (key & 0xFFFF) != 0 || value == 0 || value > kMaxValue) {
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
    ResetCold(s);
    Store(s, Make(Mac(key), value, flags));
    size_++;
    return s;
  }

  void Erase(uint32_t s) noexcept {
    Store(s, 0);
    ResetCold(s);
    size_--;
  }

  uint64_t key(uint32_t s) const noexcept { return (Load(s) & kMacMask) << 16; }
  uint32_t value(uint32_t s) const noexcept { return ValueOf(Load(s)); }
  // 1..kMaxValue (0 would read as a miss).
  void set_value(uint32_t s, uint16_t v) noexcept {
    const uint64_t w = Load(s);
    Store(s, (w & ~kValueMask) | (uint64_t{v} << 48 & kValueMask));
  }
  uint8_t flags(uint32_t s) const noexcept { return static_cast<uint8_t>(Load(s) >> 62 & 1); }
  void set_flags(uint32_t s, uint8_t f) noexcept {
    const uint64_t w = Load(s);
    Store(s, (w & ~kFlagBit) | (f ? kFlagBit : 0));
  }
  Cold &cold(uint32_t s) noexcept
    requires kHasCold
  {
    return cold_[s];
  }

  // fn(slot) for every occupied slot. fn may Erase that slot (never Insert).
  template <typename Fn>
  void ForEach(Fn &&fn) noexcept {
    for (uint32_t s = 0; s < nbuckets_ * kWays; s++) {
      if (Load(s) & kOccupied) {
        fn(s);
      }
    }
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  size_t memory_bytes() const noexcept {
    return nbuckets_ * kWays * (sizeof(uint64_t) + (kHasCold ? sizeof(Cold) : 0)) + sizeof(*this);
  }

 private:
  static constexpr uint64_t kMacMask = (uint64_t{1} << 48) - 1;
  static constexpr uint64_t kValueMask = uint64_t{kMaxValue} << 48;
  static constexpr uint64_t kFlagBit = uint64_t{1} << 62;
  static constexpr uint64_t kOccupied = uint64_t{1} << 63;
  static constexpr uint64_t kKeyMask = kOccupied | kMacMask;

  PackedMacTable(size_t capacity, size_t buckets, uint64_t seed)
      : capacity_(capacity), nbuckets_(buckets), mask_(buckets - 1), seed_(seed) {}

  // Unpredictable without failing: random_device may throw where the system
  // has no entropy source; the clock is the fallback.
  // A random odd 64-bit multiplier (odd: a bijection of the key).
  static uint64_t NewSeed() noexcept {
    uint64_t seed;
    try {
      std::random_device device;
      seed = uint64_t{device()} << 32 | device();
    } catch (...) {
      seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) *
             0xBF58476D1CE4E5B9ull;
    }
    return seed | 1;
  }

  static uint64_t Mac(uint64_t key) noexcept { return key >> 16; }
  // What a slot holding `key` has under kKeyMask; a nonzero domain sets a bit
  // no slot has, so it never matches.
  static uint64_t Want(uint64_t key) noexcept {
    return kOccupied | Mac(key) | ((key & 0xFFFF) != 0 ? kFlagBit : 0);
  }
  static uint64_t Make(uint64_t mac, uint16_t value, uint8_t flags) noexcept {
    return kOccupied | (flags ? kFlagBit : 0) | (uint64_t{value} << 48 & kValueMask) | mac;
  }
  static uint32_t ValueOf(uint64_t word) noexcept {
    return static_cast<uint32_t>((word & kValueMask) >> 48);
  }

  // One CRC32C instruction, spread over 64 bits by one multiply: the low
  // bits (the bucket) are a bijection of the CRC's, the high half (the
  // alternate's tag) mixes all of them. A CRC alone is linear, which gives a
  // bucket's keys one alternate (MacTable's note); the product does not.
  // Cheaper than MacTable's splitmix64 (measured: scalar lookups, D-073).
  // The CRC is 32 bits for a 48-bit MAC, so MACs with one CRC share both
  // buckets, and a CRC is linear: such sets are easy to build, and a table
  // learning from untrusted senders (the Bridge) could be denied chosen MACs.
  // A seed in the CRC's initial value would not help (linearity keeps the
  // same keys colliding under every initial value); the key is first
  // multiplied by a random odd per-table constant, which is not linear, so
  // which MACs collide depends on a secret (review, 2026-10-04).
  uint64_t Hash(uint64_t key) const noexcept {
    return uint64_t{arch::Crc32c(key * seed_, 0)} * 0x9E3779B97F4A7C15ull;
  }
  // The alternate bucket; an involution (Alt(Alt(b)) == b), so a resident
  // entry's other bucket follows from its key and its present bucket.
  size_t Alt(size_t b, uint64_t h) const noexcept { return (b ^ ((h >> 32) | 1)) & mask_; }

  uint64_t Load(uint32_t s) const noexcept {
    if constexpr (kShared) {
      return __atomic_load_n(&slots_[s], __ATOMIC_RELAXED);
    } else {
      return slots_[s];
    }
  }
  void Store(uint32_t s, uint64_t word) noexcept {
    if constexpr (kShared) {
      __atomic_store_n(&slots_[s], word, __ATOMIC_RELEASE);
    } else {
      slots_[s] = word;
    }
  }

  // The occupied word in bucket b matching `want` (under kKeyMask), or 0.
  // Candidates from the vector filter are re-read as one word (D-017).
  uint64_t ProbeWord(size_t b, uint64_t want) const noexcept {
    const uint64_t *bucket = &slots_[b * kWays];
    for (unsigned m = arch::MaskedWordCandidates64x4(bucket, kKeyMask, want); m != 0; m &= m - 1) {
      const uint64_t word = Load(static_cast<uint32_t>(b * kWays + __builtin_ctz(m)));
      if ((word & kKeyMask) == want) {
        return word;
      }
    }
    return 0;
  }
  uint32_t ProbeSlot(size_t b, uint64_t want) const noexcept {
    for (size_t w = 0; w < kWays; w++) {
      if ((Load(static_cast<uint32_t>(b * kWays + w)) & kKeyMask) == want) {
        return static_cast<uint32_t>(b * kWays + w);
      }
    }
    return kNotFound;
  }
  uint32_t FreeIn(size_t b) const noexcept {
    for (size_t w = 0; w < kWays; w++) {
      if ((Load(static_cast<uint32_t>(b * kWays + w)) & kOccupied) == 0) {
        return static_cast<uint32_t>(b * kWays + w);
      }
    }
    return kNotFound;
  }

  bool Stable(uint32_t seq) const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    return (seq & 1) == 0 && moves_.load(std::memory_order_relaxed) == seq;
  }

  // Moves the entry in `from` to the free slot `to`: destination first.
  void ResetCold(uint32_t s) noexcept {
    if constexpr (kHasCold) {
      cold_[s] = Cold{};
    }
  }
  void Move(uint32_t to, uint32_t from) noexcept {
    if constexpr (kHasCold) {
      cold_[to] = cold_[from];
    }
    const uint64_t word = Load(from);
    Store(to, word);
    BESS_PMT_HOOK(mid_move_hook, word);
    Store(from, 0);
  }

  // As MacTable::MakeRoom; the path's moves are bracketed by an odd sequence.
  uint32_t MakeRoom(size_t b1, size_t b2) noexcept {
    static constexpr size_t kMaxNodes = 256;
    struct Node {
      uint32_t bucket;
      int16_t parent;
      uint8_t way;
    };
    Node nodes[kMaxNodes];
    size_t head = 0, tail = 0;
    nodes[tail++] = {static_cast<uint32_t>(b1), -1, 0};
    nodes[tail++] = {static_cast<uint32_t>(b2), -1, 0};
    while (head < tail) {
      const size_t at = head++;
      const size_t b = nodes[at].bucket;
      for (uint8_t w = 0; w < kWays; w++) {
        const uint64_t resident = Load(static_cast<uint32_t>(b * kWays + w));
        const size_t other = Alt(b, Hash((resident & kMacMask) << 16));
        const uint32_t free = FreeIn(other);
        if (free != kNotFound) {
          BeginMoves();
          uint32_t dst = free;
          size_t node = at;
          uint8_t way = w;
          for (;;) {
            const uint32_t src = static_cast<uint32_t>(nodes[node].bucket * kWays + way);
            Move(dst, src);
            dst = src;
            if (nodes[node].parent < 0) {
              EndMoves();
              return dst;
            }
            way = nodes[node].way;
            node = static_cast<size_t>(nodes[node].parent);
          }
        }
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
  void BeginMoves() noexcept {
    if constexpr (kShared) {
      moves_.store(moves_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
    }
  }
  void EndMoves() noexcept {
    if constexpr (kShared) {
      moves_.store(moves_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
  }

#if defined(BESS_PACKED_MAC_TABLE_TESTING)
 public:
  // Called by a reader between its primary and alternate probes (with the
  // key), and by the writer between a move's two stores (with the word).
  std::function<void(uint64_t)> between_probes_hook;
  std::function<void(uint64_t)> mid_move_hook;
  size_t PrimaryBucketForTesting(uint64_t key) const { return Hash(key) & mask_; }
  size_t AltBucketForTesting(uint64_t key) const { return Alt(Hash(key) & mask_, Hash(key)); }
  uint32_t FreeSlotForTesting(size_t b) const { return FreeIn(b); }
  // One move path of one step, as MakeRoom writes it; or its parts.
  void MoveForTesting(uint32_t to, uint32_t from) {
    BeginMoves();
    Move(to, from);
    EndMoves();
  }
  void BeginMovesForTesting() { BeginMoves(); }
  void MoveStepForTesting(uint32_t to, uint32_t from) { Move(to, from); }
  void EndMovesForTesting() { EndMoves(); }

 private:
#endif
  size_t capacity_;
  size_t nbuckets_;
  size_t mask_;
  uint64_t seed_;  // the hash's secret odd multiplier
  size_t size_ = 0;
  uint64_t *slots_ = nullptr;
  Cold *cold_ = nullptr;
  alignas(64) std::atomic<uint32_t> moves_{0};  // odd while a move path is written
  std::atomic<bool> lock_{false};
};

}  // namespace bess::l2

#endif  // BESS_L2_PACKED_MAC_TABLE_H_
