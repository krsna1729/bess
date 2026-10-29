// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_DPDK_SLAB_H_
#define BESS_UTILS_DPDK_SLAB_H_

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <rte_memory.h>

#include "utils/dpdk_memory.h"

namespace bess::utils {

template <typename T>
class ObjectSlab;

// Destroys a slab object and returns its slot to the slab that made it. One
// pointer, trivially copyable: RcuDomain::Retire carries it in its retire
// record, so a slab object retires like any other.
template <typename T>
struct SlabDelete {
  ObjectSlab<std::remove_const_t<T>> *slab = nullptr;

  SlabDelete() = default;
  explicit SlabDelete(ObjectSlab<std::remove_const_t<T>> *s) noexcept
      : slab(s) {}
  // SlabUnique<T> -> SlabUnique<const T>.
  template <typename U, typename = std::enable_if_t<std::is_same_v<
                            std::remove_const_t<U>, std::remove_const_t<T>>>>
  SlabDelete(const SlabDelete<U> &o) noexcept : slab(o.slab) {}

  void operator()(T *p) const noexcept { slab->Destroy(p); }
};

// An owning pointer to an object of an ObjectSlab.
template <typename T>
using SlabUnique = std::unique_ptr<T, SlabDelete<T>>;

// Typed fixed-stride objects in DPDK memory (Decision D-029): packet-path
// objects allocated one at a time on the control path -- SlotTable objects,
// meter states -- without one rte_malloc per object, whose header and
// cache-line rounding would cost more than a small object.
//
// Objects live in chunks of kObjectsPerChunk slots from DpdkAllocate (so the
// memory policy applies: the memory node, the normal-page fallback, the
// process memory limit). A chunk is never moved or freed while the slab
// lives, so an object's address is stable and a reader may hold it until a
// grace period; freeing an object only after that grace period is the
// owner's job (the slab knows nothing of RCU). A freed slot goes on an
// intrusive LIFO free list threaded through the slot itself, so Free never
// allocates.
//
// Lifetime. A slab is made by Create() and held through Owner (or a
// shared_ptr built from one). Dropping the owner releases the slab, which
// deletes itself, chunks and all, when its last live object is freed (at
// once if none is): objects may outlive their owner -- a SlotTable's retired
// objects wait in the RCU queue or in a pending transaction operation.
//
// Control path only. Make/Allocate/Free lock, because objects are freed by
// whichever control thread reclaims RCU retirements, concurrently with the
// owner's writer.
//
// Memory (D-198): Create allocates no chunk (Bytes() is 0); each chunk is a
// growth of chunk_bytes(), admitted through the owner's growth hook
// (set_memory_growth) before it is allocated. Chunks are never given back
// while the slab lives, so nothing shrinks but a refused allocation; the
// owner's admission goes with the owner.
template <typename T>
class ObjectSlab {
 public:
  static constexpr size_t kObjectsPerChunk = 1024;

  // Slot size: room for T or the free-list link, aligned for both.
  static constexpr size_t Stride() noexcept {
    constexpr size_t align = std::max(alignof(T), alignof(void *));
    return (std::max(sizeof(T), sizeof(void *)) + align - 1) / align * align;
  }

  // Deletes the slab instead of destroying it: see Lifetime above.
  struct Releaser {
    void operator()(ObjectSlab *slab) const noexcept { slab->Release(); }
  };
  using Owner = std::unique_ptr<ObjectSlab, Releaser>;

  static Owner Create(int socket = MemoryNode()) {
    return Owner(new ObjectSlab(socket));
  }

  // What Create allocates in DPDK memory: no chunk (D-198). The slab object
  // itself is not counted, as bytes() does not count it.
  static constexpr size_t Bytes(int /*socket*/ = MemoryNode()) noexcept {
    return 0;
  }
  // DPDK bytes one chunk adds.
  static constexpr size_t chunk_bytes() noexcept {
    return kObjectsPerChunk * Stride();
  }

  // The admission each new chunk asks first (D-198); nullptr: unadmitted.
  // The hook must outlive every Allocate (the owner's lease does: nothing
  // is allocated after Release).
  void set_memory_growth(MemoryGrowth *growth) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    growth_ = growth;
  }

  ObjectSlab(const ObjectSlab &) = delete;
  ObjectSlab &operator=(const ObjectSlab &) = delete;

  // The owner is done with the slab (Owner's deleter): it deletes itself now
  // if no object is live, else when the last one is freed. Nothing may be
  // allocated from it afterwards.
  void Release() noexcept {
    bool last;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
      last = live_ == 0;
    }
    if (last) {
      delete this;
    }
  }

  // Builds a T in a free slot. Throws what DpdkAllocate throws (MemoryRefused,
  // std::bad_alloc) or what T's constructor throws; the slot is not lost.
  template <typename... Args>
  SlabUnique<T> Make(Args &&...args) {
    void *slot = Allocate();
    try {
      return SlabUnique<T>(new (slot) T(std::forward<Args>(args)...),
                           SlabDelete<T>(this));
    } catch (...) {
      Free(slot);
      throw;
    }
  }

  // A raw slot of Stride() bytes aligned for T, for a type built by its own
  // factory (MeterState::ConstructAt); give it back with Free. Throws as
  // DpdkAllocate does, or what the growth hook's Grow throws (MemoryRefused:
  // no chunk is added).
  void *Allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    live_++;
    if (free_ != nullptr) {
      return std::exchange(free_, free_->next);
    }
    if (used_ == kObjectsPerChunk) {
      constexpr size_t align =
          std::max<size_t>(alignof(T), RTE_CACHE_LINE_SIZE);
      bool admitted = false;
      try {
        chunks_.reserve(chunks_.size() + 1);  // push_back below cannot throw
        if (growth_ != nullptr) {
          growth_->Grow(chunk_bytes());
          admitted = true;
        }
        chunks_.push_back(DpdkAllocate(chunk_bytes(), align, socket_));
      } catch (...) {
        if (admitted) {
          growth_->Shrink(chunk_bytes());
        }
        live_--;
        throw;
      }
      used_ = 0;
    }
    return static_cast<std::byte *>(chunks_.back()) + used_++ * Stride();
  }

  // Returns a slot whose object is already destroyed (or never built). The
  // last free of a released slab deletes it: `this` is not touched after.
  void Free(void *slot) noexcept {
    bool last;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      free_ = new (slot) FreeSlot{free_};
      live_--;
      last = released_ && live_ == 0;
    }
    if (last) {
      delete this;
    }
  }

  void Destroy(const T *object) noexcept {
    object->~T();
    Free(const_cast<T *>(object));
  }

  // DPDK memory held: whole chunks, used or not.
  size_t bytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.size() * chunk_bytes();
  }
  // Slots handed out and not freed.
  size_t live() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_;
  }

 private:
  // Only Create() makes one and only the slab itself (Release, the last
  // Free) deletes it.
  explicit ObjectSlab(int socket) : socket_(socket) {}
  ~ObjectSlab() {
    for (void *chunk : chunks_) {
      DpdkFree(chunk);
    }
  }

  struct FreeSlot {
    FreeSlot *next;
  };

  const int socket_;
  mutable std::mutex mutex_;
  MemoryGrowth *growth_ = nullptr;  // D-198: admits each chunk
  // A pointer per kObjectsPerChunk objects; control side only.
  std::vector<void *> chunks_;
  FreeSlot *free_ = nullptr;
  size_t used_ = kObjectsPerChunk;  // slots carved from the newest chunk
  size_t live_ = 0;
  bool released_ = false;  // the owner let go (Release)
};

}  // namespace bess::utils

#endif  // BESS_UTILS_DPDK_SLAB_H_
