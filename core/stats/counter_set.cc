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

#include "stats/counter_set.h"

#include <atomic>
#include <utility>

#include <rte_cycles.h>

namespace bess::stats {

// Distinguishes snapshots of different sets and histograms, including one
// allocated where a destroyed one used to be.
uint64_t NextStatsSourceId() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

const char *StatsErrorName(StatsError error) {
  switch (error) {
    case StatsError::kDifferentSource:
      return "snapshots come from different sets";
    case StatsError::kNotOrdered:
      return "the later snapshot is not newer than the earlier one";
    case StatsError::kEpochChanged:
      return "the set was reset between the snapshots";
  }
  return "unknown stats error";
}

CounterSet::CounterSet(std::vector<std::string> names)
    : id_(NextStatsSourceId()),
      names_(std::move(names)),
      slots_(names_.size()),
      baseline_(dataplane::kMaxWorkers * names_.size(), 0) {}

std::vector<uint64_t> CounterSet::ReadAll() const {
  const size_t n = names_.size();
  std::vector<uint64_t> raw(dataplane::kMaxWorkers * n);
  for (size_t w = 0; w < dataplane::kMaxWorkers; w++) {
    slots_.ReadSlot(WorkerId(static_cast<uint16_t>(w)),
                    std::span(raw).subspan(w * n, n));
  }
  return raw;
}

CounterSnapshot CounterSet::Snapshot(bool per_worker) const {
  std::lock_guard<std::mutex> lock(control_mutex_);
  CounterSnapshot snap;
  snap.source = id_;
  snap.generation = ++generation_;
  snap.epoch = epoch_;
  snap.tsc = rte_get_tsc_cycles();

  std::vector<uint64_t> raw = ReadAll();
  const size_t n = names_.size();
  snap.totals.assign(n, 0);
  for (size_t w = 0; w < dataplane::kMaxWorkers; w++) {
    for (size_t i = 0; i < n; i++) {
      uint64_t &v = raw[w * n + i];
      v -= baseline_[w * n + i];
      snap.totals[i] += v;
    }
  }
  if (per_worker) {
    snap.per_worker = std::move(raw);
  }
  return snap;
}

void CounterSet::Reset() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  baseline_ = ReadAll();
  epoch_++;
}

std::expected<CounterDelta, StatsError> Delta(const CounterSnapshot &earlier,
                                              const CounterSnapshot &later) {
  if (earlier.source != later.source ||
      earlier.totals.size() != later.totals.size()) {
    return std::unexpected(StatsError::kDifferentSource);
  }
  if (later.generation <= earlier.generation) {
    return std::unexpected(StatsError::kNotOrdered);
  }
  if (later.epoch != earlier.epoch) {
    return std::unexpected(StatsError::kEpochChanged);
  }
  CounterDelta delta;
  delta.tsc_elapsed = later.tsc - earlier.tsc;
  delta.deltas.resize(later.totals.size());
  for (size_t i = 0; i < later.totals.size(); i++) {
    delta.deltas[i] = later.totals[i] - earlier.totals[i];
  }
  return delta;
}

}  // namespace bess::stats
