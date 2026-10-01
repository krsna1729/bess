// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_SCOPE_CELL_H_
#define BESS_DATAPLANE_SCOPE_CELL_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "dataplane/strong_id.h"
#include "meter/meter.h"
#include "route/next_hop_id.h"

namespace bess::dataplane {

struct ScopeIdTag;
using ScopeId = StrongId<ScopeIdTag, uint32_t>;
inline constexpr ScopeId kInvalidScopeId{};

// ScopePlan: The atomic continuation state for a session.
// Packs 32-bit MeterId and 32-bit NextHopId into a single 64-bit word so
// that both fields are published and read in a single CPU instruction,
// eliminating torn policy reads (VISIBILITY_ATOMIC, D-021).
struct ScopePlan {
  meter::MeterId meter{};
  route::NextHopId next_hop{};

  static constexpr uint64_t Pack(ScopePlan plan) noexcept {
    return (static_cast<uint64_t>(plan.meter.value()) << 32) |
           static_cast<uint64_t>(plan.next_hop.value());
  }

  static constexpr ScopePlan Unpack(uint64_t word) noexcept {
    return ScopePlan{
        .meter = meter::MeterId(static_cast<uint32_t>(word >> 32)),
        .next_hop = route::NextHopId(static_cast<uint32_t>(word & 0xFFFFFFFFULL)),
    };
  }
};

static_assert(sizeof(ScopePlan) == 8, "ScopePlan must be 64 bits");

// ScopeCell: A single 64-bit atomic word holding a session's active ScopePlan.
class alignas(8) ScopeCell {
 public:
  constexpr ScopeCell() noexcept : word_(0) {}
  explicit constexpr ScopeCell(ScopePlan plan) noexcept
      : word_(ScopePlan::Pack(plan)) {}

  ScopeCell(const ScopeCell &other) noexcept
      : word_(other.word_.load(std::memory_order_relaxed)) {}

  ScopeCell &operator=(const ScopeCell &other) noexcept {
    word_.store(other.word_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
    return *this;
  }

  ScopePlan Load() const noexcept {
    return ScopePlan::Unpack(word_.load(std::memory_order_acquire));
  }

  void Store(ScopePlan plan) noexcept {
    word_.store(ScopePlan::Pack(plan), std::memory_order_release);
  }

 private:
  std::atomic<uint64_t> word_{0};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free on target architecture");

// ScopeTable: Contiguous array of ScopeCells indexed by ScopeId.
class ScopeTable {
 public:
  explicit ScopeTable(size_t capacity)
      : capacity_(capacity), cells_(capacity + 1) {}

  size_t capacity() const noexcept { return capacity_; }

  bool ValidId(ScopeId id) const noexcept {
    return id.value() > 0 && id.value() <= capacity_;
  }

  ScopePlan Read(ScopeId id) const noexcept {
    if (unlikely(!ValidId(id))) {
      return ScopePlan{};
    }
    return cells_[id.value()].Load();
  }

  void Publish(ScopeId id, ScopePlan plan) noexcept {
    if (likely(ValidId(id))) {
      cells_[id.value()].Store(plan);
    }
  }

 private:
  const size_t capacity_;
  std::vector<ScopeCell> cells_;
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_SCOPE_CELL_H_
