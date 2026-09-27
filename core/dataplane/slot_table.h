// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_DATAPLANE_SLOT_TABLE_H_
#define BESS_DATAPLANE_SLOT_TABLE_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include <glog/logging.h>

#include "utils/common.h"

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
template <typename Id, typename T>
class SlotTable {
 public:
  enum class SlotState : uint8_t { kFree, kLive, kRetiring };

  // Ids 1..capacity.
  explicit SlotTable(size_t capacity)
      : slots_(new std::atomic<const T *>[capacity + 1]),
        states_(capacity + 1, SlotState::kFree),
        capacity_(capacity) {
    for (size_t i = 0; i <= capacity; i++) {
      slots_[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  // Destroys the published objects: the owner guarantees no reader remains
  // (module teardown after workers stopped using it).
  ~SlotTable() {
    for (size_t i = 0; i <= capacity_; i++) {
      delete slots_[i].load(std::memory_order_relaxed);
    }
  }

  SlotTable(const SlotTable &) = delete;
  SlotTable &operator=(const SlotTable &) = delete;

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

  // Makes `object` visible at `id` (free or live; CanPublish must hold) and
  // returns the object it replaced, which the caller retires after a grace
  // period. Infallible.
  std::unique_ptr<const T> Publish(Id id,
                                   std::unique_ptr<const T> object) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    DCHECK(CanPublish(id));
    const T *old = slots_[index].exchange(object.release(),
                                          std::memory_order_acq_rel);
    if (states_[index] == SlotState::kFree) {
      size_++;
    }
    states_[index] = SlotState::kLive;
    return std::unique_ptr<const T>(old);
  }

  // Removes the object at `id` (which must be live) from the control side.
  // It stays readable, and the id unpublishable, until Unpublish().
  void Retire(Id id) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    DCHECK(Contains(id));
    states_[index] = SlotState::kRetiring;
    size_--;
  }

  // Empties a retiring slot and returns its object; the caller frees it after
  // a grace period (a reader may have just looked it up). The id is free.
  std::unique_ptr<const T> Unpublish(Id id) noexcept {
    const size_t index = static_cast<size_t>(id.value());
    DCHECK(state(id) == SlotState::kRetiring);
    states_[index] = SlotState::kFree;
    return std::unique_ptr<const T>(
        slots_[index].exchange(nullptr, std::memory_order_acq_rel));
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }

 private:
  std::unique_ptr<std::atomic<const T *>[]> slots_;  // index = id
  std::vector<SlotState> states_;                    // control side
  size_t capacity_;
  size_t size_ = 0;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_SLOT_TABLE_H_
