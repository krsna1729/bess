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

#ifndef BESS_STATS_WORKER_HISTOGRAM_H_
#define BESS_STATS_WORKER_HISTOGRAM_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <vector>

#include "stats/worker_slots.h"

namespace bess::stats {

// How values map to buckets. Two shapes, both O(1) on the packet path:
//
//   Log2():                bucket 0 holds 0; bucket i (1..64) holds
//                          [2^(i-1), 2^i). 65 buckets, any uint64_t, 2x
//                          resolution -- the latency/size default.
//   Linear(width, count):  bucket i holds [i*width, (i+1)*width) for
//                          i < count; one more bucket holds everything above.
class HistogramLayout {
 public:
  static HistogramLayout Log2() noexcept { return HistogramLayout(0, 65); }

  // `width` and `count` must be non-zero.
  static HistogramLayout Linear(uint64_t width, size_t count) noexcept {
    return HistogramLayout(width, count + 1);
  }

  size_t buckets() const noexcept { return buckets_; }
  bool is_log2() const noexcept { return width_ == 0; }

  size_t Index(uint64_t value) const noexcept {
    if (width_ == 0) {
      return static_cast<size_t>(std::bit_width(value));
    }
    const uint64_t i = value / width_;
    return i < buckets_ - 1 ? static_cast<size_t>(i) : buckets_ - 1;
  }

  // Smallest value bucket `i` can hold.
  uint64_t LowerBound(size_t i) const noexcept {
    if (width_ == 0) {
      return i == 0 ? 0 : uint64_t{1} << (i - 1);
    }
    return i * width_;
  }

  // Largest value bucket `i` can hold (UINT64_MAX for the open top bucket).
  uint64_t UpperBound(size_t i) const noexcept {
    if (width_ == 0) {
      return i == 0 ? 0 : (i == 64 ? UINT64_MAX : (uint64_t{1} << i) - 1);
    }
    return i == buckets_ - 1 ? UINT64_MAX : (i + 1) * width_ - 1;
  }

  friend bool operator==(const HistogramLayout &,
                         const HistogramLayout &) = default;

 private:
  HistogramLayout(uint64_t width, size_t buckets)
      : width_(width), buckets_(buckets) {}

  uint64_t width_;  // 0 means log2
  size_t buckets_;
};

struct HistogramSnapshot {
  uint64_t source = 0;
  uint64_t generation = 0;
  uint64_t epoch = 0;
  uint64_t tsc = 0;
  uint64_t count = 0;  // == sum of buckets, always
  uint64_t sum = 0;    // sum of recorded values (wraps modulo 2^64)
  std::vector<uint64_t> buckets;
  HistogramLayout layout = HistogramLayout::Log2();

  // An upper bound on the p-th percentile (0 < p <= 100): the upper edge of
  // the first bucket at which the cumulative count reaches p% of `count`.
  // Bucketed data cannot do better than a bucket edge; the upper one never
  // understates a latency. Zero for an empty histogram.
  uint64_t PercentileUpperBound(double p) const noexcept;
};

// Worker-local histogram with controller-side snapshots (K6). Same ownership
// and reset rules as CounterSet; every Record is one grouped update, so a
// snapshot's count always equals the sum of its buckets.
class WorkerHistogram {
 public:
  explicit WorkerHistogram(HistogramLayout layout);

  WorkerHistogram(const WorkerHistogram &) = delete;
  WorkerHistogram &operator=(const WorkerHistogram &) = delete;

  const HistogramLayout &layout() const noexcept { return layout_; }

  // Worker side.
  void Record(WorkerId worker, uint64_t value) noexcept {
    const WorkerSlots::Writer w = slots_.ForWorker(worker);
    w.BeginUpdate();
    w.Add(kCount, 1);
    w.Add(kSum, value);
    w.Add(kFirstBucket + layout_.Index(value), 1);
    w.EndUpdate();
  }

  // Worker side: a batch of values as one update (one sequence bump pair).
  void RecordBatch(WorkerId worker, std::span<const uint64_t> values) noexcept {
    const WorkerSlots::Writer w = slots_.ForWorker(worker);
    w.BeginUpdate();
    uint64_t sum = 0;
    for (uint64_t v : values) {
      sum += v;
      w.Add(kFirstBucket + layout_.Index(v), 1);
    }
    w.Add(kCount, values.size());
    w.Add(kSum, sum);
    w.EndUpdate();
  }

  // Control side.
  HistogramSnapshot Snapshot() const;
  void Reset();

  size_t storage_bytes() const noexcept { return slots_.bytes(); }

 private:
  static constexpr size_t kCount = 0;
  static constexpr size_t kSum = 1;
  static constexpr size_t kFirstBucket = 2;

  std::vector<uint64_t> ReadAll() const;

  const uint64_t id_;
  const HistogramLayout layout_;
  WorkerSlots slots_;

  mutable std::mutex control_mutex_;
  mutable uint64_t generation_ = 0;
  uint64_t epoch_ = 0;
  std::vector<uint64_t> baseline_;
};

// Bucket-wise change between two snapshots of one histogram in one epoch. In
// the result, `tsc` holds the elapsed cycles and the generation/epoch are the
// later snapshot's.
std::expected<HistogramSnapshot, StatsError> Delta(
    const HistogramSnapshot &earlier, const HistogramSnapshot &later);

}  // namespace bess::stats

#endif  // BESS_STATS_WORKER_HISTOGRAM_H_
