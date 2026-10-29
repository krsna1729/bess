// SPDX-License-Identifier: BSD-3-Clause
// Armed limits over a module's own counters (D-151). Experimental API.
//
// A controller that wants to know when a counter reaches a limit should not
// have to read it every second. A module that keeps per-key counters (a usage
// count per rule, say) holds a ThresholdTable beside them: the controller arms
// (key, limit) through the module's own action, and the module's tick
// (framework/module_tick.h) sweeps the table and reports each key whose total
// reached its limit, as one event on the log WatchEvents streams.
//
//   // the module's arm action, under the control-plane lock:
//   thresholds_.Arm(key, limit);
//   // its tick, under the control-plane lock, within the tick's time slice:
//   auto r = thresholds_.Sweep(
//       [this](uint64_t key) -> std::optional<uint64_t> {
//         const Rule *rule = Find(key);
//         return rule == nullptr ? std::nullopt : std::optional(SumPlanes(*rule));
//       },
//       [&ctx] { return ctx.Expired(); });
//   framework::ReportCrossings(ctx, name(), r);
//
// The table is control-side only and not thread-safe: Arm, Disarm and Sweep
// all run under the control-plane lock. It costs the packet path nothing: no
// code on it and no memory beside the counters the module already keeps, and
// the read the module supplies covers every worker's plane, so a key whose
// traffic is spread across workers is judged on its total (a worker-side
// check sees only its own share).
//
// Semantics. Level-triggered: an entry fires when its total is at or past its
// limit at a sweep, so arming a counter already past its limit fires at the
// next sweep and no crossing slips between arming and counting. An entry fires
// once and is removed: one report per arming, and the controller re-arms with
// its next limit. A key the read no longer knows (its rule was deleted) is
// dropped and reported as gone, so a deleted rule leaves no entry behind. Arm
// on an armed key replaces its limit.
//
// Bounded. A sweep makes at most as many reads as the table held when it
// began, stops when `stop()` says so (checked every kStopCheckEvery entries,
// and not before the first batch), and resumes next sweep where it left off,
// so a pass over N entries takes as many ticks as the slice allows, however
// large N is. A removal leaves the cursor on the slot the last entry moved
// into, which is read next; when the sweep had already passed that entry it is
// read twice in the sweep and the entry it displaced waits for the next one.
// No entry is lost: the pass still reaches every entry.
//
// The read runs on the maintenance thread with the control-plane lock held: it
// must be bounded, must not allocate or block (a lookup and a sum over the
// module's planes), and must not modify this table (Arm, Disarm and Clear are
// the module's actions' and tick's, not the read's).
//
// Memory (D-198). An empty table allocates nothing (Bytes() is 0); every
// armed entry is glibc growth of kWatchBytes (its watch and its index node),
// admitted through the owner's growth hook (set_memory_growth) before Arm
// adds it -- MemoryRefused then refuses the arm and changes nothing -- and
// given back when the entry goes (Disarm, a sweep's removal, Clear). The
// admitted bytes are the per-entry estimate; bytes() reports the vector's
// capacity, which does not shrink.

#ifndef BESS_FRAMEWORK_THRESHOLD_TABLE_H_
#define BESS_FRAMEWORK_THRESHOLD_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "framework/module_tick.h"
#include "stats/crossing_notifier.h"
#include "utils/dpdk_memory.h"

namespace bess::framework {

class ThresholdTable {
 public:
  // Entries a sweep evaluates between two looks at its stop condition.
  static constexpr size_t kStopCheckEvery = 16;

  struct Crossing {
    uint64_t key = 0;
    uint64_t value = 0;  // the total the read returned
    uint64_t limit = 0;
  };
  struct Result {
    std::vector<Crossing> crossed;  // fired and removed
    std::vector<uint64_t> gone;     // keys the read no longer knows, removed
    size_t evaluated = 0;           // reads made
  };

  // What an empty table allocates (D-198): nothing; entries are growth.
  static constexpr size_t Bytes() noexcept { return 0; }

  // The admission each newly armed entry asks first (D-198); nullptr:
  // unadmitted.
  void set_memory_growth(utils::MemoryGrowth *growth) noexcept {
    growth_ = growth;
  }

  // Arms `key` at `limit`, replacing an earlier limit for the key. A new key
  // admits kWatchBytes first through the growth hook (its MemoryRefused
  // propagates and nothing changes).
  void Arm(uint64_t key, uint64_t limit) {
    auto it = index_.find(key);
    if (it != index_.end()) {
      watches_[it->second].limit = limit;
      return;
    }
    if (growth_ != nullptr) {
      growth_->Grow(kWatchBytes, kWatchBytes);
    }
    bool pushed = false;
    try {
      watches_.push_back({key, limit});
      pushed = true;
      index_.emplace(key, watches_.size() - 1);
    } catch (...) {
      if (pushed) {
        watches_.pop_back();
      }
      if (growth_ != nullptr) {
        growth_->Shrink(kWatchBytes, kWatchBytes);
      }
      throw;
    }
  }

  // Removes an armed entry; false if there was none.
  bool Disarm(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
      return false;
    }
    EraseAt(it->second);
    return true;
  }

  // The limit `key` is armed at, if it is.
  std::optional<uint64_t> limit(uint64_t key) const {
    auto it = index_.find(key);
    if (it == index_.end()) {
      return std::nullopt;
    }
    return watches_[it->second].limit;
  }

  size_t size() const noexcept { return watches_.size(); }
  void Clear() noexcept {
    if (growth_ != nullptr) {
      growth_->Shrink(watches_.size() * kWatchBytes,
                      watches_.size() * kWatchBytes);
    }
    watches_.clear();
    index_.clear();
    cursor_ = 0;
  }

  // glibc held now (D-198): the watch vector's capacity plus an estimate of
  // the index, one node (link + key/position pair) and one bucket pointer
  // per entry (libstdc++ keeps at least as many buckets as entries).
  size_t bytes() const noexcept {
    return watches_.capacity() * sizeof(Watch) +
           index_.size() * kIndexEntryBytes;
  }

  // Evaluates armed entries round-robin from where the last sweep stopped,
  // at most as many reads as the table held at the start, until they are done
  // or `stop()` is true. `read` maps a key to its current total, or nullopt if
  // the key is gone. At least one batch (kStopCheckEvery entries, or the whole
  // table if smaller) is evaluated per sweep, so a sweep always makes progress.
  template <typename Read, typename Stop>
  Result Sweep(Read &&read, Stop &&stop) {
    Result result;
    const size_t passes = watches_.size();
    while (result.evaluated < passes && !watches_.empty()) {
      if (result.evaluated != 0 && result.evaluated % kStopCheckEvery == 0 && stop()) {
        break;
      }
      if (cursor_ >= watches_.size()) {
        cursor_ = 0;
      }
      const Watch watch = watches_[cursor_];
      result.evaluated++;
      const std::optional<uint64_t> value = read(watch.key);
      if (!value) {
        result.gone.push_back(watch.key);
        EraseAt(cursor_);  // the last entry now sits at cursor_: evaluate it next
      } else if (*value >= watch.limit) {
        result.crossed.push_back({watch.key, *value, watch.limit});
        EraseAt(cursor_);
      } else {
        cursor_++;
      }
    }
    return result;
  }

 private:
  struct Watch {
    uint64_t key;
    uint64_t limit;
  };

  // An index entry, estimated: the node (next link and the key/position
  // pair) and a bucket pointer.
  static constexpr size_t kIndexEntryBytes =
      2 * sizeof(void *) + sizeof(std::pair<const uint64_t, size_t>);

 public:
  // What one armed entry admits (D-198): its watch and its index entry.
  static constexpr size_t kWatchBytes = sizeof(Watch) + kIndexEntryBytes;

 private:
  // Swap with the last entry and pop: O(1). The entry that moves into `pos` is
  // read next, since the cursor stays on `pos`. Gives the entry's bytes back.
  void EraseAt(size_t pos) {
    index_.erase(watches_[pos].key);
    if (pos != watches_.size() - 1) {
      watches_[pos] = watches_.back();
      index_[watches_[pos].key] = pos;
    }
    watches_.pop_back();
    if (growth_ != nullptr) {
      growth_->Shrink(kWatchBytes, kWatchBytes);
    }
  }

  std::vector<Watch> watches_;
  std::unordered_map<uint64_t, size_t> index_;  // key -> position in watches_
  size_t cursor_ = 0;
  utils::MemoryGrowth *growth_ = nullptr;  // D-198: admits each armed entry
};

// Queues one bess.counter_threshold event per crossing on the tick's context,
// source `source` (the module's name), for the loop to append to the log after
// the control-plane lock is released.
inline void ReportCrossings(TickContext &ctx, const std::string &source,
                            const ThresholdTable::Result &result) {
  for (const ThresholdTable::Crossing &c : result.crossed) {
    ctx.Emit(stats::kCounterThresholdEvent, source,
             {{"key", std::to_string(c.key)},
              {"value", std::to_string(c.value)},
              {"limit", std::to_string(c.limit)}});
  }
}

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_THRESHOLD_TABLE_H_
