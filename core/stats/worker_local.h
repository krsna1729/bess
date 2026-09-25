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

#ifndef BESS_STATS_WORKER_LOCAL_H_
#define BESS_STATS_WORKER_LOCAL_H_

#include <array>
#include <cstddef>
#include <memory>

#include "dataplane/worker_id.h"
#include "utils/common.h"

namespace bess::stats {

using dataplane::WorkerId;

inline constexpr size_t kCacheLine = 64;

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
