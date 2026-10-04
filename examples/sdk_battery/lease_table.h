// SPDX-License-Identifier: BSD-3-Clause

// A third-party battery built only on installed BESS primitives (roadmap M23,
// persona E): leases that expire unless renewed, for ids the library's user
// defines. It uses `dataplane::StrongId` for its own handle space,
// `dataplane::GenerationHandle` so a handle to an expired lease cannot renew
// the lease that reused its slot, and `dataplane::ExpiryWheel` for the
// deadlines. Nothing here includes an internal BESS header.

#ifndef EXAMPLES_SDK_BATTERY_LEASE_TABLE_H_
#define EXAMPLES_SDK_BATTERY_LEASE_TABLE_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

#include "dataplane/expiry_wheel.h"
#include "dataplane/generation_handle.h"
#include "dataplane/strong_id.h"

namespace lease {

struct LeaseSlotTag;
using LeaseSlot = bess::dataplane::StrongId<LeaseSlotTag, uint32_t>;
using LeaseHandle = bess::dataplane::GenerationHandle<LeaseSlot>;

// What leases are granted to: any trivially copyable value the user picks.
template <typename Holder>
class LeaseTable {
 public:
  using Wheel = bess::dataplane::ExpiryWheel<uint32_t>;  // payload: slot index

  static std::unique_ptr<LeaseTable> Create(uint32_t capacity) {
    auto wheel = Wheel::Create(capacity);
    if (!wheel) {
      return nullptr;
    }
    return std::unique_ptr<LeaseTable>(new LeaseTable(capacity, std::move(*wheel)));
  }

  // A lease for `holder` until `now + ttl`; an empty handle when full.
  LeaseHandle Grant(const Holder &holder, uint64_t now, uint64_t ttl) {
    if (free_.empty()) {
      return {};
    }
    const uint32_t slot = free_.back();
    free_.pop_back();
    Entry &e = entries_[slot];
    e.holder = holder;
    e.live = true;
    e.timer = wheel_->Schedule(Wheel::After(now, ttl), slot);
    return {LeaseSlot{slot}, e.generation};
  }

  // Extends a live lease; false for an expired or reused one.
  bool Renew(LeaseHandle handle, uint64_t now, uint64_t ttl) {
    Entry *e = Live(handle);
    return e != nullptr && wheel_->Refresh(e->timer, Wheel::After(now, ttl));
  }

  const Holder *Holding(LeaseHandle handle) const {
    const Entry *e = const_cast<LeaseTable *>(this)->Live(handle);
    return e == nullptr ? nullptr : &e->holder;
  }

  // Ends every lease whose deadline has passed; calls fn(holder) for each.
  template <typename Fn>
  size_t Expire(uint64_t now, Fn &&fn) {
    size_t ended = 0;
    (void)wheel_->Poll(now, ~size_t{0}, [&](uint32_t slot) noexcept {
      Entry &e = entries_[slot];
      fn(e.holder);
      e.live = false;
      e.generation++;  // every outstanding handle to this lease is now stale
      free_.push_back(slot);
      ended++;
    });
    return ended;
  }

 private:
  struct Entry {
    Holder holder{};
    bess::dataplane::ExpiryHandle timer{};
    uint32_t generation = 1;
    bool live = false;
  };

  LeaseTable(uint32_t capacity, std::unique_ptr<Wheel> wheel)
      : entries_(capacity), wheel_(std::move(wheel)) {
    free_.reserve(capacity);
    for (uint32_t i = capacity; i > 0; i--) {
      free_.push_back(i - 1);
    }
  }

  Entry *Live(LeaseHandle handle) {
    const uint32_t slot = handle.id.value();
    if (slot >= entries_.size()) {
      return nullptr;
    }
    Entry &e = entries_[slot];
    return e.live && e.generation == handle.generation ? &e : nullptr;
  }

  std::vector<Entry> entries_;
  std::vector<uint32_t> free_;
  std::unique_ptr<Wheel> wheel_;
};

}  // namespace lease

#endif  // EXAMPLES_SDK_BATTERY_LEASE_TABLE_H_
