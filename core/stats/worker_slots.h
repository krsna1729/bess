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

#ifndef BESS_STATS_WORKER_SLOTS_H_
#define BESS_STATS_WORKER_SLOTS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <rte_pause.h>

#include "dataplane/worker_id.h"
#include "stats/worker_local.h"
#include "utils/common.h"

namespace bess::stats {

// The storage under CounterSet and WorkerHistogram (K6): for every worker, a
// run of cache lines holding one sequence word and `cells` 64-bit cells.
//
// Writers. Only the owning worker writes its slot, so a cell update is a
// relaxed load, an add and a relaxed store -- no locked instruction, no
// shared line. The atomics exist so that the controller's concurrent read is
// well-defined (and invisible to TSan), not for synchronization between
// writers; there is only ever one.
//
// Readers. The controller reads another worker's slot while that worker is
// updating it. A single cell is always read whole. A *group* of cells that
// must agree (packets and bytes of one batch; a histogram's count, sum and
// bucket) is written inside an Update, which bumps the slot's sequence word
// to odd before and back to even after -- a seqlock with one writer. ReadSlot
// retries until it sees the same even sequence before and after, so it never
// returns half an update. Writes outside an Update are still individually
// atomic; they just carry no grouping guarantee.
//
// Worker cost of an Update is two extra stores to a line the worker already
// owns; on x86 the fences compile to nothing.
class WorkerSlots {
  static constexpr size_t kWordsPerLine = kCacheLine / sizeof(uint64_t);

  struct alignas(kCacheLine) Line {
    std::atomic<uint64_t> words[kWordsPerLine] = {};
  };
  static_assert(sizeof(Line) == kCacheLine);

 public:
  explicit WorkerSlots(size_t cells)
      : cells_(cells),
        lines_per_slot_((cells + 1 + kWordsPerLine - 1) / kWordsPerLine),
        lines_(lines_per_slot_ * dataplane::kMaxWorkers) {}

  WorkerSlots(const WorkerSlots &) = delete;
  WorkerSlots &operator=(const WorkerSlots &) = delete;

  size_t cells() const noexcept { return cells_; }
  size_t bytes() const noexcept { return lines_.size() * sizeof(Line); }

  // The writing side of one worker's slot. Cheap to copy; valid as long as
  // the WorkerSlots is. Must only be used by that worker.
  class Writer {
   public:
    // Adds `n` to cell `cell`.
    void Add(size_t cell, uint64_t n) const noexcept {
      std::atomic<uint64_t> &c = Word(cell + 1);
      c.store(c.load(std::memory_order_relaxed) + n,
              std::memory_order_relaxed);
    }

    // Opens a group of writes a reader will see all-or-nothing. Prefer the
    // Update guard in CounterSet/WorkerHistogram to calling these directly.
    void BeginUpdate() const noexcept {
      std::atomic<uint64_t> &seq = Word(0);
      seq.store(seq.load(std::memory_order_relaxed) + 1,
                std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
    }
    void EndUpdate() const noexcept {
      std::atomic<uint64_t> &seq = Word(0);
      seq.store(seq.load(std::memory_order_relaxed) + 1,
                std::memory_order_release);
    }

   private:
    friend class WorkerSlots;
    explicit Writer(Line *slot) : slot_(slot) {}

    std::atomic<uint64_t> &Word(size_t word) const noexcept {
      return slot_[word / kWordsPerLine].words[word % kWordsPerLine];
    }

    Line *slot_;
  };

  Writer ForWorker(WorkerId worker) noexcept {
    promise(worker.value() < dataplane::kMaxWorkers);
    return Writer(&lines_[worker.value() * lines_per_slot_]);
  }

  // Copies a consistent image of one worker's cells into `out` (size
  // cells()). Control side only.
  void ReadSlot(WorkerId worker, std::span<uint64_t> out) const noexcept {
    promise(out.size() == cells_);
    const Line *slot = &lines_[worker.value() * lines_per_slot_];
    auto word = [slot](size_t w) -> const std::atomic<uint64_t> & {
      return slot[w / kWordsPerLine].words[w % kWordsPerLine];
    };
    for (;;) {
      const uint64_t before = word(0).load(std::memory_order_acquire);
      if (before & 1) {
        rte_pause();  // the worker is inside an update
        continue;
      }
      for (size_t i = 0; i < cells_; i++) {
        out[i] = word(i + 1).load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      if (word(0).load(std::memory_order_relaxed) == before) {
        return;
      }
    }
  }

 private:
  const size_t cells_;
  const size_t lines_per_slot_;
  std::vector<Line> lines_;
};

// Why a delta between two snapshots cannot be computed.
enum class StatsError : uint8_t {
  kDifferentSource,  // snapshots of two different sets
  kNotOrdered,       // `later` is not newer than `earlier`
  kEpochChanged,     // a Reset happened in between
};

const char *StatsErrorName(StatsError error);

// A process-unique identity for a snapshot source, so snapshots of different
// sets (or of a set allocated where a destroyed one was) never compare.
uint64_t NextStatsSourceId();

}  // namespace bess::stats

#endif  // BESS_STATS_WORKER_SLOTS_H_
