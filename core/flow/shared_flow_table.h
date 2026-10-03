// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_SHARED_FLOW_TABLE_H_
#define BESS_FLOW_SHARED_FLOW_TABLE_H_

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include "utils/logging.h"
#include <rte_spinlock.h>

#include "classifier/concurrent_exact.h"
#include "flow/flow_key.h"
#include "flow/flow_observer.h"
#include "flow/flow_storage.h"
#include "flow/flow_types.h"
#include "flow/owner.h"
#include "rcu/rcu_domain.h"
#include "utils/common.h"

// A flow table whose lookup is shared by every worker (roadmap M9, Decision
// D-052; guide in docs/flow-state.md). NOT installed: it is built on
// ConcurrentExactTable, which is internal until it is promoted.
//
// The shape follows Decision D-028. The directory is a ConcurrentExactTable
// (DPDK rte_hash, lock-free readers, QSBR-deferred delete) mapping the key to
// an eight-byte FlowHandle; the State lives in a fixed slot array of this
// library's own, so it is not limited to rte_hash's eight-byte value and its
// address is stable. Readers take no lock and execute no atomic
// read-modify-write. Writers (Emplace, Erase, aliases) may come from any
// thread and are serialized by one spinlock inside the table; that is the
// cost D-028 measured, and why new flows should be rare here -- when they
// are not, use one WorkerFlowTable per worker and hand creation to the owner.
//
// Two questions the roadmap keeps apart:
//
//   StateSharing::kSharedMutable  Every thread that finds a flow may use its
//       State, so State must bring its own synchronisation (atomics, a lock,
//       per-worker counters). The table adds none. Find()/Lookup() return
//       State *.
//   StateSharing::kOwnedByCreator Only the thread that created a flow uses
//       its State mutably (the usual shape: a shared directory so any worker
//       can classify, state mutated only by the owning worker). Other threads
//       get const access through Peek(); FindOwned()/LookupOwned() return a
//       mutable State * and, in builds with an Owner policy, abort if the
//       caller is not the creator.
//
// Lifetime. An erased flow's State is destroyed, and its slot reused, only
// after a grace period of the RcuDomain the table was given, so a worker that
// found a State before the erase may use it until its next quiescent state.
// Destruction runs on a thread that calls Emplace (when the table is
// otherwise full) or Reclaim(), under the writer lock; a control thread that
// calls Reclaim() periodically keeps destructors off workers. Anything that
// outlives a quiescent state carries a FlowHandle and resolves it with
// Lookup(handle) each time.

namespace bess::flow {

enum class StateSharing : uint8_t { kOwnedByCreator, kSharedMutable };

struct DefaultSharedFlowTableTraits {
  static constexpr size_t kAliases = 0;
  // Slots return in the order they were erased, spreading reuse over every
  // slot: handles here are the point of a shared table.
  static constexpr SlotReuse kReuse = SlotReuse::kFifo;
  static constexpr StateSharing kSharing = StateSharing::kSharedMutable;
  using Observer = NoFlowObserver;  // called under the writer lock
  using Owner = DefaultOwner;       // checks kOwnedByCreator access
  using Allocator = DefaultFlowAllocator;
};

template <typename Key, typename State,
          typename Traits = DefaultSharedFlowTableTraits>
  requires ByteHashableFlowKey<Key> && (sizeof(Key) <= 64) &&
           std::destructible<State> &&
           FlowObserver<typename Traits::Observer, State> &&
           OwnerPolicy<typename Traits::Owner> &&
           FlowAllocator<typename Traits::Allocator>
class SharedFlowTable {
 public:
  using key_type = Key;
  using state_type = State;
  using Observer = typename Traits::Observer;

  static constexpr size_t kAliases = Traits::kAliases;
  static constexpr StateSharing kSharing = Traits::kSharing;
  static constexpr size_t kMaxBatch = 64;
  static constexpr bool kSharedMutable = kSharing == StateSharing::kSharedMutable;

  static_assert(kAliases <= 14, "alias key bits must fit the slot's key mask");

  // `capacity` flows. The directory and the slot array are allocated here and
  // never grow. `domain` must outlive the table. Needs the EAL (the directory
  // is rte_hash): the classifier's lazy bring-up applies.
  static std::expected<std::unique_ptr<SharedFlowTable>, FlowTableError> Create(
      size_t capacity, rcu::RcuDomain &domain, int socket = SOCKET_ID_ANY,
      Observer observer = Observer{}) {
    size_t keys = 0;
    if (capacity == 0 || detail::MulOverflows(capacity, kKeysPerSlot, &keys) ||
        keys >= (size_t{1} << 30)) {
      return std::unexpected(FlowTableError::kInvalidCapacity);
    }
    size_t slot_bytes = 0, free_bytes = 0, pending_bytes = 0;
    if (detail::MulOverflows(capacity, sizeof(Slot), &slot_bytes) ||
        detail::MulOverflows(capacity, sizeof(uint32_t), &free_bytes) ||
        detail::MulOverflows(capacity, sizeof(uint32_t), &pending_bytes)) {
      return std::unexpected(FlowTableError::kTooLarge);
    }
    // The directory first: its failure must not follow a large allocation.
    // It holds one key per alias too, and deleted keys wait out a grace
    // period in it like slots do; CapacityFor adds the headroom.
    auto directory = classifier::ConcurrentExactTable::Create(
        static_cast<uint32_t>(sizeof(Key)),
        classifier::ConcurrentExactTable::CapacityFor(keys), domain, socket,
        classifier::ConcurrentExactTable::Writers::kSingle);
    if (!directory.has_value()) {
      return std::unexpected(FlowTableError::kBackendFailed);
    }
    Block slots, free_items, pending;
    if (!slots.Allocate(slot_bytes, std::max<size_t>(alignof(Slot), 64)) ||
        !free_items.Allocate(free_bytes, alignof(uint32_t)) ||
        !pending.Allocate(pending_bytes, alignof(uint32_t))) {
      return std::unexpected(FlowTableError::kOutOfMemory);
    }
    std::unique_ptr<SharedFlowTable> table(new (std::nothrow) SharedFlowTable(
        domain, std::move(*directory), std::move(observer)));
    if (table == nullptr) {
      return std::unexpected(FlowTableError::kOutOfMemory);
    }
    auto *slot_array = static_cast<Slot *>(slots.get());
    for (size_t i = 0; i < capacity; i++) {
      ::new (static_cast<void *>(slot_array + i)) Slot{};
    }
    table->slots_ = slot_array;
    table->free_.Init(static_cast<uint32_t *>(free_items.get()),
                      static_cast<uint32_t>(capacity), Traits::kReuse);
    table->pending_ = static_cast<uint32_t *>(pending.get());
    table->capacity_ = static_cast<uint32_t>(capacity);
    table->slots_block_ = std::move(slots);
    table->free_block_ = std::move(free_items);
    table->pending_block_ = std::move(pending);
    return table;
  }

  // Destroys every State, live or waiting out a grace period. No worker may be
  // using the table: the caller stops them first, as for any RCU-protected
  // object.
  ~SharedFlowTable() {
    domain_.Synchronize();
    if (directory_ != nullptr) {
      directory_->ReclaimAll();
    }
    for (uint32_t slot = 0; slot < capacity_ && slots_ != nullptr; slot++) {
      if (slots_[slot].generation.load(std::memory_order_relaxed) & 1) {
        slots_[slot].state_ptr()->~State();
      }
    }
    for (uint32_t i = 0; i < pending_count_; i++) {
      const uint32_t entry = (pending_head_ + i) % capacity_;
      slots_[pending_[entry] & kSlotMask].state_ptr()->~State();
    }
  }

  SharedFlowTable(const SharedFlowTable &) = delete;
  SharedFlowTable &operator=(const SharedFlowTable &) = delete;

  // -- readers: any thread, no lock -------------------------------------------

  // Read-only view of the State `key` names (primary or alias), or nullptr.
  // Valid until the calling worker's next quiescent state.
  const State *Peek(const Key &key) const noexcept {
    const FlowHandle handle = Resolve(key);
    return handle.id.value() == 0 ? nullptr
                                  : slots_[handle.id.value() - 1].state_ptr();
  }
  const State *Peek(FlowHandle handle) const noexcept {
    const uint32_t slot = SlotOf(handle);
    return slot == kNone ? nullptr : slots_[slot].state_ptr();
  }

  // Mutable access, for State that synchronises itself.
  State *Find(const Key &key) const noexcept
    requires(kSharedMutable)
  {
    const FlowHandle handle = Resolve(key);
    return handle.id.value() == 0 ? nullptr
                                  : slots_[handle.id.value() - 1].state_ptr();
  }
  State *Lookup(FlowHandle handle) const noexcept
    requires(kSharedMutable)
  {
    const uint32_t slot = SlotOf(handle);
    return slot == kNone ? nullptr : slots_[slot].state_ptr();
  }

  // Mutable access by the creating thread only (checked where the Owner
  // policy is).
  State *FindOwned(const Key &key) const
    requires(!kSharedMutable)
  {
    const FlowHandle handle = Resolve(key);
    return handle.id.value() == 0 ? nullptr : OwnedState(handle.id.value() - 1, "FindOwned");
  }
  State *LookupOwned(FlowHandle handle) const
    requires(!kSharedMutable)
  {
    const uint32_t slot = SlotOf(handle);
    return slot == kNone ? nullptr : OwnedState(slot, "LookupOwned");
  }

  // The handle of the flow `key` names, or kNoFlow; and whether `key` was an
  // alias. Compares only the primary key, which is immutable while the flow
  // lives.
  struct HandleRef {
    FlowHandle handle{};
    bool via_alias = false;
  };
  HandleRef FindHandle(const Key &key) const noexcept {
    const FlowHandle handle = Resolve(key);
    if (handle.id.value() == 0) {
      return {};
    }
    return {handle, std::memcmp(&key, slots_[handle.id.value() - 1].key_ptr(0),
                                sizeof(Key)) != 0};
  }

  // Looks up `keys.size()` (<= kMaxBatch) keys in one pass over the
  // directory, which hashes the batch inline and probes it with DPDK's bulk
  // lookup. out[i] is the State or nullptr; bit i of the result marks hits.
  uint64_t PeekBatch(std::span<const Key> keys,
                     std::span<const State *> out) const noexcept {
    return FindBatchImpl(keys, out);
  }
  uint64_t FindBatch(std::span<const Key> keys,
                     std::span<State *> out) const noexcept
    requires(kSharedMutable)
  {
    return FindBatchImpl(keys, out);
  }

  bool Alive(FlowHandle handle) const noexcept {
    return SlotOf(handle) != kNone;
  }
  // The flow's primary key, or nullptr for a stale handle. The pointee is
  // immutable until the flow is erased and reclaimed.
  const Key *KeyOf(FlowHandle handle) const noexcept {
    const uint32_t slot = SlotOf(handle);
    return slot == kNone ? nullptr : slots_[slot].key_ptr(0);
  }

  // -- writers: any thread; serialized by the table ---------------------------

  // Creates the flow `key` with State(args...) unless present. Construction
  // happens before the flow becomes visible, so a reader finds it complete.
  // kFull when no slot is free even after reclaiming what the readers have
  // released; nothing changes then.
  template <typename... Args>
  EmplaceResult<State> Emplace(const Key &key, Args &&...args) {
    Lock guard(lock_);
    return CreateLocked(key, nullptr, std::forward<Args>(args)...);
  }

  template <typename... Args>
    requires(kAliases > 0)
  EmplaceResult<State> EmplaceAliased(const Key &key, const Key &alias,
                                      Args &&...args) {
    Lock guard(lock_);
    return CreateLocked(key, &alias, std::forward<Args>(args)...);
  }

  AliasStatus AddAlias(FlowHandle handle, const Key &alias)
    requires(kAliases > 0)
  {
    Lock guard(lock_);
    const uint32_t slot_number = SlotOf(handle);
    if (slot_number == kNone) {
      return AliasStatus::kStale;
    }
    if (Resolve(alias).id.value() != 0) {
      return AliasStatus::kExists;
    }
    Slot &slot = slots_[slot_number];
    size_t j = 1;
    while (j <= kAliases && (slot.key_mask >> j & 1)) {
      j++;
    }
    if (j > kAliases) {
      return AliasStatus::kNoRoom;
    }
    slot.StoreKey(j, alias);
    const auto added = directory_->InsertIfAbsent(Bytes(alias), ValueOf(handle));
    CHECK(added.status == classifier::ConcurrentExactTable::InsertResult::Status::kInserted)
        << "the shared directory is sized for every key";
    slot.key_mask = static_cast<uint16_t>(slot.key_mask | (1u << j));
    return AliasStatus::kAdded;
  }

  bool RemoveAlias(const Key &key)
    requires(kAliases > 0)
  {
    Lock guard(lock_);
    const FlowHandle handle = Resolve(key);
    if (handle.id.value() == 0) {
      return false;
    }
    Slot &slot = slots_[handle.id.value() - 1];
    if (std::memcmp(&key, slot.key_ptr(0), sizeof(Key)) == 0) {
      return false;  // the primary key
    }
    for (size_t j = 1; j <= kAliases; j++) {
      if ((slot.key_mask >> j & 1) &&
          std::memcmp(&key, slot.key_ptr(j), sizeof(Key)) == 0) {
        directory_->Erase(Bytes(key));
        slot.key_mask = static_cast<uint16_t>(slot.key_mask & ~(1u << j));
        return true;
      }
    }
    return false;
  }

  // Erases the flow `key` names (primary or alias). Its keys leave the
  // directory at once; its State stays valid for readers until a grace period
  // passes, and is destroyed by a later Reclaim() or Emplace().
  bool Erase(const Key &key) {
    Lock guard(lock_);
    const FlowHandle handle = Resolve(key);
    if (handle.id.value() == 0) {
      return false;
    }
    EraseLocked(handle.id.value() - 1);
    return true;
  }
  // False -- and nothing happens -- for a stale handle.
  bool Erase(FlowHandle handle) {
    Lock guard(lock_);
    const uint32_t slot = SlotOf(handle);
    if (slot == kNone) {
      return false;
    }
    EraseLocked(slot);
    return true;
  }

  // Destroys the State of every erased flow whose grace period has passed and
  // makes its slot reusable. Returns the slots freed.
  size_t Reclaim() {
    Lock guard(lock_);
    return ReclaimLocked();
  }

  // Visits every live flow: fn(FlowHandle, const Key &primary, const State &).
  // Holds the writer lock: the callback must not call back into the table.
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    Lock guard(lock_);
    for (uint32_t slot = 0; slot < capacity_; slot++) {
      if (slots_[slot].LoadGeneration() & 1) {
        fn(HandleOf(slot), *slots_[slot].key_ptr(0), *slots_[slot].state_ptr());
      }
    }
  }

  // -- state of the table -----------------------------------------------------

  size_t size() const noexcept { return size_.load(std::memory_order_relaxed); }
  size_t capacity() const noexcept { return capacity_; }
  // Erased flows whose State has not been destroyed yet: the reclamation
  // backlog. A stalled reader makes it grow until the table is full.
  size_t pending_reclaim() const noexcept {
    return pending_published_.load(std::memory_order_relaxed);
  }
  size_t quarantined_slots() const noexcept {
    return quarantined_.load(std::memory_order_relaxed);
  }
  Observer &observer() noexcept { return observer_; }
  const classifier::ConcurrentExactTable &directory() const noexcept {
    return *directory_;
  }

  // Bytes of the slot array, free list and pending ring. The directory's own
  // memory is DPDK's (rte_hash in the EAL heap) and is not reported here.
  size_t slab_bytes() const noexcept {
    return slots_block_.bytes() + free_block_.bytes() + pending_block_.bytes();
  }
  static constexpr size_t slot_bytes() noexcept { return sizeof(Slot); }

  void SetFreeSlotGenerationForTesting(uint32_t slot, uint32_t generation) {
    Lock guard(lock_);
    CHECK_LT(slot, capacity_);
    CHECK_EQ(0u, slots_[slot].LoadGeneration() & 1);
    CHECK_EQ(0u, generation & 1);
    slots_[slot].StoreGeneration(generation);
  }

 private:
  static constexpr size_t kKeysPerSlot = 1 + kAliases;
  static constexpr uint32_t kNone = 0xffffffffu;
  static constexpr bool kStampOwner =
      !kSharedMutable && Traits::Owner::kChecked;

  using Slot = detail::SlotRecord<
      Key, State, kAliases, /*atomic generation=*/true,
      std::conditional_t<kStampOwner, OwnerToken, detail::NoExtra>>;
  using Block = detail::Block<typename Traits::Allocator>;

  // An erased flow waiting for readers is one word in a ring: its slot, and a
  // top bit that says the slot must not be reused afterwards (generation
  // exhausted). Entries are grouped into batches that share one grace period.
  static constexpr uint32_t kQuarantineBit = 0x80000000u;
  static constexpr uint32_t kSlotMask = 0x7fffffffu;
  struct Batch {
    rcu::GracePeriod token;
    uint32_t count;  // entries, in ring order
  };
  // Outstanding batches are bounded so the table needs no allocation; when
  // they are all in use, new entries join the newest batch under a newer
  // grace period, which is only later than necessary, never earlier.
  static constexpr uint32_t kMaxBatches = 256;

  class Lock {
   public:
    explicit Lock(rte_spinlock_t &l) noexcept : lock_(&l) {
      rte_spinlock_lock(lock_);
    }
    ~Lock() { rte_spinlock_unlock(lock_); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

   private:
    rte_spinlock_t *lock_;
  };

  SharedFlowTable(rcu::RcuDomain &domain,
                  std::unique_ptr<classifier::ConcurrentExactTable> directory,
                  Observer observer)
      : domain_(domain),
        directory_(std::move(directory)),
        observer_(std::move(observer)) {
    rte_spinlock_init(&lock_);
  }

  static classifier::ConstBytes Bytes(const Key &key) noexcept {
    return classifier::ConstBytes(reinterpret_cast<const std::byte *>(&key),
                                  sizeof(Key));
  }
  static uint64_t ValueOf(FlowHandle handle) noexcept {
    return std::bit_cast<uint64_t>(handle);
  }

  FlowHandle HandleOf(uint32_t slot) const noexcept {
    return {FlowId(slot + 1), slots_[slot].LoadGeneration()};
  }

  // The handle the directory holds for `key`, or the empty handle.
  FlowHandle Resolve(const Key &key) const noexcept {
    uint64_t value = 0;
    if (directory_->LookupBatch(Bytes(key), sizeof(Key), &value, 1) == 0) {
      return {};
    }
    return std::bit_cast<FlowHandle>(value);
  }

  uint32_t SlotOf(FlowHandle handle) const noexcept {
    const uint32_t slot = handle.id.value() - 1;  // an empty id wraps high
    if (slot >= capacity_) {
      return kNone;
    }
    const uint32_t generation = slots_[slot].LoadGeneration();
    return (generation == handle.generation && (generation & 1)) ? slot : kNone;
  }

  State *OwnedState(uint32_t slot, const char *op) const {
    if constexpr (kStampOwner) {
      const OwnerToken caller = Traits::Owner::Current();
      if (slots_[slot].extra != caller) [[unlikely]] {
        detail::OwnerViolation(op, slots_[slot].extra, caller);
      }
    }
    return slots_[slot].state_ptr();
  }

  template <typename Out>
  uint64_t FindBatchImpl(std::span<const Key> keys,
                         std::span<Out *> out) const noexcept {
    const size_t n = keys.size();
    promise(n <= kMaxBatch);
    promise(out.size() >= n);
    uint64_t values[kMaxBatch];
    const uint64_t hits = directory_->LookupBatch(
        classifier::ConstBytes(reinterpret_cast<const std::byte *>(keys.data()),
                               n * sizeof(Key)),
        sizeof(Key), values, n);
    for (size_t i = 0; i < n; i++) {
      out[i] = nullptr;
    }
    for (uint64_t m = hits; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(std::countr_zero(m));
      const auto handle = std::bit_cast<FlowHandle>(values[i]);
      out[i] = slots_[handle.id.value() - 1].state_ptr();
    }
    return hits;
  }

  template <typename... Args>
  EmplaceResult<State> CreateLocked(const Key &key, const Key *alias,
                                    Args &&...args) {
    if (const FlowHandle existing = Resolve(key); existing.id.value() != 0) {
      State *state = slots_[existing.id.value() - 1].state_ptr();
      if constexpr (kStampOwner) {
        if (slots_[existing.id.value() - 1].extra != Traits::Owner::Current()) {
          state = nullptr;  // not the creator: no mutable access
        }
      }
      return {state, existing, EmplaceStatus::kExists};
    }
    if (alias != nullptr) {
      if (std::memcmp(&key, alias, sizeof(Key)) == 0 ||
          Resolve(*alias).id.value() != 0) {
        return {nullptr, {}, EmplaceStatus::kAliasExists};
      }
    }
    if (free_.empty()) {
      ReclaimLocked();
    }
    if (free_.empty()) {
      observer_.OnFull();
      return {nullptr, {}, EmplaceStatus::kFull};
    }
    const uint32_t slot_number = free_.Peek();
    Slot &slot = slots_[slot_number];
    // Build the State while nobody can see it; a throwing constructor leaves
    // the table as it was.
    State *state = ::new (static_cast<void *>(slot.state))
        State(std::forward<Args>(args)...);
    free_.Pop();
    slot.StoreKey(0, key);
    slot.key_mask = 1;
    if constexpr (kStampOwner) {
      slot.extra = Traits::Owner::Current();
    }
    const uint32_t generation = slot.LoadGeneration() + 1;  // even -> odd
    const FlowHandle handle{FlowId(slot_number + 1), generation};
    if (alias != nullptr) {
      slot.StoreKey(1, *alias);
      slot.key_mask = 3;
    }
    // Everything the flow consists of is written; the directory insert below
    // (a release store inside rte_hash) is what publishes it to readers.
    slot.StoreGeneration(generation);
    if (alias != nullptr) {
      const auto second =
          directory_->InsertIfAbsent(Bytes(*alias), ValueOf(handle));
      CHECK(second.status ==
            classifier::ConcurrentExactTable::InsertResult::Status::kInserted)
          << "the shared directory is sized for every key";
    }
    const auto first = directory_->InsertIfAbsent(Bytes(key), ValueOf(handle));
    CHECK(first.status ==
          classifier::ConcurrentExactTable::InsertResult::Status::kInserted)
        << "the shared directory is sized for every key";
    size_.fetch_add(1, std::memory_order_relaxed);
    observer_.OnCreate(handle, *state);
    return {state, handle, EmplaceStatus::kCreated};
  }

  void EraseLocked(uint32_t slot_number) {
    Slot &slot = slots_[slot_number];
    State *state = slot.state_ptr();
    observer_.OnErase(HandleOf(slot_number), *state);
    // Out of the directory first, so no new reader can find the flow; then the
    // generation, so a handle already held stops resolving. The State and the
    // slot wait for readers that may be using them.
    for (size_t j = 0; j < kKeysPerSlot; j++) {
      if (slot.key_mask >> j & 1) {
        const bool erased = directory_->Erase(Bytes(*slot.key_ptr(j)));
        DCHECK(erased);
      }
    }
    slot.key_mask = 0;
    const uint32_t next = slot.LoadGeneration() + 1;  // odd -> even
    slot.StoreGeneration(next);
    size_.fetch_sub(1, std::memory_order_relaxed);

    const uint32_t tail = (pending_head_ + pending_count_) % capacity_;
    pending_[tail] = slot_number | (next == 0 ? kQuarantineBit : 0u);
    pending_count_++;
    untokened_++;
    pending_published_.store(pending_count_, std::memory_order_relaxed);
  }

  size_t ReclaimLocked() {
    if (untokened_ > 0) {
      // One grace period for everything erased since the last call; started
      // after those erases, as the RCU rules require.
      const rcu::GracePeriod token = domain_.StartGracePeriod();
      if (batch_count_ == kMaxBatches) {
        Batch &newest = batches_[(batch_head_ + batch_count_ - 1) % kMaxBatches];
        newest.token = token;
        newest.count += untokened_;
      } else {
        batches_[(batch_head_ + batch_count_) % kMaxBatches] = {token, untokened_};
        batch_count_++;
      }
      untokened_ = 0;
    }
    size_t freed = 0;
    while (batch_count_ > 0) {
      const Batch batch = batches_[batch_head_];
      if (!domain_.IsComplete(batch.token)) {
        break;
      }
      for (uint32_t i = 0; i < batch.count; i++) {
        const uint32_t entry = pending_[pending_head_];
        const uint32_t slot = entry & kSlotMask;
        slots_[slot].state_ptr()->~State();
        if (entry & kQuarantineBit) {
          quarantined_.fetch_add(1, std::memory_order_relaxed);
        } else {
          free_.Push(slot);
          freed++;
        }
        pending_head_ = (pending_head_ + 1) % capacity_;
        pending_count_--;
      }
      batch_head_ = (batch_head_ + 1) % kMaxBatches;
      batch_count_--;
    }
    pending_published_.store(pending_count_, std::memory_order_relaxed);
    return freed;
  }

  rcu::RcuDomain &domain_;
  std::unique_ptr<classifier::ConcurrentExactTable> directory_;
  [[no_unique_address]] Observer observer_;
  Slot *slots_ = nullptr;
  uint32_t capacity_ = 0;
  std::atomic<uint32_t> size_{0};
  std::atomic<uint32_t> quarantined_{0};
  std::atomic<uint32_t> pending_published_{0};
  mutable rte_spinlock_t lock_;
  // Guarded by lock_:
  detail::FreeList free_;
  uint32_t *pending_ = nullptr;
  uint32_t pending_head_ = 0;
  uint32_t pending_count_ = 0;
  uint32_t untokened_ = 0;
  Batch batches_[kMaxBatches] = {};
  uint32_t batch_head_ = 0;
  uint32_t batch_count_ = 0;
  Block slots_block_, free_block_, pending_block_;
};

}  // namespace bess::flow

#endif  // BESS_FLOW_SHARED_FLOW_TABLE_H_
