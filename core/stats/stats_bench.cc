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

// K6: what worker-local statistics cost on the packet path, and what the
// alternatives cost.
//
//   single thread -- one batch's accounting (1 batch, 32 packets, bytes):
//     cell        three single-writer atomic cells, ungrouped. A relaxed
//                 load + add + relaxed store compiles to the same mov/add/mov
//                 as a plain `+=` (checked in the disassembly), so there is no
//                 separate plain-uint64 row: the atomics cost nothing on x86
//                 and make the controller's read well-defined.
//     update      the same three inside one Update (sequence bump pair)
//     shared_rmw  three lock-prefixed fetch_adds on one shared line
//   threads (1..4, each its own worker slot) -- the same update, versus one
//   shared set of atomic counters every thread fetch_adds, versus per-thread
//   plain counters packed into one line (false sharing).
//   histogram -- Record() per value and RecordBatch() of 32 values.
//   snapshot -- the controller-side cost of reading 64 worker slots.
//
// Rows are meant to be run under omarchy-benchmark with isolated CPUs; each
// benchmark thread pins itself to one CPU of the launch mask, because an
// isolated partition does no load balancing.

#include <benchmark/benchmark.h>
#include <sched.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <random>
#include <vector>

#include "stats/counter_set.h"
#include "stats/worker_histogram.h"

namespace {

using bess::stats::CounterSet;
using bess::stats::HistogramLayout;
using bess::stats::WorkerHistogram;
using bess::stats::WorkerId;

enum : size_t { kBatches, kPackets, kBytes };
constexpr uint64_t kBatchPackets = 32;
constexpr uint64_t kBatchBytes = 32 * 700;

cpu_set_t LaunchAffinity() {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  return set;
}
const cpu_set_t kLaunchAffinity = LaunchAffinity();

void PinBenchmarkThread(const benchmark::State &state) {
  int seen = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
    if (CPU_ISSET(cpu, &kLaunchAffinity) &&
        seen++ == state.thread_index() % CPU_COUNT(&kLaunchAffinity)) {
      cpu_set_t one;
      CPU_ZERO(&one);
      CPU_SET(cpu, &one);
      sched_setaffinity(0, sizeof(one), &one);
      return;
    }
  }
}

// -- single thread -------------------------------------------------------------

void BM_AccountCell(benchmark::State &state) {
  CounterSet set({"batches", "packets", "bytes"});
  const auto w = set.ForWorker(WorkerId(0));
  for (auto _ : state) {
    w.Add(kBatches, 1);
    w.Add(kPackets, kBatchPackets);
    w.Add(kBytes, kBatchBytes);
  }
  benchmark::DoNotOptimize(set.Snapshot().totals);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountCell);

void BM_AccountUpdate(benchmark::State &state) {
  CounterSet set({"batches", "packets", "bytes"});
  for (auto _ : state) {
    auto u = set.Updating(WorkerId(0));
    u.Add(kBatches, 1);
    u.Add(kPackets, kBatchPackets);
    u.Add(kBytes, kBatchBytes);
  }
  benchmark::DoNotOptimize(set.Snapshot().totals);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountUpdate);

void BM_AccountSharedRmw(benchmark::State &state) {
  struct alignas(64) {
    std::atomic<uint64_t> batches{0}, packets{0}, bytes{0};
  } stats;
  for (auto _ : state) {
    stats.batches.fetch_add(1, std::memory_order_relaxed);
    stats.packets.fetch_add(kBatchPackets, std::memory_order_relaxed);
    stats.bytes.fetch_add(kBatchBytes, std::memory_order_relaxed);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AccountSharedRmw);

// -- threads -------------------------------------------------------------------

CounterSet &ThreadSet() {
  static CounterSet set({"batches", "packets", "bytes"});
  return set;
}

void BM_ThreadsWorkerLocalUpdate(benchmark::State &state) {
  PinBenchmarkThread(state);
  CounterSet &set = ThreadSet();
  const WorkerId me(static_cast<uint16_t>(state.thread_index()));
  for (auto _ : state) {
    auto u = set.Updating(me);
    u.Add(kBatches, 1);
    u.Add(kPackets, kBatchPackets);
    u.Add(kBytes, kBatchBytes);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ThreadsWorkerLocalUpdate)->ThreadRange(1, 4)->UseRealTime();

struct alignas(64) SharedAtomics {
  std::atomic<uint64_t> batches{0}, packets{0}, bytes{0};
};
SharedAtomics g_shared;

void BM_ThreadsSharedRmw(benchmark::State &state) {
  PinBenchmarkThread(state);
  for (auto _ : state) {
    g_shared.batches.fetch_add(1, std::memory_order_relaxed);
    g_shared.packets.fetch_add(kBatchPackets, std::memory_order_relaxed);
    g_shared.bytes.fetch_add(kBatchBytes, std::memory_order_relaxed);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ThreadsSharedRmw)->ThreadRange(1, 4)->UseRealTime();

// Per-thread counters, but packed: four threads' 16-byte records share one
// line. Correct ownership, wrong layout. Caveat: in a loop this tight each
// core forwards its own stores from its store buffer, which hides most of the
// line ping-pong, so this row does *not* demonstrate a false-sharing penalty;
// it is kept as a lower bound. Worker slots are padded regardless.
struct alignas(64) PackedLine {
  struct {
    std::atomic<uint64_t> packets{0}, bytes{0};
  } per_thread[4];
};
PackedLine g_packed;

void BM_ThreadsFalseSharing(benchmark::State &state) {
  PinBenchmarkThread(state);
  auto &mine = g_packed.per_thread[state.thread_index() % 4];
  for (auto _ : state) {
    mine.packets.store(mine.packets.load(std::memory_order_relaxed) +
                           kBatchPackets,
                       std::memory_order_relaxed);
    mine.bytes.store(mine.bytes.load(std::memory_order_relaxed) + kBatchBytes,
                     std::memory_order_relaxed);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ThreadsFalseSharing)->ThreadRange(1, 4)->UseRealTime();

// -- histogram -----------------------------------------------------------------

std::vector<uint64_t> Latencies(size_t n) {
  std::mt19937_64 rng(0x6b36);
  std::lognormal_distribution<double> d(7.0, 1.0);  // ~1 us-ish cycles
  std::vector<uint64_t> v(n);
  for (auto &x : v) {
    x = static_cast<uint64_t>(d(rng));
  }
  return v;
}

void BM_HistogramRecord(benchmark::State &state) {
  WorkerHistogram h(state.range(0) ? HistogramLayout::Log2()
                                   : HistogramLayout::Linear(64, 256));
  const std::vector<uint64_t> values = Latencies(1024);
  size_t i = 0;
  for (auto _ : state) {
    h.Record(WorkerId(0), values[i++ & 1023]);
  }
  state.SetItemsProcessed(state.iterations());
  state.SetLabel(state.range(0) ? "log2" : "linear");
}
BENCHMARK(BM_HistogramRecord)->Arg(1)->Arg(0);

void BM_HistogramRecordBatch32(benchmark::State &state) {
  WorkerHistogram h(HistogramLayout::Log2());
  const std::vector<uint64_t> values = Latencies(1024);
  size_t offset = 0;
  for (auto _ : state) {
    h.RecordBatch(WorkerId(0), std::span(values).subspan(offset, 32));
    offset = (offset + 32) & 1023;
  }
  state.SetItemsProcessed(state.iterations() * 32);
}
BENCHMARK(BM_HistogramRecordBatch32);

// -- snapshot (control side) -----------------------------------------------------

void BM_SnapshotCounters(benchmark::State &state) {
  std::vector<std::string> names(static_cast<size_t>(state.range(0)), "c");
  CounterSet set(std::move(names));
  for (auto _ : state) {
    benchmark::DoNotOptimize(set.Snapshot());
  }
  state.counters["storage_bytes"] = static_cast<double>(set.storage_bytes());
}
BENCHMARK(BM_SnapshotCounters)->Arg(3)->Arg(32);

void BM_SnapshotHistogram(benchmark::State &state) {
  WorkerHistogram h(HistogramLayout::Log2());
  for (auto _ : state) {
    benchmark::DoNotOptimize(h.Snapshot());
  }
  state.counters["storage_bytes"] = static_cast<double>(h.storage_bytes());
}
BENCHMARK(BM_SnapshotHistogram);

}  // namespace
