// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/concurrent_exact.h"

#include <rte_hash_crc.h>

#include <atomic>
#include <new>

#include "classifier/rte_hash_exact.h"
#include "dpdk.h"
#include "utils/logging.h"

namespace bess::classifier {

namespace {

// A counter only the writer serialization writes: a load and a store, no
// locked read-modify-write on the writer's path.
template <typename T>
void Add(std::atomic<T> &counter, T n) noexcept {
  counter.store(counter.load(std::memory_order_relaxed) + n,
                std::memory_order_relaxed);
}

}  // namespace

std::expected<std::unique_ptr<ConcurrentExactTable>, std::string>
ConcurrentExactTable::Create(uint32_t key_len, uint32_t capacity,
                             rcu::RcuDomain &domain, int socket,
                             Writers writers) {
  if (key_len == 0 || key_len > 64) {
    return std::unexpected("key length must be 1..64 bytes");
  }
  if (!IsDpdkInitialized()) {
    InitDpdk();  // rte_hash lives in EAL memory
  }
  static std::atomic<uint64_t> seq{0};
  const std::string name = "cext_" + std::to_string(seq++);
  rte_hash_parameters params{};
  params.name = name.c_str();
  params.entries = std::max<uint32_t>(capacity, 8);
  params.key_len = key_len;
  params.hash_func = rte_hash_crc;
  params.hash_func_init_val = 0;
  params.socket_id = socket;
  // No RCU is attached (D-208): the LF flag alone keeps a deleted key's slot
  // allocated (NO_FREE_ON_DEL) until this table frees it.
  params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;
  // On the memory policy: retried on the normal-page fallback heap when the
  // socket is out of memory (MemoryRefused if that is not allowed). First,
  // so that a size rte_hash refuses (over 2^30 keys) allocates nothing.
  std::unique_ptr<rte_hash, void (*)(rte_hash *)> table(
      detail::CreateRteHash(params), rte_hash_free);
  if (table == nullptr) {
    return std::unexpected("rte_hash_create failed (capacity " +
                           std::to_string(capacity) + ")");
  }
  if (const detail::CmpFn cmp = detail::SelectCmp(key_len)) {
    rte_hash_set_cmp_func(table.get(), cmp);
  }
  // The held-slot rings, on the memory policy like the table
  // (utils::DpdkAllocate: a refusal throws MemoryRefused, and `table` frees
  // the hash).
  utils::DpdkArray<uint32_t> held(params.entries, socket);
  utils::DpdkArray<Batch> batches(kMaxDeleteBatches, socket);
  // Nothrow: a refusal here must free the hash rather than leak it.
  rte_hash *const raw = table.release();
  auto *wrapper = new (std::nothrow)
      ConcurrentExactTable(raw, key_len, params.entries, writers, domain,
                           std::move(held), std::move(batches));
  if (wrapper == nullptr) {
    rte_hash_free(raw);
    throw std::bad_alloc();
  }
  return std::unique_ptr<ConcurrentExactTable>(wrapper);
}

ConcurrentExactTable::~ConcurrentExactTable() {
  // Held slots go with the table: they are slots of its own key store.
  // Owners drop a table only with the last generation that maps it, retired
  // through the same QSBR variable, so no reader is still inside it.
  rte_hash_free(table_);
}

size_t ConcurrentExactTable::Bytes(uint32_t key_len, uint32_t capacity,
                                   const rcu::RcuDomain & /*domain*/,
                                   int /*socket*/, Writers /*writers*/) {
  const uint32_t entries = std::max<uint32_t>(capacity, 8);
  return detail::RteHashBytes(entries, key_len) + HeldBytes(entries);
}

size_t ConcurrentExactTable::bytes() const noexcept {
  return detail::RteHashBytes(capacity_, key_len_) + HeldBytes(capacity_);
}

ConcurrentExactTable::DeferStats ConcurrentExactTable::defer_stats()
    const noexcept {
  return {held_count_.load(std::memory_order_relaxed),
          freed_.load(std::memory_order_relaxed),
          merged_.load(std::memory_order_relaxed),
          refused_full_.load(std::memory_order_relaxed),
          checks_.load(std::memory_order_relaxed)};
}

ConcurrentExactTable::UpsertResult ConcurrentExactTable::Upsert(
    ConstBytes key, uint64_t value) {
  WriterGuard guard(*this);
  return UpsertLocked(key, value);
}

ConcurrentExactTable::InsertResult ConcurrentExactTable::InsertIfAbsent(
    ConstBytes key, uint64_t value) {
  promise(key.size() == key_len_);
  WriterGuard guard(*this);
  // Lookup and add under one writer lock: no other writer can add the key
  // in between. Readers are unaffected (lock-free throughout).
  const hash_sig_t sig = rte_hash_hash(table_, key.data());
  void *existing = nullptr;
  if (rte_hash_lookup_with_hash_data(table_, key.data(), sig, &existing) >=
      0) {
    return {InsertResult::Status::kExists,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(existing))};
  }
  void *const data = reinterpret_cast<void *>(static_cast<uintptr_t>(value));
  if (rte_hash_add_key_with_hash_data(table_, key.data(), sig, data) != 0) {
    // As in UpsertLocked: free what is ready and retry once.
    if (FreeReadyLocked(kFreeBatch) == 0 ||
        rte_hash_add_key_with_hash_data(table_, key.data(), sig, data) != 0) {
      Add(refused_full_, uint64_t{1});
      return {InsertResult::Status::kFull, 0};
    }
  }
  size_.fetch_add(1, std::memory_order_relaxed);
  return {InsertResult::Status::kInserted, value};
}

ConcurrentExactTable::UpsertResult ConcurrentExactTable::UpsertLocked(
    ConstBytes key, uint64_t value) {
  promise(key.size() == key_len_);
  void *const data = reinterpret_cast<void *>(static_cast<uintptr_t>(value));
  // One hash for both steps. rte_hash_add_key_data returns 0 for insert
  // and update alike; the lookup tells them apart (size_ accounting).
  const hash_sig_t sig = rte_hash_hash(table_, key.data());
  const bool present = rte_hash_lookup_with_hash(table_, key.data(), sig) >= 0;
  if (rte_hash_add_key_with_hash_data(table_, key.data(), sig, data) != 0) {
    // No free slot (or no place for the key): hand back held slots whose
    // grace period has passed and retry once. Nothing waits: with readers
    // stalled the add is refused, counted.
    if (FreeReadyLocked(kFreeBatch) == 0 ||
        rte_hash_add_key_with_hash_data(table_, key.data(), sig, data) != 0) {
      Add(refused_full_, uint64_t{1});
      return UpsertResult::kFull;
    }
  }
  if (present) {
    return UpsertResult::kUpdated;
  }
  size_.fetch_add(1, std::memory_order_relaxed);
  return UpsertResult::kInserted;
}

uint32_t ConcurrentExactTable::CapacityFor(size_t rules) noexcept {
  uint64_t pow2 = 1024;
  while (pow2 / 4 * 3 - Headroom(static_cast<uint32_t>(pow2 / 4 * 3)) <
         rules + 1) {
    pow2 *= 2;
  }
  return static_cast<uint32_t>(pow2 / 4 * 3);
}

bool ConcurrentExactTable::HasRoomForOne() {
  ReclaimAll();
  return slots_in_use() + 1 + Headroom(capacity_) <= capacity_;
}

void ConcurrentExactTable::ReclaimAll() {
  WriterGuard guard(*this);
  (void)FreeReadyLocked(UINT32_MAX);
}

void ConcurrentExactTable::Reclaim() {
  WriterGuard guard(*this);
  (void)FreeReadyLocked(kFreeBatch);
}

void ConcurrentExactTable::HoldLocked(uint32_t position) noexcept {
  // Started after rte_hash_del_key took the key out of its bucket (a release
  // store), as the RCU rules require: a reader that found the key before
  // then is online at this token and must pass a quiescent state.
  const rcu::GracePeriod token = domain_.StartGracePeriodLockFree();
  const uint32_t held = held_count_.load(std::memory_order_relaxed);
  // Never full: each held position is a distinct allocated slot.
  promise(held < capacity_);
  uint32_t tail = held_head_ + held;
  if (tail >= capacity_) {
    tail -= capacity_;
  }
  held_[tail] = position;
  held_count_.store(held + 1, std::memory_order_relaxed);
  if (batch_count_ == kMaxDeleteBatches) {
    // Every grace period in flight has a batch: this delete joins the newest
    // under its own, later grace period, which only delays that batch.
    Batch &newest =
        batches_[(batch_head_ + batch_count_ - 1) & (kMaxDeleteBatches - 1)];
    newest.token = token;
    newest.count++;
    Add(merged_, uint64_t{1});
  } else {
    batches_[(batch_head_ + batch_count_) & (kMaxDeleteBatches - 1)] = {token,
                                                                       1};
    batch_count_++;
  }
}

uint32_t ConcurrentExactTable::FreeReadyLocked(uint32_t limit) noexcept {
  uint32_t freed = 0;
  uint32_t checks = 0;
  // Tokens grow along the ring (started in order under the serialization;
  // a merge only raises the newest) and complete in order: stop at the
  // first that has not. Each check that passes frees at least one slot, so
  // a call makes at most limit + 1 checks.
  while (batch_count_ > 0 && freed < limit) {
    Batch &oldest = batches_[batch_head_];
    checks++;
    if (!domain_.IsCompleteLockFree(oldest.token)) {
      break;
    }
    const uint32_t n = std::min(oldest.count, limit - freed);
    for (uint32_t i = 0; i < n; i++) {
      const int rc = rte_hash_free_key_with_position(
          table_, static_cast<int32_t>(held_[held_head_]));
      DCHECK_EQ(rc, 0);
      held_head_ = held_head_ + 1 == capacity_ ? 0 : held_head_ + 1;
    }
    freed += n;
    oldest.count -= n;
    if (oldest.count == 0) {
      batch_head_ = (batch_head_ + 1) & (kMaxDeleteBatches - 1);
      batch_count_--;
    }
  }
  if (checks > 0) {
    Add(checks_, uint64_t{checks});
  }
  if (freed > 0) {
    held_count_.store(held_count_.load(std::memory_order_relaxed) - freed,
                      std::memory_order_relaxed);
    Add(freed_, uint64_t{freed});
  }
  return freed;
}

bool ConcurrentExactTable::Erase(ConstBytes key) {
  promise(key.size() == key_len_);
  WriterGuard guard(*this);
  // The slot stays allocated (NO_FREE_ON_DEL); rte_hash returns its position.
  const int32_t position = rte_hash_del_key(table_, key.data());
  if (position < 0) {
    return false;
  }
  size_.fetch_sub(1, std::memory_order_relaxed);
  // Ready slots first, so their batches make room for this one's.
  (void)FreeReadyLocked(kFreeBatch);
  HoldLocked(static_cast<uint32_t>(position));
  return true;
}

}  // namespace bess::classifier
