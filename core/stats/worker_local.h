// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_WORKER_LOCAL_H_
#define BESS_STATS_WORKER_LOCAL_H_

#include <array>
#include <cstddef>
#include <memory>

#include "arch/cpu.h"
#include "dataplane/worker_id.h"
#include "utils/common.h"

namespace bess::stats {

using dataplane::WorkerId;

inline constexpr size_t kCacheLine = arch::kCacheLineSize;

// One `T` per worker, each on its own cache lines (K6).
//
// The general rule for mutable high-frequency dataplane state: a worker
// writes only its own slot, so no two workers ever share a line and nothing
// on the packet path is locked or atomic-RMW'd. Whoever reads across slots
// (the controller) needs `T` itself to make that read safe -- CounterSet and
// WorkerHistogram are the K6 types that do; a plain WorkerLocal<T> is for
// scratch that only its own worker ever touches.
//
// Slots are indexed by BESS WorkerId, never by CPU or lcore id.
template <typename T>
class WorkerLocal {
 public:
  WorkerLocal() : slots_(std::make_unique<Slots>()) {}

  WorkerLocal(const WorkerLocal &) = delete;
  WorkerLocal &operator=(const WorkerLocal &) = delete;

  T &operator[](WorkerId worker) noexcept {
    promise(worker.value() < dataplane::kMaxWorkers);
    return (*slots_)[worker.value()].value;
  }
  const T &operator[](WorkerId worker) const noexcept {
    promise(worker.value() < dataplane::kMaxWorkers);
    return (*slots_)[worker.value()].value;
  }

  static constexpr size_t size() noexcept { return dataplane::kMaxWorkers; }

 private:
  struct alignas(kCacheLine) Slot {
    T value{};
  };
  using Slots = std::array<Slot, dataplane::kMaxWorkers>;

  // Heap-allocated so a WorkerLocal member does not put kMaxWorkers cache
  // lines inside its owner (and over-aligned new keeps the alignment).
  std::unique_ptr<Slots> slots_;
};

}  // namespace bess::stats

#endif  // BESS_STATS_WORKER_LOCAL_H_
