// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_FLOW_STORAGE_H_
#define BESS_FLOW_FLOW_STORAGE_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "flow/flow_types.h"

namespace bess::flow {

// Where a flow table gets its memory. A table allocates in its factory and
// nowhere else; this is the one seam for that allocation (a test that fails
// the Nth allocation, a hugepage or NUMA-aware allocator later).
template <typename A>
concept FlowAllocator = requires(size_t bytes, size_t align, void *p) {
  { A::Allocate(bytes, align) } noexcept -> std::same_as<void *>;
  { A::Deallocate(p, bytes, align) } noexcept;
};

// Aligned `operator new`, returning nullptr instead of throwing.
struct DefaultFlowAllocator {
  static void *Allocate(size_t bytes, size_t align) noexcept {
    return ::operator new(bytes, std::align_val_t(align), std::nothrow);
  }
  static void Deallocate(void *p, size_t, size_t align) noexcept {
    ::operator delete(p, std::align_val_t(align));
  }
};

namespace detail {

// One allocation, released on destruction.
template <FlowAllocator Allocator>
class Block {
 public:
  Block() = default;
  Block(Block &&other) noexcept
      : p_(std::exchange(other.p_, nullptr)),
        bytes_(other.bytes_),
        align_(other.align_) {}
  Block &operator=(Block &&other) noexcept {
    if (this != &other) {
      Reset();
      p_ = std::exchange(other.p_, nullptr);
      bytes_ = other.bytes_;
      align_ = other.align_;
    }
    return *this;
  }
  Block(const Block &) = delete;
  Block &operator=(const Block &) = delete;
  ~Block() { Reset(); }

  // False if the allocator refused (the block stays empty).
  bool Allocate(size_t bytes, size_t align) noexcept {
    Reset();
    p_ = Allocator::Allocate(bytes, align);
    bytes_ = bytes;
    align_ = align;
    return p_ != nullptr;
  }

  void *get() const noexcept { return p_; }
  size_t bytes() const noexcept { return p_ != nullptr ? bytes_ : 0; }

 private:
  void Reset() noexcept {
    if (p_ != nullptr) {
      Allocator::Deallocate(p_, bytes_, align_);
      p_ = nullptr;
    }
  }

  void *p_ = nullptr;
  size_t bytes_ = 0;
  size_t align_ = 0;
};

// a*b, or false on overflow.
inline bool MulOverflows(size_t a, size_t b, size_t *out) noexcept {
  return __builtin_mul_overflow(a, b, out);
}
inline bool AddOverflows(size_t a, size_t b, size_t *out) noexcept {
  return __builtin_add_overflow(a, b, out);
}

// Stands in for "no extra per-slot field" (it occupies no space).
struct NoExtra {};

// One flow's slot: its keys, its generation, and its state, in one
// contiguous record so that the key compare and the state access that follows
// a hit share cache lines. Key 0 is the primary key; keys 1..A are aliases.
//
// The generation is odd while the slot holds a live flow and even while it is
// free. It advances at creation and again at erasure, so a handle (always
// odd) matches only the lifetime it was issued for.
//
// Keys and state live in raw storage: the state is constructed in place by
// Emplace and destroyed by Erase, never copied or moved, so a State needs
// neither to be movable nor to be default-constructible, and its address is
// stable for the flow's life.
template <typename Key, typename State, size_t A, bool kAtomicGeneration,
          typename Extra>
struct alignas(std::max({alignof(Key), alignof(State), alignof(uint64_t)}))
    SlotRecord {
  using Generation =
      std::conditional_t<kAtomicGeneration, std::atomic<uint32_t>, uint32_t>;

  alignas(Key) std::byte keys[sizeof(Key) * (1 + A)];
  Generation generation;
  uint16_t key_mask;  // bit j: keys[j] is in the directory
  [[no_unique_address]] Extra extra;
  alignas(State) std::byte state[sizeof(State)];

  Key *key_ptr(size_t j) noexcept {
    return std::launder(reinterpret_cast<Key *>(keys + j * sizeof(Key)));
  }
  const Key *key_ptr(size_t j) const noexcept {
    return std::launder(reinterpret_cast<const Key *>(keys + j * sizeof(Key)));
  }
  State *state_ptr() noexcept {
    return std::launder(reinterpret_cast<State *>(state));
  }
  const State *state_ptr() const noexcept {
    return std::launder(reinterpret_cast<const State *>(state));
  }
  void StoreKey(size_t j, const Key &key) noexcept {
    ::new (static_cast<void *>(keys + j * sizeof(Key))) Key(key);
  }

  uint32_t LoadGeneration() const noexcept {
    if constexpr (kAtomicGeneration) {
      return generation.load(std::memory_order_acquire);
    } else {
      return generation;
    }
  }
  // Writers only (the slot's generation has a single writer at a time).
  void StoreGeneration(uint32_t g) noexcept {
    if constexpr (kAtomicGeneration) {
      generation.store(g, std::memory_order_release);
    } else {
      generation = g;
    }
  }
};

// The slots not holding a flow. LIFO is a stack; FIFO is a ring. Both are a
// preallocated array of slot numbers: no allocation after construction.
class FreeList {
 public:
  FreeList() = default;
  // `storage` holds `capacity` slot numbers; every slot starts free, handed
  // out in ascending order.
  void Init(uint32_t *storage, uint32_t capacity, SlotReuse reuse) noexcept {
    items_ = storage;
    capacity_ = capacity;
    reuse_ = reuse;
    head_ = 0;
    count_ = capacity;
    if (reuse == SlotReuse::kLifo) {
      for (uint32_t i = 0; i < capacity; i++) {
        items_[i] = capacity - 1 - i;  // the top is slot 0
      }
    } else {
      for (uint32_t i = 0; i < capacity; i++) {
        items_[i] = i;
      }
    }
  }

  bool empty() const noexcept { return count_ == 0; }
  uint32_t size() const noexcept { return count_; }

  // The slot Pop() would return (not empty).
  uint32_t Peek() const noexcept {
    return reuse_ == SlotReuse::kLifo ? items_[count_ - 1] : items_[head_];
  }
  void Pop() noexcept {
    if (reuse_ == SlotReuse::kLifo) {
      count_--;
    } else {
      head_ = (head_ + 1 == capacity_) ? 0 : head_ + 1;
      count_--;
    }
  }
  // Frees `slot`; room always exists because only slots taken from the list
  // are returned to it.
  void Push(uint32_t slot) noexcept {
    if (reuse_ == SlotReuse::kLifo) {
      items_[count_++] = slot;
    } else {
      uint32_t tail = head_ + count_;
      if (tail >= capacity_) {
        tail -= capacity_;
      }
      items_[tail] = slot;
      count_++;
    }
  }

 private:
  uint32_t *items_ = nullptr;
  uint32_t capacity_ = 0;
  uint32_t head_ = 0;
  uint32_t count_ = 0;
  SlotReuse reuse_ = SlotReuse::kLifo;
};

}  // namespace detail
}  // namespace bess::flow

#endif  // BESS_FLOW_FLOW_STORAGE_H_
