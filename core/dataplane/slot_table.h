// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_SLOT_TABLE_H_
#define BESS_DATAPLANE_SLOT_TABLE_H_

#include <atomic>
#include <cstddef>
#include <utility>

#include "utils/logging.h"

#include "utils/common.h"
#include "utils/dpdk_memory.h"
#include "utils/dpdk_slab.h"

namespace bess {
namespace dataplane {

// Id -> immutable object, changed one object at a time while workers read
// (G1.2 mode C). Decision D-021 (docs/decisions.md).
//
//   reader:  const T *obj = table.Lookup(id);   // one acquire load
//   writer:  old = table.Publish(id, std::move(obj));   // one release store
//            retire `old` after a grace period
//
// This is ObjectTable (K2) without the rebuild: ObjectTable publishes a whole
// new generation per change, O(capacity); here a change is one pointer store
// and the object it replaced is retired through RCU. Objects are immutable
// once published -- a change publishes a new object -- so a reader holding a
// pointer keeps a consistent object until its next quiescent state.
//
// Ids are one-based (zero is invalid) and chosen by the caller.
//
// Removal has two steps, because a reader may hold an id it read from a
// referrer's old value:
//
//   Retire(id)     the id is absent on the control side at once, but its
//                  object stays published (readable) and the id cannot be
//                  published again;
//   Unpublish(id)  later, empties the slot and returns the object, to be
//                  freed after one more grace period; the id is free again.
//
// When Unpublish may run is the owner's decision: only once no reader can
// still obtain the id from any referrer. The transaction engine decides it
// with a dependency-ordered cascade of grace periods (Decision D-021).
//
// One writer at a time; the caller serializes (the transaction engine does).
//
// Memory (Decision D-029): the slot array and the objects are packet-path
// memory, so both come from DPDK's heap on the memory node -- the array in
// one allocation, the objects from a slab the table owns (Make). An object
// is owned by `Owned`, whose deleter returns it to that slab. Objects the
// table handed out may outlive it -- the retired ones Publish and Unpublish
// return wait in the RCU queue, an unpublished one in a pending transaction
// operation -- as they could when they were plain heap objects: the table
// releases its slab rather than destroying it, and the slab goes with the
// last of them (utils::ObjectSlab::Owner).
template <typename Id, typename T>
class SlotTable {
 public:
  enum class SlotState : uint8_t { kFree, kLive, kRetiring };
  using Owned = utils::SlabUnique<const T>;

  // Ids 1..capacity.
  explicit SlotTable(size_t capacity)
      : objects_(utils::ObjectSlab<T>::Create()),
        slots_(capacity + 1),
        states_(capacity + 1),
        capacity_(capacity) {}

  // What the constructor allocates in DPDK memory (D-198): the slot array
  // and the states; the slab starts without a chunk. Objects are growth
  // (set_memory_growth).
  static size_t Bytes(size_t capacity) noexcept {
    return (capacity + 1) * (sizeof(std::atomic<const T *>) +
                             sizeof(SlotState)) +
           utils::ObjectSlab<T>::Bytes();
  }

  // The admission each new slab chunk asks first (D-198): Make admits
  // ObjectSlab<T>::chunk_bytes() whenever the slab adds a chunk.
  void set_memory_growth(utils::MemoryGrowth *growth) noexcept {
    objects_->set_memory_growth(growth);
  }

  // Destroys the published objects: the owner guarantees no reader remains
  // (module teardown after workers stopped using it). Then the slab is
  // released (objects_ goes last).
  ~SlotTable() {
    for (size_t i = 0; i <= capacity_; i++) {
      if (const T *object = slots_[i].load(std::memory_order_relaxed)) {
        objects_->Destroy(object);
      }
    }
  }

  SlotTable(const SlotTable &) = delete;
  SlotTable &operator=(const SlotTable &) = delete;

  // A new object for this table, to Publish. Mutable until then (it converts
  // to Owned): an owner may finish it in a later reservation step. Throws
  // MemoryRefused (the growth hook's refusal of a new slab chunk, or
  // DpdkAllocate's) or std::bad_alloc (control path), or what T's
  // constructor throws.
  template <typename... Args>
  utils::SlabUnique<T> Make(Args &&...args) {
    return objects_->Make(std::forward<Args>(args)...);
  }

  // -- reader -----------------------------------------------------------------

  // The object at `id`, or nullptr for an invalid, out-of-range or empty id.
  // Valid until the calling worker's next quiescent state.
  const T *Lookup(Id id) const noexcept {
    const size_t index = static_cast<size_t>(id.value());
    if (index == 0 || index > capacity_) {
      return nullptr;
    }
    return slots_[index].load(std::memory_order_acquire);
  }

  // -- writer -----------------------------------------------------------------

  bool ValidId(Id id) const noexcept {
    const size_t index = static_cast<size_t>(id.value());
    return index != 0 && index <= capacity_;
  }

  SlotState state(Id id) const noexcept {
    return states_[static_cast<size_t>(id.value())];
  }

  bool Contains(Id id) const noexcept {
    return ValidId(id) && state(id) == SlotState::kLive;
  }

  // Whether `id` may be published now: valid and not retiring.
  bool CanPublish(Id id) const noexcept {
    return ValidId(id) && state(id) != SlotState::kRetiring;
  }

  // The object the writer last published at `id` (control side).
  const T *Current(Id id) const noexcept {
    return Contains(id)
               ? slots_[static_cast<size_t>(id.value())].load(
                     std::memory_order_relaxed)
               : nullptr;
  }

  // Makes `object` (from Make) visible at `id` (free or live; CanPublish must
  // hold) and returns the object it replaced, which the caller retires after
  // a grace period. Infallible.
  Owned Publish(Id id, Owned object) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    CHECK(CanPublish(id));  // a retiring object may still be read (D-061)
    CHECK(object == nullptr || object.get_deleter().slab == objects_.get());
    const T *old = slots_[index].exchange(object.release(),
                                          std::memory_order_acq_rel);
    if (states_[index] == SlotState::kFree) {
      size_++;
    }
    states_[index] = SlotState::kLive;
    return Adopt(old);
  }

  // Removes the object at `id` (which must be live) from the control side.
  // It stays readable, and the id unpublishable, until Unpublish().
  void Retire(Id id) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    CHECK(Contains(id));
    states_[index] = SlotState::kRetiring;
    size_--;
  }

  // Empties a retiring slot and returns its object; the caller frees it after
  // a grace period (a reader may have just looked it up). The id is free.
  Owned Unpublish(Id id) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    CHECK(state(id) == SlotState::kRetiring);  // else a live object is freed (D-061)
    states_[index] = SlotState::kFree;
    return Adopt(slots_[index].exchange(nullptr, std::memory_order_acq_rel));
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  // DPDK memory held: the slot array, the states and the object slab.
  size_t bytes() const noexcept {
    return slots_.size() * sizeof(slots_[0]) +
           states_.size() * sizeof(states_[0]) + objects_->bytes();
  }

 private:
  Owned Adopt(const T *object) noexcept {
    return Owned(object, utils::SlabDelete<const T>(objects_.get()));
  }

  // Declared first, released last: the destructor returns objects to it.
  typename utils::ObjectSlab<T>::Owner objects_;
  utils::DpdkArray<std::atomic<const T *>> slots_;  // index = id
  utils::DpdkArray<SlotState> states_;              // control side
  size_t capacity_;
  size_t size_ = 0;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_SLOT_TABLE_H_
