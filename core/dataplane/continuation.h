// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_CONTINUATION_H_
#define BESS_DATAPLANE_CONTINUATION_H_

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include <glog/logging.h>

#include "dataplane/generation_handle.h"
#include "dataplane/strong_id.h"

// Continuations: the token a handed-off packet carries so that whoever resumes
// it can find, safely, where it was going (roadmap M11, Decision D-054; guide in
// docs/handoff.md). Experimental API.
//
// A continuation is not a pointer into the graph and not a bare integer. A
// packet parked in a queue, a service thread, or a crypto engine outlives the
// structural state it was headed for: the module can be destroyed, the scope
// replaced, the port removed while the packet is away. `ContinuationHandle` is
// an id plus the generation of the slot it named when it was issued, so the owner
// can see that the slot has since been retired (and maybe reused) and fail
// closed instead of acting on the new occupant (docs/architecture.md section 7).
// An asynchronous completion therefore never depends on RCU quiescence of the
// thread that issued the handle: a resume that comes from an accelerator or a
// service thread that is not an RCU reader is checked by generation alone.
//
// What the token means is the application's: `Target` is any trivially copyable
// value up to 64 bytes (an `InterfaceId` and a stage number; an index into the
// application's own RCU-protected table). The table copies it in on `Issue` and
// out on `Resolve`; it never holds a pointer to anything the application owns.
//
// Mechanism. A fixed array of slots, allocated once in `Create`, each holding a
// 32-bit generation (odd while the slot is live, even while it is free) and the
// target. `Issue` takes the slot freed longest ago (a FIFO free list), so a
// slot's generation advances 1/capacity as fast as it would under LIFO reuse:
// the right policy for handles that live in queues. `Retire` ends the
// continuation: it advances the generation, which kills every handle to it at
// once, whatever has been parked with them.
//
// Threads. One thread at a time may `Issue` and `Retire` (they are not atomic
// with each other: the owner serialises them, typically by being one worker);
// any number of threads may `Resolve` concurrently with them and with each
// other, lock-free and without a read-modify-write. Resolve reads the
// generation, copies the target, and reads the generation again: a slot
// retired (or retired and reused) in between fails the check, so the caller
// gets a complete target that was live for the whole read or nothing. The
// target words are atomic, so there is no data race even then.
//
// A handle is forged if its id is out of range, zero, or its generation is
// even or not the slot's current one. All of them resolve to nothing and
// retire nothing. A table must outlive every thread that can resolve against
// it.
//
// A slot whose generation would wrap (2^31 continuations through one slot) is
// retired for good and lowers `capacity()`'s usable share (`quarantined()`):
// after the wrap a handle of the first life would match the next.

namespace bess::dataplane {

// Names one continuation slot. One-based: zero is "no continuation".
struct ContinuationIdTag;
using ContinuationId = StrongId<ContinuationIdTag, uint32_t>;

// A ContinuationId plus the generation of its slot when it was issued: one
// 64-bit word, one 64-bit compare. It resolves only while that continuation is
// live.
using ContinuationHandle = GenerationHandle<ContinuationId>;

// The handle of no continuation; also what `Issue` returns for a full table.
inline constexpr ContinuationHandle kNoContinuation{};

static_assert(sizeof(ContinuationId) == sizeof(uint32_t));
static_assert(std::is_trivially_copyable_v<ContinuationId>);
static_assert(std::is_standard_layout_v<ContinuationId>);
static_assert(sizeof(ContinuationHandle) == sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<ContinuationHandle>);
static_assert(!std::is_convertible_v<uint32_t, ContinuationId>,
              "an integer must not convert to a ContinuationId implicitly");
static_assert(!std::is_convertible_v<ContinuationId, uint32_t>,
              "a ContinuationId must not convert to an integer implicitly");

enum class ContinuationError : uint8_t {
  kInvalidCapacity,  // zero, or more slots than 32-bit ids can name
  kOutOfMemory,      // the allocator refused
};

inline const char *ToString(ContinuationError error) noexcept {
  switch (error) {
    case ContinuationError::kInvalidCapacity:
      return "invalid continuation table capacity";
    case ContinuationError::kOutOfMemory:
      return "out of memory creating the continuation table";
  }
  return "unknown continuation table error";
}

template <typename Target>
  requires std::is_trivially_copyable_v<Target> && (sizeof(Target) <= 64)
class ContinuationTable {
 public:
  using target_type = Target;

  // An empty table for up to `capacity` live continuations. The only
  // allocation it ever makes happens here.
  static std::expected<std::unique_ptr<ContinuationTable>, ContinuationError>
  Create(size_t capacity) noexcept {
    if (capacity == 0 || capacity >= std::numeric_limits<uint32_t>::max()) {
      return std::unexpected(ContinuationError::kInvalidCapacity);
    }
    const size_t count = capacity + 1;  // slot 0 is the nil sentinel
    if (count > std::numeric_limits<size_t>::max() / sizeof(Slot)) {
      return std::unexpected(ContinuationError::kInvalidCapacity);
    }
    void *memory = ::operator new(count * sizeof(Slot), std::align_val_t(64),
                                  std::nothrow);
    if (memory == nullptr) {
      return std::unexpected(ContinuationError::kOutOfMemory);
    }
    Slots slots(static_cast<Slot *>(memory));
    std::unique_ptr<ContinuationTable> table(
        new (std::nothrow) ContinuationTable(std::move(slots), capacity));
    if (table == nullptr) {
      return std::unexpected(ContinuationError::kOutOfMemory);
    }
    return table;
  }

  ContinuationTable(const ContinuationTable &) = delete;
  ContinuationTable &operator=(const ContinuationTable &) = delete;

  // -- the owner's side (one thread at a time) ------------------------------------

  // Starts a continuation that resolves to `target`. Returns kNoContinuation,
  // changing nothing, when the table holds `capacity` live continuations.
  ContinuationHandle Issue(const Target &target) noexcept {
    if (free_head_ == kNil) {
      return kNoContinuation;
    }
    const uint32_t index = free_head_;
    Slot &slot = slots_[index];
    free_head_ = slot.next_free;
    if (free_head_ == kNil) {
      free_tail_ = kNil;
    }
    StoreTarget(slot, target);
    const uint32_t generation =
        slot.generation.load(std::memory_order_relaxed) + 1;  // even -> odd
    slot.generation.store(generation, std::memory_order_release);
    size_++;
    return {ContinuationId(index), generation};
  }

  // Ends the continuation: every copy of `handle`, wherever it is parked, stops
  // resolving. True exactly once per issued handle; false (and nothing
  // happens) for a handle that was already retired, never issued, or forged.
  bool Retire(ContinuationHandle handle) noexcept {
    Slot *slot = Live(handle);
    if (slot == nullptr) {
      return false;
    }
    const uint32_t generation =
        slot->generation.load(std::memory_order_relaxed) + 1;  // odd -> even
    slot->generation.store(generation, std::memory_order_release);
    size_--;
    if (generation == 0) [[unlikely]] {
      // Wrapped: a handle of this slot's first life would match its next.
      quarantined_++;
      return true;
    }
    const uint32_t index = handle.id.value();
    slot->next_free = kNil;
    if (free_tail_ == kNil) {
      free_head_ = index;
    } else {
      slots_[free_tail_].next_free = index;
    }
    free_tail_ = index;
    return true;
  }

  // -- anyone's side (any thread, concurrently with the owner) --------------------

  // The target of a live continuation, or nullopt (a retired, never-issued or
  // forged handle). The target returned was live for the whole read.
  std::optional<Target> Resolve(ContinuationHandle handle) const noexcept {
    const uint32_t index = handle.id.value();
    if (index == 0 || index > capacity_ || (handle.generation & 1) == 0) {
      return std::nullopt;
    }
    const Slot &slot = slots_[index];
    if (slot.generation.load(std::memory_order_acquire) != handle.generation) {
      return std::nullopt;
    }
    const Target target = LoadTarget(slot);
    // The target words were read with acquire: this read cannot be hoisted
    // above them. A retire (and any reuse) that raced the copy changed it.
    if (slot.generation.load(std::memory_order_acquire) != handle.generation) {
      return std::nullopt;
    }
    return target;
  }

  // -- state (owner's thread) ------------------------------------------------------

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  bool full() const noexcept { return free_head_ == kNil; }
  // Slots retired for good because their generation was exhausted.
  size_t quarantined() const noexcept { return quarantined_; }
  // Bytes of table memory; the per-continuation cost is memory_bytes() /
  // capacity().
  size_t memory_bytes() const noexcept {
    return (capacity_ + size_t{1}) * sizeof(Slot) + sizeof(*this);
  }
  static constexpr size_t slot_bytes() noexcept { return sizeof(Slot); }

  // Testing only: sets the (even) generation of the slot the next Issue will
  // use, to reach the wrap-around without 2^31 continuations.
  void SetNextSlotGenerationForTesting(uint32_t generation) noexcept {
    CHECK_NE(free_head_, kNil);
    CHECK_EQ(0u, generation & 1);
    slots_[free_head_].generation.store(generation, std::memory_order_relaxed);
  }

 private:
  static constexpr uint32_t kNil = 0;  // free list end and the unused slot 0
  static constexpr size_t kWords = (sizeof(Target) + 7) / 8;

  struct Slot {
    // Odd while live, even while free. Everything a handle must match.
    std::atomic<uint32_t> generation{0};
    uint32_t next_free = kNil;  // owner only: the free list link while free
    std::atomic<uint64_t> words[kWords]{};
  };

  struct Deleter {
    void operator()(Slot *p) const noexcept {
      ::operator delete(p, std::align_val_t(64));
    }
  };
  using Slots = std::unique_ptr<Slot[], Deleter>;

  ContinuationTable(Slots slots, size_t capacity)
      : slots_(std::move(slots)), capacity_(static_cast<uint32_t>(capacity)) {
    for (uint32_t i = 0; i <= capacity_; i++) {
      new (&slots_[i]) Slot();
    }
    for (uint32_t i = 1; i <= capacity_; i++) {
      slots_[i].next_free = (i == capacity_) ? kNil : i + 1;
    }
    free_head_ = 1;
    free_tail_ = capacity_;
  }

  // The live slot for the handle, or nullptr (owner's side).
  Slot *Live(ContinuationHandle handle) noexcept {
    const uint32_t index = handle.id.value();
    if (index == 0 || index > capacity_) {
      return nullptr;
    }
    Slot &slot = slots_[index];
    const uint32_t generation = slot.generation.load(std::memory_order_relaxed);
    return (generation == handle.generation && (generation & 1)) ? &slot
                                                                 : nullptr;
  }

  // The words are stored with release so that a reader that sees one also sees
  // the retirement that preceded it (see Resolve).
  static void StoreTarget(Slot &slot, const Target &target) noexcept {
    std::array<std::byte, kWords * 8> bytes{};
    std::memcpy(bytes.data(), &target, sizeof(Target));
    for (size_t i = 0; i < kWords; i++) {
      uint64_t word;
      std::memcpy(&word, bytes.data() + i * 8, 8);
      slot.words[i].store(word, std::memory_order_release);
    }
  }

  static Target LoadTarget(const Slot &slot) noexcept {
    std::array<std::byte, sizeof(Target)> exact;
    std::array<std::byte, kWords * 8> bytes;
    for (size_t i = 0; i < kWords; i++) {
      const uint64_t word = slot.words[i].load(std::memory_order_acquire);
      std::memcpy(bytes.data() + i * 8, &word, 8);
    }
    std::memcpy(exact.data(), bytes.data(), sizeof(Target));
    return std::bit_cast<Target>(exact);
  }

  Slots slots_;
  const uint32_t capacity_;
  uint32_t free_head_ = kNil;
  uint32_t free_tail_ = kNil;
  uint32_t size_ = 0;
  uint32_t quarantined_ = 0;
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_CONTINUATION_H_
