// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_CONCURRENT_EXACT_H_
#define BESS_CLASSIFIER_CONCURRENT_EXACT_H_

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_spinlock.h>

#include <array>
#include <cstring>
#include <utility>

#include "classifier/classifier.h"
#include "rcu/rcu_domain.h"
#include "utils/common.h"
#include "utils/dpdk_memory.h"

namespace bess::classifier {

namespace detail {

// CRC32C over exactly W bytes, seed 0, inlined: the same function rte_hash
// applies (rte_hash_crc, the table's hash_func) and bit-identical to it --
// CRC is defined over the byte stream, so 8-, 4-, 2- and 1-byte steps give
// the same value however the bytes are grouped or aligned. The step functions
// keep rte_hash_crc's fallback to a software CRC on CPUs without one.
template <size_t W>
inline uint32_t CrcFixed(const std::byte *p) noexcept {
  uint32_t h = 0;
  size_t i = 0;
  for (; i + 8 <= W; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    h = rte_hash_crc_8byte(v, h);
  }
  if constexpr (W % 8 >= 4) {
    uint32_t v;
    std::memcpy(&v, p + i, 4);
    h = rte_hash_crc_4byte(v, h);
    i += 4;
  }
  if constexpr (W % 4 >= 2) {
    uint16_t v;
    std::memcpy(&v, p + i, 2);
    h = rte_hash_crc_2byte(v, h);
    i += 2;
  }
  if constexpr (W % 2 == 1) {
    h = rte_hash_crc_1byte(static_cast<uint8_t>(p[i]), h);
  }
  return h;
}

using HashBatchFn = void (*)(const std::byte *keys, size_t stride, size_t n,
                             hash_sig_t *out) noexcept;

template <size_t W>
void HashBatchFixed(const std::byte *keys, size_t stride, size_t n,
                    hash_sig_t *out) noexcept {
  for (size_t i = 0; i < n; i++) {
    out[i] = CrcFixed<W>(keys + i * stride);
  }
}

// The kernel for a key width, chosen once per table (1..64 bytes).
inline HashBatchFn SelectHashBatch(size_t width) noexcept {
  static constexpr auto kTable = []<size_t... I>(std::index_sequence<I...>) {
    return std::array<HashBatchFn, sizeof...(I)>{&HashBatchFixed<I + 1>...};
  }(std::make_index_sequence<64>{});
  return width >= 1 && width <= kTable.size() ? kTable[width - 1] : nullptr;
}

// Key equality for exactly W bytes (0 iff equal, the rte_hash_cmp_eq_t
// contract), with the width a compile-time constant so the compiler emits a
// few loads and compares instead of calling libc's general memcmp. rte_hash
// has its own SIMD compares only for 16/32/.../128-byte keys and falls back to
// memcmp through a function pointer otherwise -- ~9% of a WildcardMatch
// lookup, profiled (Decision D-015).
//
// Equality only: rte_hash tests the result against 0 and never uses memcmp's
// ordering, which is also what stopped the compiler from inlining a
// constant-size memcmp (it compiled to a tail call into libc). XOR the words
// and OR the differences: branchless, two loads and a compare per 8 bytes.
template <size_t W>
int CmpFixed(const void *a, const void *b, size_t) {
  const auto *x = static_cast<const std::byte *>(a);
  const auto *y = static_cast<const std::byte *>(b);
  uint64_t diff = 0;
  size_t i = 0;
  for (; i + 8 <= W; i += 8) {
    uint64_t u, v;
    std::memcpy(&u, x + i, 8);
    std::memcpy(&v, y + i, 8);
    diff |= u ^ v;
  }
  if constexpr (W % 8 >= 4) {
    uint32_t u, v;
    std::memcpy(&u, x + i, 4);
    std::memcpy(&v, y + i, 4);
    diff |= u ^ v;
    i += 4;
  }
  if constexpr (W % 4 >= 2) {
    uint16_t u, v;
    std::memcpy(&u, x + i, 2);
    std::memcpy(&v, y + i, 2);
    diff |= static_cast<uint16_t>(u ^ v);
    i += 2;
  }
  if constexpr (W % 2 == 1) {
    diff |= std::to_integer<uint8_t>(x[i] ^ y[i]);
  }
  return diff != 0;
}

using CmpFn = int (*)(const void *, const void *, size_t);

// A fixed-width compare for widths rte_hash does not specialize; nullptr
// where it already has one (multiples of 16 up to 128).
inline CmpFn SelectCmp(size_t width) noexcept {
  static constexpr auto kTable = []<size_t... I>(std::index_sequence<I...>) {
    return std::array<CmpFn, sizeof...(I)>{&CmpFixed<I + 1>...};
  }(std::make_index_sequence<64>{});
  if (width == 0 || width > kTable.size() || width % 16 == 0) {
    return nullptr;
  }
  return kTable[width - 1];
}

}  // namespace detail

// A shared exact-match table updated in place while workers read it (G1.2
// mode C). DPDK's `rte_hash` owns the algorithm and the concurrency: created
// with RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF, lookups are lock-free and run
// while one writer adds and deletes. A deleted key's slot is reused only
// after a grace period of the runtime RcuDomain's QSBR: the table holds it
// and hands it back itself (Decision D-208), the way K7's rte_lpm queue does
// inside DPDK.
//
// Why not a generation swap: that rebuilds the whole table per insert
// (ExactMatch: 47 ms per add at 100K rules). Here an insert or delete is one
// rte_hash operation, flat in table size. Lookup cost, the rte_hash modes
// chosen, and the DPDK behaviours this relies on (each pinned by a test) are
// in docs/dataplane-tables.md; Decisions D-001, D-002 (docs/decisions.md).
//
// Values are up to eight bytes, stored in rte_hash's data pointer.
//
// Writers (D-028). A kSingle table has one writer at a time, serialized by
// the caller (a module's command path, or the one worker that owns a
// partition). A kShared table takes writers from any thread -- several
// workers learning flows -- serialized by a spinlock inside the table;
// InsertIfAbsent() makes "first packet of a new flow" race-free. DPDK's own
// multi-writer mode is not used: it serializes writers on one table lock as
// well, and its per-lcore free-slot cache has no guard for threads without
// an lcore id (control threads). A shared table has a fixed capacity: it
// cannot be copied to grow while workers write it.
//
// Deleted slots (Decision D-208, amending D-002). No RCU is attached to the
// rte_hash, so the LF flag's NO_FREE_ON_DEL holds: rte_hash_del_key takes
// the key out of its bucket, keeps its slot allocated and returns the slot's
// position, and rte_hash_free_key_with_position hands it back later.
// Lookups are untouched (they never read rte_hash's RCU state).
//  - One producer. Every delete -- a control thread's, or a worker's
//    (SharedFlowTable expiry) -- runs inside the table's writer
//    serialization (the caller's for kSingle, writer_lock_ for kShared), as
//    rte_hash's LF mode requires of every add and delete, and so does every
//    free: rte_hash puts a slot back with a single-producer ring enqueue,
//    which a failed add also does. The held positions are therefore a plain
//    ring of capacity() entries (4 B each) that one writer at a time fills
//    and drains -- no MPSC ring, no per-writer rings, no atomic
//    read-modify-write -- and a held position is an allocated slot, so the
//    ring cannot overflow.
//  - Grace periods. Each delete starts one, after the key left its bucket,
//    in a ring of kMaxDeleteBatches; when that is full, the delete joins the
//    newest batch, whose grace period becomes the delete's own, later one
//    (counted: DeferStats::merged) -- only later than necessary, never
//    earlier.
//  - Frees. On the writing thread: up to kFreeBatch ready slots per Erase
//    and per add that finds no free slot; Reclaim, ReclaimAll and
//    HasRoomForOne on demand (owners: the control thread).
//  - Never a wait. A delete or a free takes no lock, allocates nothing and
//    never waits for a grace period: starting one is an atomic add, checking
//    one is loads (RcuDomain::StartGracePeriodLockFree, IsCompleteLockFree).
//    While readers stall, held slots are bounded by the key store itself:
//    once they exhaust it, an add is refused kFull (counted:
//    DeferStats::refused_full) until readers pass a quiescent state.
//
// Sizing (Decision D-010). Capacity is key slots, fixed at creation; the
// owner grows by building a larger table. Owners create tables with
// CapacityFor(rules): three quarters of a power of two, so rte_hash's bucket
// array (rounded up to the next power of two) has 4/3 as many positions as
// slots and every slot is reachable (occupancy_bench: 100% for every key
// shape, and the last 10% of a fill still costs ~200 ns per add). Owners grow
// when HasRoomForOne() is false: live keys plus deletes still waiting out a
// grace period must leave Headroom() slots free, because a delete's slot is
// reusable only after every worker's next quiescent point. `Upsert` reporting
// kFull remains the backstop.
class ConcurrentExactTable {
 public:
  enum class UpsertResult : uint8_t { kInserted, kUpdated, kFull };
  enum class Writers : uint8_t { kSingle, kShared };

  struct InsertResult {
    enum class Status : uint8_t { kInserted, kExists, kFull } status;
    uint64_t value;  // the stored value: `value` if inserted, else the one
                     // already there (kExists)
  };

  // Deleted slots' grace periods in flight (D-208), at most; past it a
  // delete joins the newest batch. One writer's peak deletes over a p99.9
  // grace period (D-010: about 4.3M/s for 60 us), as kMinHeadroom.
  static constexpr uint32_t kMaxDeleteBatches = 256;
  // Held slots one Erase, one add that finds no free slot, or one Reclaim()
  // frees at most: the batch DPDK's defer queue freed per call
  // (RTE_HASH_RCU_DQ_RECLAIM_MAX), so a worker's delete stays O(1).
  static constexpr uint32_t kFreeBatch = 16;

  // What the deleted slots are doing (D-208); readable from any thread.
  struct DeferStats {
    uint64_t held = 0;          // slots of deleted keys awaiting a grace period
    uint64_t freed = 0;         // slots handed back to rte_hash, ever
    uint64_t merged = 0;        // deletes that joined the newest batch, ever
    uint64_t refused_full = 0;  // adds refused kFull, ever
    uint64_t checks = 0;        // grace periods checked by frees, ever
  };

  // Slots of reserve for deletes pending a grace period, at least: one
  // writer's measured peak (~4M ops/s with readers present, update_scale_bench
  // E2) times a 64 us grace period. Tables also keep 5% of their slots.
  static constexpr uint32_t kMinHeadroom = 256;

  static uint32_t Headroom(uint32_t capacity) noexcept {
    return std::max(capacity / 20, kMinHeadroom);
  }

  // The smallest capacity (3/4 of a power of two, at least 768) that holds
  // `rules` keys plus its headroom.
  static uint32_t CapacityFor(size_t rules) noexcept;

  // `capacity` key slots exactly; pass CapacityFor(rules) for the D-010
  // sizing. On DPDK socket `socket`, by default the memory node (D-123).
  // Out of memory throws (utils::MemoryRefused for DPDK memory, D-198);
  // an error is any other failure.
  static std::expected<std::unique_ptr<ConcurrentExactTable>, std::string>
  Create(uint32_t key_len, uint32_t capacity, rcu::RcuDomain &domain,
         int socket = bess::utils::MemoryNode(),
         Writers writers = Writers::kSingle);

  // Memory (D-198): what Create allocates on the DPDK heap, the rte_hash for
  // max(capacity, 8) keys (detail::RteHashBytes) and the deleted-slot rings
  // (D-208: 4 B per key and kMaxDeleteBatches grace periods). Fixed: the
  // table never grows; owners admit a bigger replacement.
  static size_t Bytes(uint32_t key_len, uint32_t capacity,
                      const rcu::RcuDomain &domain,
                      int socket = bess::utils::MemoryNode(),
                      Writers writers = Writers::kSingle);

  ~ConcurrentExactTable();

  ConcurrentExactTable(const ConcurrentExactTable &) = delete;
  ConcurrentExactTable &operator=(const ConcurrentExactTable &) = delete;

  // -- writer ------------------------------------------------------------------

  UpsertResult Upsert(ConstBytes key, uint64_t value);
  // Adds `key` unless present; never overwrites. With several writers,
  // exactly one of those racing to add the same key sees kInserted, and the
  // others get the winner's value (kExists).
  InsertResult InsertIfAbsent(ConstBytes key, uint64_t value);
  // False if the key is absent.
  bool Erase(ConstBytes key);

  Writers writers() const noexcept { return writers_; }

  // Whether one more key fits with Headroom() slots to spare, counting
  // deletes still waiting out a grace period (reclaims what is ready first).
  bool HasRoomForOne();

  // Frees every held slot whose grace period has passed.
  void ReclaimAll();

  // Frees up to kFreeBatch held slots whose grace period has passed. Erases
  // and adds do this on their own; exposed for owners and tests.
  void Reclaim();

  DeferStats defer_stats() const noexcept;

  // Visits every entry: fn(ConstBytes key, uint64_t value). Writer thread
  // (a shared table holds its writer lock meanwhile).
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    WriterGuard guard(*this);
    const void *key;
    void *data;
    uint32_t next = 0;
    while (rte_hash_iterate(table_, &key, &data, &next) >= 0) {
      fn(ConstBytes(static_cast<const std::byte *>(key), key_len_),
         static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)));
    }
  }

  // Visits entries by key slot from `slot` (0: the first) until fn(ConstBytes
  // key, uint64_t value) returns false or the slots end; returns the slot to
  // resume from, 0 at the end. A key keeps its slot while it is in the table
  // (cuckoo moves bucket entries, not key slots), so a walk resumed on the
  // same table visits an entry present throughout exactly once; one added
  // meanwhile may land behind it. Costs the slots it passes, not the table.
  template <typename Fn>
  uint32_t VisitFrom(uint32_t slot, Fn &&fn) const {
    WriterGuard guard(*this);
    const auto end = static_cast<uint32_t>(rte_hash_max_key_id(table_));
    for (uint32_t s = slot; s < end; s++) {
      void *key;
      void *data;
      if (rte_hash_get_key_with_position(table_, static_cast<int32_t>(s), &key) != 0 ||
          rte_hash_lookup_data(table_, key, &data) != static_cast<int32_t>(s)) {
        continue;  // a free slot, or one whose key waits out a grace period
      }
      if (!fn(ConstBytes(static_cast<const std::byte *>(key), key_len_),
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data)))) {
        return s + 1 < end ? s + 1 : 0;
      }
    }
    return 0;
  }

  size_t size() const noexcept {
    return size_.load(std::memory_order_relaxed);
  }
  // Key slots taken: live entries plus deleted ones still waiting out a
  // reader grace period (DeferStats::held).
  size_t slots_in_use() const noexcept {
    return static_cast<size_t>(rte_hash_count(table_));
  }
  uint32_t capacity() const noexcept { return capacity_; }
  uint32_t key_len() const noexcept { return key_len_; }
  // DPDK memory held: Bytes(key_len(), capacity()).
  size_t bytes() const noexcept;
  // The table's own hash of `key` (rte_hash_hash), for tests that prove the
  // inline kernel identical to it.
  hash_sig_t DpdkHash(ConstBytes key) const noexcept {
    return rte_hash_hash(table_, key.data());
  }
  // The inline kernel's hash of `key`.
  hash_sig_t InlineHash(ConstBytes key) const noexcept {
    hash_sig_t sig;
    hash_batch_(key.data(), key.size(), 1, &sig);
    return sig;
  }

  // -- reader -------------------------------------------------------------------

  // The read path's two parts, for a wrapper that inlines it without this
  // header (flow::SharedExactIndex): the DPDK table and the batch hash.
  rte_hash *dpdk_table() const noexcept { return table_; }
  detail::HashBatchFn hash_batch() const noexcept { return hash_batch_; }

  // Looks up `n` (<= 64) keys packed at `stride` bytes. Writes values[i] and
  // sets bit i for each hit; misses leave values[i] untouched.
  //
  // Hashes the batch here, inline, with the fixed-width kernel chosen at
  // create, and calls DPDK's prehashed bulk lookup: rte_hash would otherwise
  // hash each key through its hash-function pointer into the generic,
  // any-length rte_hash_crc (~20% of a WildcardMatch lookup, profiled). Same
  // hash, same probe, same concurrency protocol (Decision D-015).
  uint64_t LookupBatch(ConstBytes keys, size_t stride, uint64_t *values,
                       size_t n) const noexcept {
    promise(n <= 64);
    hash_sig_t sigs[64];
    hash_batch_(keys.data(), stride, n, sigs);
    return LookupBatchPrehashed(keys, stride, sigs, values, n);
  }

  // LookupBatch with the keys already hashed: sigs[i] must be the table's
  // hash of key i (detail::CrcFixed over key_len() bytes), for a caller that
  // builds and hashes its keys in one pass (ExactMatch, D-121).
  uint64_t LookupBatchPrehashed(ConstBytes keys, size_t stride,
                                hash_sig_t *sigs, uint64_t *values,
                                size_t n) const noexcept {
    promise(n <= 64);
    const void *ptrs[64];
    void *data[64];
    for (size_t i = 0; i < n; i++) {
      ptrs[i] = keys.data() + i * stride;
    }
    uint64_t hits = 0;
    rte_hash_lookup_with_hash_bulk_data(table_, ptrs, sigs,
                                        static_cast<uint32_t>(n), &hits, data);
    for (uint64_t m = hits; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(__builtin_ctzll(m));
      values[i] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data[i]));
    }
    return hits;
  }

 private:
  // A grace period for the deletes behind it in the held ring (D-208).
  struct Batch {
    rcu::GracePeriod token;  // started after the batch's last delete
    uint32_t count;          // held positions, in ring order
  };
  static_assert(std::has_single_bit(kMaxDeleteBatches));

  // The held-slot rings: a position per key slot, and the grace periods.
  static size_t HeldBytes(uint32_t entries) noexcept {
    return utils::DpdkArray<uint32_t>::Bytes(entries) +
           utils::DpdkArray<Batch>::Bytes(kMaxDeleteBatches);
  }

  ConcurrentExactTable(rte_hash *table, uint32_t key_len, uint32_t capacity,
                       Writers writers, rcu::RcuDomain &domain,
                       utils::DpdkArray<uint32_t> held,
                       utils::DpdkArray<Batch> batches) noexcept
      : table_(table),
        key_len_(key_len),
        capacity_(capacity),
        writers_(writers),
        hash_batch_(detail::SelectHashBatch(key_len)),
        domain_(domain),
        held_(std::move(held)),
        batches_(std::move(batches)) {
    rte_spinlock_init(&writer_lock_);
  }

  // Serializes a kShared table's writers; nothing for a kSingle table.
  class WriterGuard {
   public:
    explicit WriterGuard(const ConcurrentExactTable &t) noexcept
        : lock_(t.writers_ == Writers::kShared ? &t.writer_lock_ : nullptr) {
      if (lock_ != nullptr) {
        rte_spinlock_lock(lock_);
      }
    }
    ~WriterGuard() {
      if (lock_ != nullptr) {
        rte_spinlock_unlock(lock_);
      }
    }
    WriterGuard(const WriterGuard &) = delete;
    WriterGuard &operator=(const WriterGuard &) = delete;

   private:
    rte_spinlock_t *lock_;
  };

  // Bodies with the writer lock held (or not needed).
  UpsertResult UpsertLocked(ConstBytes key, uint64_t value);
  // A deleted key's slot, `position`, waits out a grace period started now.
  void HoldLocked(uint32_t position) noexcept;
  // Frees up to `limit` held slots whose grace period has passed, oldest
  // first; returns how many.
  uint32_t FreeReadyLocked(uint32_t limit) noexcept;

  rte_hash *table_;
  const uint32_t key_len_;
  const uint32_t capacity_;
  const Writers writers_;
  const detail::HashBatchFn hash_batch_;
  mutable rte_spinlock_t writer_lock_;
  std::atomic<size_t> size_{0};
  rcu::RcuDomain &domain_;
  // Held slots (D-208), off the line lookups read. Written only under the
  // writer serialization; the counters are atomics (loads and stores, no
  // read-modify-write) so that defer_stats() may read them from anywhere.
  // held_ is a ring of capacity_ positions from held_head_; batches_ a ring
  // of kMaxDeleteBatches from batch_head_, oldest first, whose counts add
  // up to held_count_.
  alignas(RTE_CACHE_LINE_SIZE) utils::DpdkArray<uint32_t> held_;
  utils::DpdkArray<Batch> batches_;
  uint32_t held_head_ = 0;
  uint32_t batch_head_ = 0;
  uint32_t batch_count_ = 0;
  std::atomic<uint32_t> held_count_{0};
  std::atomic<uint64_t> freed_{0};
  std::atomic<uint64_t> merged_{0};
  std::atomic<uint64_t> refused_full_{0};
  std::atomic<uint64_t> checks_{0};
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_CONCURRENT_EXACT_H_
