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

// K6: worker-local counters and histograms. None of this needs the EAL, so
// these tests also run in the ASan/UBSan lane (and are the ones to run under
// TSan: the concurrent tests exercise every cross-thread access the design
// has).

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "stats/counter_set.h"
#include "stats/worker_histogram.h"
#include "stats/worker_local.h"

namespace bess::stats {
namespace {

WorkerId W(uint16_t id) { return WorkerId(id); }

// -- WorkerLocal ---------------------------------------------------------------

TEST(WorkerLocalTest, SlotsAreDistinctCacheLines) {
  WorkerLocal<uint64_t> local;
  for (uint16_t w = 0; w < WorkerLocal<uint64_t>::size(); w++) {
    local[W(w)] = w * 10;
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(&local[W(w)]) % kCacheLine);
  }
  for (uint16_t w = 1; w < WorkerLocal<uint64_t>::size(); w++) {
    EXPECT_EQ(w * 10u, local[W(w)]);
    EXPECT_EQ(kCacheLine, reinterpret_cast<uintptr_t>(&local[W(w)]) -
                              reinterpret_cast<uintptr_t>(&local[W(w - 1)]));
  }
}

// -- CounterSet ----------------------------------------------------------------

enum : size_t { kPackets, kBytes };

TEST(CounterSetTest, SumsWorkersAndBreaksDown) {
  CounterSet set({"packets", "bytes"});
  EXPECT_EQ(2u, set.size());
  set.ForWorker(W(0)).Add(kPackets, 3);
  set.ForWorker(W(0)).Add(kBytes, 300);
  set.ForWorker(W(5)).Add(kPackets, 4);
  {
    auto u = set.Updating(W(63));
    u.Add(kPackets, 1);
    u.Add(kBytes, 64);
  }

  const CounterSnapshot snap = set.Snapshot(/*per_worker=*/true);
  EXPECT_EQ(8u, snap.totals[kPackets]);
  EXPECT_EQ(364u, snap.totals[kBytes]);
  EXPECT_EQ(3u, snap.PerWorker(W(0), kPackets));
  EXPECT_EQ(4u, snap.PerWorker(W(5), kPackets));
  EXPECT_EQ(0u, snap.PerWorker(W(5), kBytes));
  EXPECT_EQ(64u, snap.PerWorker(W(63), kBytes));
  EXPECT_TRUE(set.Snapshot().per_worker.empty());
}

TEST(CounterSetTest, GenerationsAndSources) {
  CounterSet a({"x"});
  CounterSet b({"x"});
  const CounterSnapshot a1 = a.Snapshot();
  const CounterSnapshot a2 = a.Snapshot();
  EXPECT_EQ(1u, a1.generation);
  EXPECT_EQ(2u, a2.generation);
  EXPECT_NE(a1.source, b.Snapshot().source);
  EXPECT_GE(a2.tsc, a1.tsc);
}

TEST(CounterSetTest, ResetIsABaselineNotAWrite) {
  CounterSet set({"packets", "bytes"});
  set.ForWorker(W(1)).Add(kPackets, 10);
  set.ForWorker(W(2)).Add(kPackets, 7);
  const uint64_t epoch = set.Snapshot().epoch;

  set.Reset();
  CounterSnapshot snap = set.Snapshot(true);
  EXPECT_EQ(epoch + 1, snap.epoch);
  EXPECT_EQ(0u, snap.totals[kPackets]);
  EXPECT_EQ(0u, snap.PerWorker(W(1), kPackets));

  set.ForWorker(W(1)).Add(kPackets, 5);
  snap = set.Snapshot(true);
  EXPECT_EQ(5u, snap.totals[kPackets]);
  EXPECT_EQ(5u, snap.PerWorker(W(1), kPackets));
}

TEST(CounterSetTest, DeltaRules) {
  CounterSet set({"packets", "bytes"});
  set.ForWorker(W(0)).Add(kPackets, 2);
  const CounterSnapshot s1 = set.Snapshot();
  set.ForWorker(W(3)).Add(kPackets, 5);
  set.ForWorker(W(3)).Add(kBytes, 50);
  const CounterSnapshot s2 = set.Snapshot();

  auto d = Delta(s1, s2);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(5u, d->deltas[kPackets]);
  EXPECT_EQ(50u, d->deltas[kBytes]);
  EXPECT_EQ(s2.tsc - s1.tsc, d->tsc_elapsed);

  EXPECT_EQ(StatsError::kNotOrdered, Delta(s2, s1).error());
  EXPECT_EQ(StatsError::kNotOrdered, Delta(s1, s1).error());

  CounterSet other({"packets", "bytes"});
  EXPECT_EQ(StatsError::kDifferentSource, Delta(s1, other.Snapshot()).error());

  set.Reset();
  EXPECT_EQ(StatsError::kEpochChanged, Delta(s2, set.Snapshot()).error());
}

// The grouping guarantee under real concurrency: four writer threads, each
// owning a worker slot, add (1 packet, 1500 bytes) as one Update; a reader
// snapshotting concurrently -- and resetting now and then -- must never see
// bytes != 1500 x packets for any worker or in total. Removing the sequence
// bumps breaks this within a few thousand snapshots.
TEST(CounterSetTest, SnapshotsNeverSplitAnUpdate) {
  CounterSet set({"packets", "bytes"});
  constexpr int kWriters = 4;
  constexpr uint64_t kPerWriter = 2'000'000;
  std::atomic<int> running{kWriters};
  std::vector<std::thread> writers;
  for (int t = 0; t < kWriters; t++) {
    writers.emplace_back([&, t] {
      for (uint64_t i = 0; i < kPerWriter; i++) {
        auto u = set.Updating(W(static_cast<uint16_t>(t * 7)));
        u.Add(kPackets, 1);
        u.Add(kBytes, 1500);
      }
      running--;
    });
  }

  uint64_t snapshots = 0;
  uint64_t split = 0;
  while (running.load() > 0) {
    const CounterSnapshot snap = set.Snapshot(true);
    snapshots++;
    if (snap.totals[kBytes] != 1500 * snap.totals[kPackets]) {
      split++;
    }
    for (int t = 0; t < kWriters; t++) {
      const WorkerId w = W(static_cast<uint16_t>(t * 7));
      if (snap.PerWorker(w, kBytes) != 1500 * snap.PerWorker(w, kPackets)) {
        split++;
      }
    }
    if (snapshots % 64 == 0) {
      set.Reset();
    }
  }
  for (auto &w : writers) {
    w.join();
  }
  EXPECT_EQ(0u, split) << "over " << snapshots << " snapshots";
  EXPECT_GT(snapshots, 10u);
}

// Single-writer adds lose nothing, whatever the reader does meanwhile.
TEST(CounterSetTest, ConcurrentAddsAreExact) {
  CounterSet set({"packets"});
  constexpr int kWriters = 4;
  constexpr uint64_t kPerWriter = 1'000'000;
  std::atomic<bool> done{false};
  std::thread reader([&] {
    while (!done.load()) {
      (void)set.Snapshot();
    }
  });
  std::vector<std::thread> writers;
  for (int t = 0; t < kWriters; t++) {
    writers.emplace_back([&, t] {
      const auto w = set.ForWorker(W(static_cast<uint16_t>(t)));
      for (uint64_t i = 0; i < kPerWriter; i++) {
        w.Add(kPackets, 1);
      }
    });
  }
  for (auto &w : writers) {
    w.join();
  }
  done = true;
  reader.join();
  EXPECT_EQ(kWriters * kPerWriter, set.Snapshot().totals[kPackets]);
}

// -- WorkerHistogram -----------------------------------------------------------

TEST(HistogramLayoutTest, Log2) {
  const HistogramLayout l = HistogramLayout::Log2();
  EXPECT_EQ(65u, l.buckets());
  EXPECT_EQ(0u, l.Index(0));
  EXPECT_EQ(1u, l.Index(1));
  EXPECT_EQ(2u, l.Index(2));
  EXPECT_EQ(2u, l.Index(3));
  EXPECT_EQ(3u, l.Index(4));
  EXPECT_EQ(10u, l.Index(1023));
  EXPECT_EQ(11u, l.Index(1024));
  EXPECT_EQ(64u, l.Index(UINT64_MAX));
  for (size_t i = 0; i < l.buckets(); i++) {
    EXPECT_EQ(i, l.Index(l.LowerBound(i))) << i;
    EXPECT_EQ(i, l.Index(l.UpperBound(i))) << i;
  }
}

TEST(HistogramLayoutTest, Linear) {
  const HistogramLayout l = HistogramLayout::Linear(100, 10);
  EXPECT_EQ(11u, l.buckets());
  EXPECT_EQ(0u, l.Index(99));
  EXPECT_EQ(1u, l.Index(100));
  EXPECT_EQ(9u, l.Index(999));
  EXPECT_EQ(10u, l.Index(1000));
  EXPECT_EQ(10u, l.Index(UINT64_MAX));
  EXPECT_EQ(199u, l.UpperBound(1));
  EXPECT_EQ(UINT64_MAX, l.UpperBound(10));
  EXPECT_EQ(1000u, l.LowerBound(10));
}

TEST(WorkerHistogramTest, RecordSnapshotPercentiles) {
  WorkerHistogram h(HistogramLayout::Linear(10, 100));
  // 1..100 on worker 0, 101..200 on worker 9.
  for (uint64_t v = 1; v <= 100; v++) {
    h.Record(W(0), v);
  }
  std::vector<uint64_t> batch;
  for (uint64_t v = 101; v <= 200; v++) {
    batch.push_back(v);
  }
  h.RecordBatch(W(9), batch);

  const HistogramSnapshot snap = h.Snapshot();
  EXPECT_EQ(200u, snap.count);
  EXPECT_EQ(200u * 201 / 2, snap.sum);
  uint64_t in_buckets = 0;
  for (uint64_t b : snap.buckets) {
    in_buckets += b;
  }
  EXPECT_EQ(200u, in_buckets);
  // The 50th percentile value is 100, in bucket [100, 110).
  EXPECT_EQ(109u, snap.PercentileUpperBound(50));
  EXPECT_EQ(209u, snap.PercentileUpperBound(100));
  EXPECT_EQ(9u, snap.PercentileUpperBound(0.1));
  EXPECT_EQ(0u, WorkerHistogram(HistogramLayout::Log2())
                    .Snapshot()
                    .PercentileUpperBound(99));
}

TEST(WorkerHistogramTest, ResetAndDelta) {
  WorkerHistogram h(HistogramLayout::Log2());
  h.Record(W(2), 5);
  const HistogramSnapshot s1 = h.Snapshot();
  h.Record(W(2), 5);
  h.Record(W(3), 1000);
  const HistogramSnapshot s2 = h.Snapshot();

  auto d = Delta(s1, s2);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(2u, d->count);
  EXPECT_EQ(1005u, d->sum);
  EXPECT_EQ(1u, d->buckets[h.layout().Index(5)]);
  EXPECT_EQ(1u, d->buckets[h.layout().Index(1000)]);

  h.Reset();
  const HistogramSnapshot s3 = h.Snapshot();
  EXPECT_EQ(0u, s3.count);
  EXPECT_EQ(StatsError::kEpochChanged, Delta(s2, s3).error());
  WorkerHistogram other(HistogramLayout::Log2());
  EXPECT_EQ(StatsError::kDifferentSource, Delta(s3, other.Snapshot()).error());
}

// Every snapshot taken while four workers record has count == sum(buckets)
// and a sum consistent with the one value each writer records.
TEST(WorkerHistogramTest, SnapshotsAreInternallyConsistent) {
  WorkerHistogram h(HistogramLayout::Log2());
  constexpr int kWriters = 4;
  constexpr uint64_t kPerWriter = 1'000'000;
  std::atomic<int> running{kWriters};
  std::vector<std::thread> writers;
  for (int t = 0; t < kWriters; t++) {
    writers.emplace_back([&, t] {
      for (uint64_t i = 0; i < kPerWriter; i++) {
        h.Record(W(static_cast<uint16_t>(t)), 100);
      }
      running--;
    });
  }
  uint64_t snapshots = 0;
  uint64_t inconsistent = 0;
  while (running.load() > 0) {
    const HistogramSnapshot snap = h.Snapshot();
    snapshots++;
    uint64_t in_buckets = 0;
    for (uint64_t b : snap.buckets) {
      in_buckets += b;
    }
    if (in_buckets != snap.count || snap.sum != 100 * snap.count) {
      inconsistent++;
    }
  }
  for (auto &w : writers) {
    w.join();
  }
  EXPECT_EQ(0u, inconsistent) << "over " << snapshots << " snapshots";
  EXPECT_EQ(kWriters * kPerWriter, h.Snapshot().count);
}

}  // namespace
}  // namespace bess::stats
