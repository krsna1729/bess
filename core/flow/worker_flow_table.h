// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_WORKER_FLOW_TABLE_H_
#define BESS_FLOW_WORKER_FLOW_TABLE_H_

#include <algorithm>
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

#include "flow/flow_index.h"
#include "flow/flow_key.h"
#include "flow/flow_observer.h"
#include "flow/flow_storage.h"
#include "flow/flow_types.h"
#include "flow/owner.h"
#include "utils/common.h"

// A worker-owned flow table (roadmap M9, Decision D-052; guide in
// docs/flow-state.md). Experimental API.
//
// What it is. A map from a typed key to application-owned State, with
//
//   - the State constructed in place and never moved: its address is stable
//     until the flow is erased;
//   - an optional FlowHandle (slot id + generation) that stays safe to keep
//     after the flow is gone;
//   - optional alias keys that name the same flow (the reverse key of a
//     bidirectional flow, a second tunnel id);
//   - a fixed capacity chosen at construction, no allocation after it, no
//     resize, no rehash: a full table says so (EmplaceStatus::kFull) and
//     changes nothing.
//
// Ownership. The table belongs to one worker. Nothing in it is atomic and
// nothing is locked: lookup, create, erase and the State itself are ordinary
// loads and stores, and no other thread may call any member. Builds that
// enable the Owner policy check this on every call and abort on a violation.
// A table can be handed to another worker with ReleaseOwner() once the first
// is done with it. For lookup shared between workers see
// shared_flow_table.h.
//
// Lifetime of what it returns.
//   State *       valid until that flow is erased (Erase, EraseHandle,
//                 destruction of the table). Creating other flows never
//                 moves it.
//   FlowHandle    names the flow; resolve it with Lookup(handle) each time.
//                 After the flow is erased Lookup returns nullptr, even if
//                 the slot now holds another flow.
//   const Key &   from KeyOf(handle): valid until the flow is erased.
//
// Cost. A lookup is the key's hash (inline), one cache line of the index, a
// 16-bit tag compare, and on a tag match one key compare in the slot record,
// which also holds the generation and the start of the State. See
// docs/flow-state.md for the measured numbers and the bytes per flow.

namespace bess::flow {

// Static configuration of a table beyond its types. Derive and override:
//
//   struct NatTraits : DefaultFlowTableTraits {
//     static constexpr size_t kAliases = 1;       // reverse key
//     static constexpr SlotReuse kReuse = SlotReuse::kFifo;
//     using Observer = MyExpiry;                  // see flow_observer.h
//   };
struct DefaultFlowTableTraits {
  // Alias keys per flow, in addition to the primary key. Each costs one Key of
  // slot storage per flow and one index entry while it is in use; zero costs
  // nothing.
  static constexpr size_t kAliases = 0;
  static constexpr SlotReuse kReuse = SlotReuse::kLifo;
  // FindBatch also prefetches the slot record of each candidate hit. Measured
  // (D-052): 4-8% faster for a batch of all-hit lookups in a table of a
  // million 76-byte flows, but 10-22% slower for misses and up to 13% slower
  // for hits while the table is cache-resident, so it is off by default. Turn
  // it on for large tables whose lookups mostly hit.
  static constexpr bool kPrefetchSlots = false;
  using Observer = NoFlowObserver;
  using Owner = DefaultOwner;
  using Allocator = DefaultFlowAllocator;
};

template <FixedFlowKey Key, typename State,
          typename Hash = DefaultFlowHash<Key>,
          typename Equal = DefaultFlowEqual<Key>,
          typename Traits = DefaultFlowTableTraits>
  requires FlowKeyOps<Key, Hash, Equal> && std::destructible<State> &&
           FlowObserver<typename Traits::Observer, State> &&
           OwnerPolicy<typename Traits::Owner> &&
           FlowAllocator<typename Traits::Allocator>
class WorkerFlowTable {
 public:
  using key_type = Key;
  using state_type = State;
  using Observer = typename Traits::Observer;
  using OwnerPolicyType = typename Traits::Owner;

  static constexpr size_t kAliases = Traits::kAliases;
  static constexpr size_t kMaxBatch = 64;

  static_assert(kAliases <= 14, "alias key bits must fit the slot's key mask");

  // An empty table of exactly `capacity` flows. The only allocation the table
  // ever makes happens here; on failure nothing is left allocated.
  static std::expected<std::unique_ptr<WorkerFlowTable>, FlowTableError>
  Create(size_t capacity, Hash hash = Hash{}, Equal equal = Equal{},
         Observer observer = Observer{}) {
    size_t keys = 0;
    if (capacity == 0 || detail::MulOverflows(capacity, kKeysPerSlot, &keys) ||
        keys >= detail::FlowIndex::kNone) {
      return std::unexpected(FlowTableError::kInvalidCapacity);
    }
    const size_t bucket_count = detail::FlowIndex::BucketsFor(keys);
    if (bucket_count >= detail::FlowIndex::kNone) {
      return std::unexpected(FlowTableError::kInvalidCapacity);
    }
    size_t bucket_bytes = 0, slot_bytes = 0, free_bytes = 0;
    if (detail::MulOverflows(bucket_count, sizeof(detail::Bucket),
                             &bucket_bytes) ||
        detail::MulOverflows(capacity, sizeof(Slot), &slot_bytes) ||
        detail::MulOverflows(capacity, sizeof(uint32_t), &free_bytes)) {
      return std::unexpected(FlowTableError::kTooLarge);
    }
    Block buckets, slots, free_items;
    if (!buckets.Allocate(bucket_bytes, alignof(detail::Bucket)) ||
        !slots.Allocate(slot_bytes, std::max<size_t>(alignof(Slot), 64)) ||
        !free_items.Allocate(free_bytes, alignof(uint32_t))) {
      return std::unexpected(FlowTableError::kOutOfMemory);
    }
    std::unique_ptr<WorkerFlowTable> table(new (std::nothrow) WorkerFlowTable(
        std::move(hash), std::move(equal), std::move(observer)));
    if (table == nullptr) {
      return std::unexpected(FlowTableError::kOutOfMemory);
    }
    // Zero the directory and give every slot generation 0 (free). Touching all
    // of it now is deliberate: the table's memory is committed at creation,
    // not on the first packets.
    std::memset(buckets.get(), 0, bucket_bytes);
    auto *slot_array = static_cast<Slot *>(slots.get());
    for (size_t i = 0; i < capacity; i++) {
      ::new (static_cast<void *>(slot_array + i)) Slot{};
    }
    table->index_ = detail::FlowIndex(
        static_cast<detail::Bucket *>(buckets.get()),
        static_cast<uint32_t>(bucket_count));
    table->slots_ = slot_array;
    table->free_.Init(static_cast<uint32_t *>(free_items.get()),
                      static_cast<uint32_t>(capacity), Traits::kReuse);
    table->capacity_ = static_cast<uint32_t>(capacity);
    table->buckets_block_ = std::move(buckets);
    table->slots_block_ = std::move(slots);
    table->free_block_ = std::move(free_items);
    return table;
  }

  // Destroys every live flow's State (without notifying the observer).
  ~WorkerFlowTable() {
    for (uint32_t slot = 0; slot < capacity_ && slots_ != nullptr; slot++) {
      if (slots_[slot].generation & 1) {
        slots_[slot].state_ptr()->~State();
      }
    }
  }

  WorkerFlowTable(const WorkerFlowTable &) = delete;
  WorkerFlowTable &operator=(const WorkerFlowTable &) = delete;

  // -- lookup -----------------------------------------------------------------

  // The State of the flow `key` names (its primary key or an alias), or
  // nullptr.
  State *Find(const Key &key) noexcept {
    owner_.Check("Find");
    const uint32_t kid = FindKid(Locate(key), key);
    return kid == kNone ? nullptr : slots_[kid / kKeysPerSlot].state_ptr();
  }
  const State *Find(const Key &key) const noexcept {
    owner_.Check("Find");
    const uint32_t kid = FindKid(Locate(key), key);
    return kid == kNone ? nullptr : slots_[kid / kKeysPerSlot].state_ptr();
  }

  // Like Find, and also the flow's handle and whether `key` was an alias.
  FlowRef<State> FindRef(const Key &key) noexcept {
    owner_.Check("FindRef");
    const uint32_t kid = FindKid(Locate(key), key);
    if (kid == kNone) {
      return {};
    }
    Slot &slot = slots_[kid / kKeysPerSlot];
    return {slot.state_ptr(), HandleOf(kid / kKeysPerSlot),
            kid % kKeysPerSlot != 0};
  }

  // Looks up `keys.size()` (<= kMaxBatch) keys. out[i] is the State of
  // keys[i] or nullptr; bit i of the result is set for a hit. The index lines
  // (and with Traits::kPrefetchSlots the slot records) of the whole batch are
  // requested before any is read, so a batch overlaps its cache misses. The
  // keys must stay unchanged until the call returns.
  uint64_t FindBatch(std::span<const Key> keys,
                     std::span<State *> out) noexcept {
    return FindBatchImpl(keys, out);
  }
  uint64_t FindBatch(std::span<const Key> keys,
                     std::span<const State *> out) const noexcept {
    return FindBatchImpl(keys, out);
  }

  // FindBatch that also gives each flow's handle and whether its key was an
  // alias (FindRef for a batch): out[i] is empty for a miss.
  uint64_t FindRefBatch(std::span<const Key> keys, std::span<FlowRef<State>> out) noexcept {
    owner_.Check("FindRefBatch");
    const size_t n = keys.size();
    promise(n <= kMaxBatch);
    promise(out.size() >= n);
    detail::FlowIndex::Hashed hashed[kMaxBatch];
    PrefetchBatch(keys, hashed);
    uint64_t hits = 0;
    for (size_t i = 0; i < n; i++) {
      const uint32_t kid = FindKid(hashed[i], keys[i]);
      if (kid != kNone) {
        Slot &slot = slots_[kid / kKeysPerSlot];
        out[i] = {slot.state_ptr(), HandleOf(kid / kKeysPerSlot), kid % kKeysPerSlot != 0};
        hits |= uint64_t{1} << i;
      } else {
        out[i] = {};
      }
    }
    return hits;
  }

  // Resolves a handle: the flow's State, or nullptr if the handle is stale,
  // forged or empty. Never reaches a flow that reused the slot.
  State *Lookup(FlowHandle handle) noexcept {
    owner_.Check("Lookup");
    const uint32_t slot = SlotOf(handle);
    return slot == kNone ? nullptr : slots_[slot].state_ptr();
  }
  const State *Lookup(FlowHandle handle) const noexcept {
    owner_.Check("Lookup");
    const uint32_t slot = SlotOf(handle);
    return slot == kNone ? nullptr : slots_[slot].state_ptr();
  }

  bool Alive(FlowHandle handle) const noexcept {
    owner_.Check("Alive");
    return SlotOf(handle) != kNone;
  }

  // The flow's primary key, or nullptr for a stale handle.
  const Key *KeyOf(FlowHandle handle) const noexcept {
    owner_.Check("KeyOf");
    const uint32_t slot = SlotOf(handle);
    return slot == kNone ? nullptr : slots_[slot].key_ptr(0);
  }

  // -- create and erase -------------------------------------------------------

  // Creates the flow `key` with State(args...), unless the key is present
  // (then the existing flow is returned, status kExists, and args are not
  // used) or no slot is free (status kFull, state nullptr, nothing changed;
  // the observer hears OnFull). If State's constructor throws, the table is
  // unchanged.
  template <typename... Args>
  EmplaceResult<State> Emplace(const Key &key, Args &&...args) {
    owner_.Check("Emplace");
    return CreateImpl(key, nullptr, std::forward<Args>(args)...);
  }

  // Creates a flow reachable under two keys at once (a forward and a reverse
  // key). Fails with kExists if `key` is present, kAliasExists if `alias` is
  // present or equals `key`, kFull if no slot is free; nothing changes then.
  template <typename... Args>
    requires(kAliases > 0)
  EmplaceResult<State> EmplaceAliased(const Key &key, const Key &alias,
                                      Args &&...args) {
    owner_.Check("EmplaceAliased");
    return CreateImpl(key, &alias, std::forward<Args>(args)...);
  }

  // Makes `alias` name the flow `handle` names. The alias is found by Find
  // like the primary key, and goes when the flow does.
  AliasStatus AddAlias(FlowHandle handle, const Key &alias) noexcept
    requires(kAliases > 0)
  {
    owner_.Check("AddAlias");
    const uint32_t slot = SlotOf(handle);
    if (slot == kNone) {
      return AliasStatus::kStale;
    }
    const auto hashed = Locate(alias);
    if (FindKid(hashed, alias) != kNone) {
      return AliasStatus::kExists;
    }
    Slot &record = slots_[slot];
    size_t j = 1;
    while (j <= kAliases && (record.key_mask >> j & 1)) {
      j++;
    }
    if (j > kAliases) {
      return AliasStatus::kNoRoom;
    }
    record.StoreKey(j, alias);
    const bool inserted = index_.Insert(hashed, slot * kKeysPerSlot + j);
    CHECK(inserted) << "flow index sized for every key cannot be full";
    record.key_mask = static_cast<uint16_t>(record.key_mask | (1u << j));
    return AliasStatus::kAdded;
  }

  // Removes one alias key; the flow and its other keys stay. False if `key`
  // is absent or is a flow's primary key (erase the flow instead).
  bool RemoveAlias(const Key &key) noexcept
    requires(kAliases > 0)
  {
    owner_.Check("RemoveAlias");
    const auto hashed = Locate(key);
    const uint32_t kid = FindKid(hashed, key);
    if (kid == kNone || kid % kKeysPerSlot == 0) {
      return false;
    }
    const bool erased = index_.Erase(hashed, kid);
    DCHECK(erased);
    Slot &record = slots_[kid / kKeysPerSlot];
    record.key_mask = static_cast<uint16_t>(
        record.key_mask & ~(1u << (kid % kKeysPerSlot)));
    return true;
  }

  // Erases the flow `key` names, under its primary key or an alias: the State
  // is destroyed and every key of the flow leaves the table. False if absent.
  bool Erase(const Key &key) noexcept {
    owner_.Check("Erase");
    const uint32_t kid = FindKid(Locate(key), key);
    if (kid == kNone) {
      return false;
    }
    EraseSlot(kid / kKeysPerSlot);
    return true;
  }

  // Erases the flow `handle` names. False -- and nothing happens -- if the
  // handle is stale: an expiry record or queued completion that outlived its
  // flow cannot erase the newer flow that reused the slot.
  bool Erase(FlowHandle handle) noexcept {
    owner_.Check("Erase");
    const uint32_t slot = SlotOf(handle);
    if (slot == kNone) {
      return false;
    }
    EraseSlot(slot);
    return true;
  }

  // Visits every live flow: fn(FlowHandle, const Key &primary, State &). The
  // callback must not create or erase flows. O(capacity).
  template <typename Fn>
  void ForEach(Fn &&fn) {
    owner_.Check("ForEach");
    for (uint32_t slot = 0; slot < capacity_; slot++) {
      if (slots_[slot].generation & 1) {
        fn(HandleOf(slot), *slots_[slot].key_ptr(0), *slots_[slot].state_ptr());
      }
    }
  }

  // -- state of the table -----------------------------------------------------

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  // Capacity minus slots retired because their generation was exhausted (see
  // docs/flow-state.md; 2^31 reuses of one slot).
  size_t usable_capacity() const noexcept { return capacity_ - quarantined_; }
  size_t quarantined_slots() const noexcept { return quarantined_; }
  bool full() const noexcept { return free_.empty(); }
  Observer &observer() noexcept { return observer_; }
  const Observer &observer() const noexcept { return observer_; }

  // Bytes the table holds for its slots, directory and free list; its
  // per-flow cost at full occupancy is memory_bytes() / capacity().
  size_t memory_bytes() const noexcept {
    return buckets_block_.bytes() + slots_block_.bytes() +
           free_block_.bytes() + sizeof(*this);
  }
  static constexpr size_t slot_bytes() noexcept { return sizeof(Slot); }
  const detail::FlowIndex &index() const noexcept { return index_; }

  // The next Check() binds whoever calls it as the owner.
  void ReleaseOwner() noexcept { owner_.Release("ReleaseOwner"); }
  OwnerToken owner() const noexcept { return owner_.owner(); }

  // Testing only: sets the generation of a free slot (even), to reach the
  // wrap-around without 2^31 reuses.
  void SetFreeSlotGenerationForTesting(uint32_t slot, uint32_t generation) {
    CHECK_LT(slot, capacity_);
    CHECK_EQ(0u, slots_[slot].generation & 1);
    CHECK_EQ(0u, generation & 1);
    slots_[slot].generation = generation;
  }

 private:
  static constexpr size_t kKeysPerSlot = 1 + kAliases;
  static constexpr uint32_t kNone = detail::FlowIndex::kNone;

  using Slot = detail::SlotRecord<Key, State, kAliases, false, detail::NoExtra>;
  using Block = detail::Block<typename Traits::Allocator>;

  WorkerFlowTable(Hash hash, Equal equal, Observer observer)
      : hash_(std::move(hash)),
        equal_(std::move(equal)),
        observer_(std::move(observer)) {}

  // Where `key` lives in the index.
  detail::FlowIndex::Hashed Locate(const Key &key) const noexcept {
    return index_.Split(hash_(key));
  }

  // The key id (slot * kKeysPerSlot + key index) for `key`, or kNone.
  uint32_t FindKid(detail::FlowIndex::Hashed hashed,
                   const Key &key) const noexcept {
    return index_.Find(hashed, [&](uint32_t kid) noexcept {
      return equal_(*slots_[kid / kKeysPerSlot].key_ptr(kid % kKeysPerSlot),
                    key);
    });
  }

  FlowHandle HandleOf(uint32_t slot) const noexcept {
    return {FlowId(slot + 1), slots_[slot].generation};
  }

  // The slot a live handle names, else kNone.
  uint32_t SlotOf(FlowHandle handle) const noexcept {
    const uint32_t slot = handle.id.value() - 1;  // an empty id wraps high
    if (slot >= capacity_) {
      return kNone;
    }
    const uint32_t generation = slots_[slot].generation;
    return (generation == handle.generation && (generation & 1)) ? slot : kNone;
  }

  template <typename... Args>
  EmplaceResult<State> CreateImpl(const Key &key, const Key *alias,
                                  Args &&...args) {
    const auto hashed = Locate(key);
    if (const uint32_t kid = FindKid(hashed, key); kid != kNone) {
      const uint32_t slot = kid / kKeysPerSlot;
      return {slots_[slot].state_ptr(), HandleOf(slot), EmplaceStatus::kExists};
    }
    detail::FlowIndex::Hashed alias_hashed{};
    if (alias != nullptr) {
      if (equal_(key, *alias)) {
        return {nullptr, {}, EmplaceStatus::kAliasExists};
      }
      alias_hashed = Locate(*alias);
      if (FindKid(alias_hashed, *alias) != kNone) {
        return {nullptr, {}, EmplaceStatus::kAliasExists};
      }
    }
    if (free_.empty()) {
      observer_.OnFull();
      return {nullptr, {}, EmplaceStatus::kFull};
    }
    const uint32_t slot_number = free_.Peek();
    Slot &slot = slots_[slot_number];
    // Construct first: if the constructor throws, nothing else has changed.
    State *state = ::new (static_cast<void *>(slot.state))
        State(std::forward<Args>(args)...);
    free_.Pop();
    slot.StoreKey(0, key);
    slot.key_mask = 1;
    const bool primary =
        index_.Insert(hashed, slot_number * kKeysPerSlot);
    CHECK(primary) << "flow index sized for every key cannot be full";
    if (alias != nullptr) {
      slot.StoreKey(1, *alias);
      slot.key_mask = 3;
      const bool second =
          index_.Insert(alias_hashed, slot_number * kKeysPerSlot + 1);
      CHECK(second) << "flow index sized for every key cannot be full";
    }
    slot.generation = slot.generation + 1;  // even -> odd: live
    size_++;
    const FlowHandle handle = HandleOf(slot_number);
    observer_.OnCreate(handle, *state);
    return {state, handle, EmplaceStatus::kCreated};
  }

  void EraseSlot(uint32_t slot_number) noexcept {
    Slot &slot = slots_[slot_number];
    State *state = slot.state_ptr();
    observer_.OnErase(HandleOf(slot_number), *state);
    for (size_t j = 0; j < kKeysPerSlot; j++) {
      if (slot.key_mask >> j & 1) {
        const bool erased =
            index_.Erase(Locate(*slot.key_ptr(j)),
                         slot_number * kKeysPerSlot + static_cast<uint32_t>(j));
        DCHECK(erased) << "a live flow's key is missing from the index";
      }
    }
    slot.key_mask = 0;
    state->~State();
    const uint32_t next = slot.generation + 1;  // odd -> even: free
    slot.generation = next;
    size_--;
    if (next == 0) [[unlikely]] {
      // The generation wrapped: the slot's first lifetime's handles would
      // match its next. Retire the slot instead of reusing it.
      quarantined_++;
    } else {
      free_.Push(slot_number);
    }
  }

  template <typename Out>
  uint64_t FindBatchImpl(std::span<const Key> keys,
                         std::span<Out *> out) const noexcept {
    owner_.Check("FindBatch");
    const size_t n = keys.size();
    promise(n <= kMaxBatch);
    promise(out.size() >= n);
    detail::FlowIndex::Hashed hashed[kMaxBatch];
    for (size_t i = 0; i < n; i++) {
      hashed[i] = Locate(keys[i]);
      index_.Prefetch(hashed[i]);
    }
    if constexpr (Traits::kPrefetchSlots) {
      for (size_t i = 0; i < n; i++) {
        const uint32_t kid = index_.FirstCandidate(hashed[i]);
        if (kid != kNone) {
          const Slot *slot = &slots_[kid / kKeysPerSlot];
          __builtin_prefetch(slot, 0, 3);
          __builtin_prefetch(slot->state, 0, 3);
        }
      }
    }
    uint64_t hits = 0;
    for (size_t i = 0; i < n; i++) {
      const uint32_t kid = FindKid(hashed[i], keys[i]);
      if (kid != kNone) {
        out[i] = slots_[kid / kKeysPerSlot].state_ptr();
        hits |= uint64_t{1} << i;
      } else {
        out[i] = nullptr;
      }
    }
    return hits;
  }

  // FindBatchImpl's prefetch phase, for FindRefBatch. FindBatchImpl keeps its
  // own copy: sharing it (or a callback loop) measured FindBatch 6-9% slower
  // on in-cache tables.
  void PrefetchBatch(std::span<const Key> keys, detail::FlowIndex::Hashed *hashed) const noexcept {
    const size_t n = keys.size();
    for (size_t i = 0; i < n; i++) {
      hashed[i] = Locate(keys[i]);
      index_.Prefetch(hashed[i]);
    }
    if constexpr (Traits::kPrefetchSlots) {
      for (size_t i = 0; i < n; i++) {
        const uint32_t kid = index_.FirstCandidate(hashed[i]);
        if (kid != kNone) {
          const Slot *slot = &slots_[kid / kKeysPerSlot];
          __builtin_prefetch(slot, 0, 3);
          __builtin_prefetch(slot->state, 0, 3);
        }
      }
    }
  }

  [[no_unique_address]] Hash hash_;
  [[no_unique_address]] Equal equal_;
  [[no_unique_address]] Observer observer_;
  OwnerGuard<typename Traits::Owner> owner_;
  detail::FlowIndex index_;
  Slot *slots_ = nullptr;
  detail::FreeList free_;
  uint32_t capacity_ = 0;
  uint32_t size_ = 0;
  uint32_t quarantined_ = 0;
  Block buckets_block_, slots_block_, free_block_;
};

}  // namespace bess::flow

#endif  // BESS_FLOW_WORKER_FLOW_TABLE_H_
