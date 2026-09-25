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

#include "stats/worker_histogram.h"

#include <atomic>

#include <rte_cycles.h>

namespace bess::stats {

WorkerHistogram::WorkerHistogram(HistogramLayout layout)
    : id_(NextStatsSourceId()),
      layout_(layout),
      slots_(kFirstBucket + layout.buckets()),
      baseline_(dataplane::kMaxWorkers * slots_.cells(), 0) {}

std::vector<uint64_t> WorkerHistogram::ReadAll() const {
  const size_t n = slots_.cells();
  std::vector<uint64_t> raw(dataplane::kMaxWorkers * n);
  for (size_t w = 0; w < dataplane::kMaxWorkers; w++) {
    slots_.ReadSlot(WorkerId(static_cast<uint16_t>(w)),
                    std::span(raw).subspan(w * n, n));
  }
  return raw;
}

HistogramSnapshot WorkerHistogram::Snapshot() const {
  std::lock_guard<std::mutex> lock(control_mutex_);
  HistogramSnapshot snap;
  snap.source = id_;
  snap.generation = ++generation_;
  snap.epoch = epoch_;
  snap.tsc = rte_get_tsc_cycles();
  snap.layout = layout_;
  snap.buckets.assign(layout_.buckets(), 0);

  const std::vector<uint64_t> raw = ReadAll();
  const size_t n = slots_.cells();
  for (size_t w = 0; w < dataplane::kMaxWorkers; w++) {
    const uint64_t *cells = &raw[w * n];
    const uint64_t *base = &baseline_[w * n];
    snap.count += cells[kCount] - base[kCount];
    snap.sum += cells[kSum] - base[kSum];
    for (size_t b = 0; b < layout_.buckets(); b++) {
      snap.buckets[b] += cells[kFirstBucket + b] - base[kFirstBucket + b];
    }
  }
  return snap;
}

void WorkerHistogram::Reset() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  baseline_ = ReadAll();
  epoch_++;
}

uint64_t HistogramSnapshot::PercentileUpperBound(double p) const noexcept {
  if (count == 0) {
    return 0;
  }
  // Smallest rank r with r >= p% of count, at least 1.
  const double target = p / 100.0 * static_cast<double>(count);
  uint64_t rank = static_cast<uint64_t>(target);
  if (static_cast<double>(rank) < target) {
    rank++;
  }
  rank = rank == 0 ? 1 : rank;

  uint64_t cumulative = 0;
  for (size_t i = 0; i < buckets.size(); i++) {
    cumulative += buckets[i];
    if (cumulative >= rank) {
      return layout.UpperBound(i);
    }
  }
  return layout.UpperBound(buckets.size() - 1);
}

std::expected<HistogramSnapshot, StatsError> Delta(
    const HistogramSnapshot &earlier, const HistogramSnapshot &later) {
  if (earlier.source != later.source || !(earlier.layout == later.layout)) {
    return std::unexpected(StatsError::kDifferentSource);
  }
  if (later.generation <= earlier.generation) {
    return std::unexpected(StatsError::kNotOrdered);
  }
  if (later.epoch != earlier.epoch) {
    return std::unexpected(StatsError::kEpochChanged);
  }
  HistogramSnapshot delta = later;
  delta.tsc = later.tsc - earlier.tsc;
  delta.count = later.count - earlier.count;
  delta.sum = later.sum - earlier.sum;
  for (size_t i = 0; i < delta.buckets.size(); i++) {
    delta.buckets[i] = later.buckets[i] - earlier.buckets[i];
  }
  return delta;
}

}  // namespace bess::stats
