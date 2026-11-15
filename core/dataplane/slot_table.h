// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_SLOT_TABLE_H_
#define BESS_DATAPLANE_SLOT_TABLE_H_

#include <array>
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
// Memory (Decision D-029): the directory and objects come from DPDK's heap.
// By default the directory is dense; kPaged uses stable 1024-ID pages admitted
// by PrepareSlot during reservation. Objects come from the table's slab. An object
// is owned by `Owned`, whose deleter returns it to that slab. Objects the
// table handed out may outlive it -- the retired ones Publish and Unpublish
// return wait in the RCU queue, an unpublished one in a pending transaction
// operation -- as they could when they were plain heap objects: the table
// releases its slab rather than destroying it, and the slab goes with the
// last of them (utils::ObjectSlab::Owner).
template <typename Id, typename T, bool kPaged = false>
class SlotTable {
 public:
  enum class SlotState : uint8_t { kFree, kLive, kRetiring };
  using Owned = utils::SlabUnique<const T>;

  // Ids 1..capacity.
  explicit SlotTable(size_t capacity)
      : objects_(utils::ObjectSlab<T>::Create()),
        slots_(kPaged ? utils::DpdkArray<std::atomic<const T *>>{} :
                        utils::DpdkArray<std::atomic<const T *>>(capacity + 1)),
        states_(kPaged ? utils::DpdkArray<SlotState>{} :
                         utils::DpdkArray<SlotState>(capacity + 1)),
        pages_(kPaged ? utils::DpdkArray<std::atomic<Page *>>(capacity / kPageSize + 1) :
                        utils::DpdkArray<std::atomic<Page *>>{}),
        capacity_(capacity) {}

  // What the constructor allocates in DPDK memory (D-198): the slot array
  // and the states; the slab starts without a chunk. Objects are growth
  // (set_memory_growth).
  static size_t Bytes(size_t capacity) noexcept {
    if constexpr (kPaged)
      return (capacity / kPageSize + 1) * sizeof(std::atomic<Page *>) +
             utils::ObjectSlab<T>::Bytes();
    return (capacity + 1) * (sizeof(std::atomic<const T *>) + sizeof(SlotState)) +
           utils::ObjectSlab<T>::Bytes();
  }

  static constexpr size_t PageBytes() noexcept { return sizeof(Page); }

  // The admission each new slab chunk asks first (D-198): Make admits
  // ObjectSlab<T>::chunk_bytes() whenever the slab adds a chunk.
  void set_memory_growth(utils::MemoryGrowth *growth) noexcept {
    objects_->set_memory_growth(growth);
    growth_ = growth;
  }

  // Destroys the published objects: the owner guarantees no reader remains
  // (module teardown after workers stopped using it). Then the slab is
  // released (objects_ goes last).
  ~SlotTable() {
    if constexpr (kPaged) {
      for (size_t i = 0; i < pages_.size(); ++i) {
        auto *page = pages_[i].load(std::memory_order_relaxed);
        if (page == nullptr) continue;
        for (auto &slot : page->slots)
          if (const T *object = slot.load(std::memory_order_relaxed))
            objects_->Destroy(object);
        utils::DpdkDelete<Page>{}(page);
      }
    } else {
      for (size_t i = 0; i <= capacity_; ++i)
        if (const T *object = slots_[i].load(std::memory_order_relaxed))
          objects_->Destroy(object);
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

  // Control path, before staging an upsert. Successfully allocated pages are
  // retained capacity, like slab chunks, even if a later reservation aborts.
  // Neither packet readers nor Publish allocate; pages and addresses remain
  // stable through removal cascades and are released only at teardown.
  void PrepareSlot(Id id) {
    CHECK(ValidId(id));
    if constexpr (kPaged) {
      auto &entry = pages_[id.value() / kPageSize];
      if (entry.load(std::memory_order_relaxed) != nullptr) return;
      if (growth_ != nullptr) growth_->Grow(sizeof(Page));
      utils::DpdkUnique<Page> page;
      try {
        page = utils::MakeDpdk<Page>(utils::MemoryNode());
      } catch (...) {
        if (growth_ != nullptr) growth_->Shrink(sizeof(Page));
        throw;
      }
      entry.store(page.release(), std::memory_order_release);
      page_bytes_ += sizeof(Page);
    }
  }

  // -- reader -----------------------------------------------------------------

  // The object at `id`, or nullptr for an invalid, out-of-range or empty id.
  // Valid until the calling worker's next quiescent state.
  const T *Lookup(Id id) const noexcept {
    const size_t index = static_cast<size_t>(id.value());
    if (index == 0 || index > capacity_) {
      return nullptr;
    }
    auto *slot = ObjectSlot(index);
    return slot == nullptr ? nullptr : slot->load(std::memory_order_acquire);
  }

  // -- writer -----------------------------------------------------------------

  bool ValidId(Id id) const noexcept {
    const size_t index = static_cast<size_t>(id.value());
    return index != 0 && index <= capacity_;
  }

  SlotState state(Id id) const noexcept {
    if constexpr (kPaged) {
      auto *page = pages_[id.value() / kPageSize].load(std::memory_order_acquire);
      return page == nullptr ? SlotState::kFree : page->states[id.value() % kPageSize];
    } else {
      return states_[static_cast<size_t>(id.value())];
    }
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
    return Contains(id) ? ObjectSlot(id.value())->load(std::memory_order_relaxed) : nullptr;
  }

  // Makes `object` (from Make) visible at `id` (free or live; CanPublish must
  // hold) and returns the object it replaced, which the caller retires after
  // a grace period. Infallible.
  Owned Publish(Id id, Owned object) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    CHECK(CanPublish(id));  // a retiring object may still be read (D-061)
    CHECK(object == nullptr || object.get_deleter().slab == objects_.get());
    auto *slot = ObjectSlot(index);
    CHECK(slot != nullptr);  // PrepareSlot admitted it during reservation.
    const T *old = slot->exchange(object.release(),
                                          std::memory_order_acq_rel);
    if (state(id) == SlotState::kFree) {
      size_++;
    }
    StateSlot(index) = SlotState::kLive;
    return Adopt(old);
  }

  // Removes the object at `id` (which must be live) from the control side.
  // It stays readable, and the id unpublishable, until Unpublish().
  void Retire(Id id) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    CHECK(Contains(id));
    StateSlot(index) = SlotState::kRetiring;
    size_--;
  }

  // Empties a retiring slot and returns its object; the caller frees it after
  // a grace period (a reader may have just looked it up). The id is free.
  Owned Unpublish(Id id) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    CHECK(state(id) == SlotState::kRetiring);  // else a live object is freed (D-061)
    StateSlot(index) = SlotState::kFree;
    return Adopt(ObjectSlot(index)->exchange(nullptr, std::memory_order_acq_rel));
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  // DPDK memory held: the slot array, the states and the object slab.
  size_t bytes() const noexcept {
    return slots_.size() * sizeof(slots_[0]) +
           states_.size() * sizeof(states_[0]) + pages_.bytes() + page_bytes_ + objects_->bytes();
  }

 private:
  static constexpr size_t kPageSize = 1024;
  struct Page {
    std::array<std::atomic<const T *>, kPageSize> slots{};
    std::array<SlotState, kPageSize> states{};
  };
  std::atomic<const T *> *ObjectSlot(size_t index) const noexcept {
    if constexpr (kPaged) {
      auto *page = pages_[index / kPageSize].load(std::memory_order_acquire);
      return page == nullptr ? nullptr : &page->slots[index % kPageSize];
    } else {
      return const_cast<std::atomic<const T *> *>(&slots_[index]);
    }
  }
  SlotState &StateSlot(size_t index) noexcept {
    if constexpr (kPaged)
      return pages_[index / kPageSize].load(std::memory_order_relaxed)->states[index % kPageSize];
    else
      return states_[index];
  }
  Owned Adopt(const T *object) noexcept {
    return Owned(object, utils::SlabDelete<const T>(objects_.get()));
  }

  // Declared first, released last: the destructor returns objects to it.
  typename utils::ObjectSlab<T>::Owner objects_;
  utils::DpdkArray<std::atomic<const T *>> slots_;  // index = id
  utils::DpdkArray<SlotState> states_;              // control side
  utils::DpdkArray<std::atomic<Page *>> pages_;
  utils::MemoryGrowth *growth_ = nullptr;
  size_t page_bytes_ = 0;
  size_t capacity_;
  size_t size_ = 0;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_SLOT_TABLE_H_
