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

#ifndef BESS_STATS_COUNTER_SET_H_
#define BESS_STATS_COUNTER_SET_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <vector>

#include "stats/worker_slots.h"

namespace bess::stats {

// A controller-side, immutable reading of a CounterSet.
//
// Consistency, precisely: each worker's contribution is an instant image of
// that worker's cells (a whole Update or none of it); different workers are
// read one after another, so the snapshot is not one global instant. Totals
// are therefore exact sums of per-worker instants -- "consistent enough" for
// rates and accounting, and never torn within a worker.
struct CounterSnapshot {
  uint64_t source = 0;      // identity of the CounterSet it came from
  uint64_t generation = 0;  // strictly increasing per set, starts at 1
  uint64_t epoch = 0;       // increments on every Reset()
  uint64_t tsc = 0;         // TSC when the read started
  std::vector<uint64_t> totals;  // one per counter, summed over workers

  // workers x counters, row-major, when requested; empty otherwise.
  std::vector<uint64_t> per_worker;

  uint64_t PerWorker(WorkerId worker, size_t counter) const {
    return per_worker[worker.value() * totals.size() + counter];
  }
};

// The change between two snapshots of the same set within one epoch.
struct CounterDelta {
  uint64_t tsc_elapsed = 0;
  std::vector<uint64_t> deltas;  // one per counter
};

// Monotonic worker-local counters with controller-side snapshots (K6).
//
//   worker:      set.ForWorker(wid).Add(kPackets, n)       -- no lock, no RMW
//                { auto u = set.Update(wid); u.Add(kPackets, n);
//                  u.Add(kBytes, b); }                     -- grouped
//   controller:  set.Snapshot(), Delta(earlier, later), Reset()
//
// Counters are indexed by position; names are the schema a future exporter
// (Phase F) reads. What a counter *means* belongs to its owner.
//
// Reset never writes a worker's cells -- a controller store would race with
// the worker's load-add-store and could be lost or undo increments. Instead
// the set records the current per-worker values as a baseline and bumps the
// epoch; snapshots subtract the baseline. Worker cells are monotonic for the
// life of the set (64-bit wrap is centuries away at line rate, and unsigned
// subtraction would handle it anyway).
class CounterSet {
 public:
  explicit CounterSet(std::vector<std::string> names);

  CounterSet(const CounterSet &) = delete;
  CounterSet &operator=(const CounterSet &) = delete;

  size_t size() const noexcept { return names_.size(); }
  const std::vector<std::string> &names() const noexcept { return names_; }

  // Worker-side handle for ungrouped adds.
  WorkerSlots::Writer ForWorker(WorkerId worker) noexcept {
    return slots_.ForWorker(worker);
  }

  // Worker-side guard: the adds made through it are seen by a snapshot all
  // together or not at all.
  class Update {
   public:
    explicit Update(WorkerSlots::Writer writer) noexcept : writer_(writer) {
      writer_.BeginUpdate();
    }
    ~Update() { writer_.EndUpdate(); }
    Update(const Update &) = delete;
    Update &operator=(const Update &) = delete;

    void Add(size_t counter, uint64_t n) const noexcept {
      writer_.Add(counter, n);
    }

   private:
    WorkerSlots::Writer writer_;
  };

  Update Updating(WorkerId worker) noexcept {
    return Update(slots_.ForWorker(worker));
  }

  // Control side. Safe to call concurrently with workers and with each other.
  CounterSnapshot Snapshot(bool per_worker = false) const;
  void Reset();

  size_t storage_bytes() const noexcept { return slots_.bytes(); }

 private:
  // Raw per-worker cells, workers x counters.
  std::vector<uint64_t> ReadAll() const;

  const uint64_t id_;
  const std::vector<std::string> names_;
  WorkerSlots slots_;

  mutable std::mutex control_mutex_;
  mutable uint64_t generation_ = 0;
  uint64_t epoch_ = 0;
  std::vector<uint64_t> baseline_;  // workers x counters
};

std::expected<CounterDelta, StatsError> Delta(const CounterSnapshot &earlier,
                                              const CounterSnapshot &later);

}  // namespace bess::stats

#endif  // BESS_STATS_COUNTER_SET_H_
