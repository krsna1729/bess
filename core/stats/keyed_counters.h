// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_KEYED_COUNTERS_H_
#define BESS_STATS_KEYED_COUNTERS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dataplane/worker_id.h"
#include "stats/worker_local.h"
#include "utils/common.h"

namespace bess::stats {

// Per-worker planes of keyed counters (D-154).
//
// A module's counters are usually per key and per worker: a rule, a session or
// a flow has counters, and each worker keeps its own share of them. The keys
// are dense indices the owner assigns (a rule's slot, a session's slot), and
// the owner decides what each cell means -- the UPF's counter module keeps
// octets and packets per rule, so its keys are ActionId slots and it has two
// cells per key. This structure never hashes, never grows and never allocates
// on the packet path.
//
// Memory is keys x cells_per_key x 8 bytes x workers: the planes are sized to
// the worker count the owner passes, so a module that runs on 8 workers pays
// for 8 planes and not for the 64 worker slots the daemon reserves. That is
// the difference from CounterSet, whose cell count is a small schema rather
// than a key space.
//
// Writers. One plane per worker, and only its owning worker writes it, so an
// update is a relaxed load, an add and a relaxed store: no locked
// instruction, no shared line, no allocation, and `Add` is noexcept. The
// atomics make a controller's concurrent read well defined (and invisible to
// TSan); they are not a synchronization between writers, because there is
// only ever one.
//
// Readers. The control side reads one cell of one plane (Read) or one cell
// summed over the planes (Total). A single cell is always read whole, and the
// cells are additive counters, so the cells of one update need not be seen
// together: a crossing that lands during a read is reported by the next read,
// which is the level-triggered rule ThresholdTable states (D-151). That is
// why there is no sequence word here -- WorkerSlots has one for cells that
// must agree (a batch's packets and bytes), and this structure is not that
// case.
//
// Why not build on WorkerSlots: its storage is one plane per kMaxWorkers slot
// whatever the daemon runs, and its read copies a whole plane into a span. A
// keyed plane is large -- 1M keys x 2 cells x 8 B x 8 workers is 128 MB -- so
// both the fixed 64 slots and a whole-plane copy are the wrong shape. The
// ownership rule and the relaxed update are deliberately the same as that
// class's.
//
// Reuse of a key is the owner's: a key's cells keep their value when the
// owner reuses the slot, so an owner that re-arms a threshold on a reused key
// either records the value it saw and subtracts it, or keeps its keys
// monotonic. Zeroing a plane from the control side would race its writer.
class KeyedCounters {
 public:
  // `keys`, `cells_per_key` and `workers` are all at least 1, and `workers` is
  // at most dataplane::kMaxWorkers: the owner's key space, the cells per key,
  // and the planes. A caller that violates one is a bug rather than a case, and
  // `promise` states it -- a hint to the optimiser, not a runtime check, so a
  // violation is undefined behaviour (this tree's convention for a caller bug,
  // `utils/common.h`). The zero key space is the one to watch: it would leave
  // every plane empty and the first `ForWorker` would hand out a pointer into
  // nothing, which is why the module that owns the capacity refuses a zero one
  // in its own Init rather than passing it here.
  KeyedCounters(size_t keys, size_t cells_per_key, size_t workers)
      : keys_(keys),
        cells_per_key_(cells_per_key),
        workers_(workers),
        words_per_plane_(RoundUpToLine(keys_ * cells_per_key_)),
        planes_((words_per_plane_ / kWordsPerLine) * workers_) {
    promise(keys >= 1);
    promise(cells_per_key >= 1);
    promise(workers >= 1);
    promise(workers <= dataplane::kMaxWorkers);
  }

  KeyedCounters(const KeyedCounters &) = delete;
  KeyedCounters &operator=(const KeyedCounters &) = delete;

  // One worker's plane. Cheap to copy, valid as long as the KeyedCounters is,
  // and must only be used by that worker.
  class Plane {
   public:
    // Adds `n` to cell `cell` of key `key`.
    void Add(size_t key, size_t cell, uint64_t n) const noexcept {
      promise(key < keys_);
      std::atomic<uint64_t> &c = plane_[cell + key * cells_per_key_];
      c.store(c.load(std::memory_order_relaxed) + n,
              std::memory_order_relaxed);
    }

   private:
    friend class KeyedCounters;
    Plane(std::atomic<uint64_t> *plane, size_t keys, size_t cells_per_key)
        : plane_(plane), keys_(keys), cells_per_key_(cells_per_key) {}

    std::atomic<uint64_t> *plane_;
    size_t keys_;
    size_t cells_per_key_;
  };

  Plane ForWorker(WorkerId worker) noexcept {
    promise(worker.value() < workers_);
    return Plane(&planes_[worker.value() * words_per_plane_ / kWordsPerLine]
                         .words[0],
                 keys_, cells_per_key_);
  }

  // Control side. Both are safe to call while the workers are adding, and
  // both read one cell per plane: `Read` is one plane's cell, `Total` the sum
  // over every plane. A cell never written reads 0.
  uint64_t Read(WorkerId worker, size_t key, size_t cell) const noexcept {
    promise(worker.value() < workers_);
    return Word(worker, key, cell).load(std::memory_order_relaxed);
  }

  uint64_t Total(size_t key, size_t cell) const noexcept {
    uint64_t sum = 0;
    for (size_t w = 0; w < workers_; w++) {
      sum += Word(WorkerId(static_cast<uint16_t>(w)), key, cell)
                 .load(std::memory_order_relaxed);
    }
    return sum;
  }

  size_t keys() const noexcept { return keys_; }
  size_t cells_per_key() const noexcept { return cells_per_key_; }
  size_t workers() const noexcept { return workers_; }

  // The whole allocation: workers x planes, each padded to whole cache lines.
  size_t bytes() const noexcept { return planes_.size() * sizeof(Line); }

  // The byte offset of one cell inside the allocation, relative to its start.
  // Each worker's plane begins on a cache line of its own, so no two workers
  // share one; the footprint report and the layout tests read this.
  size_t CellOffset(WorkerId worker, size_t key, size_t cell) const noexcept {
    promise(worker.value() < workers_);
    promise(key < keys_);
    promise(cell < cells_per_key_);
    return ((worker.value() * words_per_plane_) + cell + key * cells_per_key_) *
           sizeof(uint64_t);
  }

 private:
  static constexpr size_t kWordsPerLine = kCacheLine / sizeof(uint64_t);

  // One cache line of cells. The storage is a flat run of these and each
  // worker's plane is padded to whole lines, so two planes never share one.
  struct alignas(kCacheLine) Line {
    std::atomic<uint64_t> words[kWordsPerLine] = {};
  };
  static_assert(sizeof(Line) == kCacheLine);

  static constexpr size_t RoundUpToLine(size_t words) noexcept {
    return ((words + kWordsPerLine - 1) / kWordsPerLine) * kWordsPerLine;
  }

  const std::atomic<uint64_t> &Word(WorkerId worker, size_t key,
                                    size_t cell) const noexcept {
    promise(cell < cells_per_key_);
    promise(key < keys_);
    const size_t word =
        worker.value() * words_per_plane_ + cell + key * cells_per_key_;
    return planes_[word / kWordsPerLine].words[word % kWordsPerLine];
  }

  const size_t keys_;
  const size_t cells_per_key_;
  const size_t workers_;
  const size_t words_per_plane_;  // keys * cells_per_key, rounded to lines
  std::vector<Line> planes_;      // workers planes, PlaneLines() lines each
};

}  // namespace bess::stats

#endif  // BESS_STATS_KEYED_COUNTERS_H_
