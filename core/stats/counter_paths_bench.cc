// SPDX-License-Identifier: BSD-3-Clause

// Noticing a counter threshold: what each of the three paths costs (D-150,
// D-151, D-152, D-154).
//
// A controller wants to know when a per-key counter passes a limit. There are
// three ways to find out, and they pay in different places:
//
//   polling   the control side reads every key on an interval
//             (KeyedCounters::Total per key): K reads per poll, nothing on the
//             packet path, and a crossing is noticed up to one interval late.
//   sweep     a module's tick reads every armed key on the control side
//             (ThresholdTable::Sweep, D-151): the same K reads, on the
//             maintenance thread under the control-plane lock, noticed within
//             one tick.
//   events    the packet path checks the key it just bumped and posts a
//             WorkerEvent when it crosses (CrossingNotifier::Note, D-152): no
//             control-side reads at all, a per-packet tax, and exact only when
//             a key's traffic is pinned to one worker.
//
// So the comparison is K control-side reads per interval (polling, sweep)
// against a per-packet tax (events). Rows:
//
//   bump          the packet path's own counter write (Plane::Add)
//   bump-check    the packet path's write plus the one compare the event path
//                 makes after it, nothing crossing (CrossingNotifier::Crossed)
//   note-post     the crossing: one WorkerEvent that lands in the hub's ring
//                 (the ring is drained between batches, timer paused, so the
//                 row is the post and not the refusal a full ring gives)
//   arm           arming K keys (control side)
//   poll/K        one full poll of K keys (control side)
//   sweep/K       one full sweep of K armed keys, nothing crossing (the steady
//                 state of a tick)
//   sweep-cross/K one sweep where one key has crossed (fire-and-remove)
//
// Rows are meant to be run under omarchy-benchmark with isolated CPUs; on a
// shared host the spread is the answer, not the number.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "dataplane/worker_id.h"
#include "framework/threshold_table.h"
#include "stats/crossing_notifier.h"
#include "stats/event_hub.h"
#include "stats/keyed_counters.h"

namespace {

using bess::dataplane::WorkerId;
using bess::framework::ThresholdTable;
using bess::stats::CrossingNotifier;
using bess::stats::EventHub;
using bess::stats::KeyedCounters;

constexpr WorkerId kWorker{uint16_t{0}};
constexpr uint64_t kLimit = 1000;

// The packet path's own counter write: one relaxed load + store into this
// worker's plane, cycling over the key space so the line is not always hot.
// The args are powers of two, so the cycle is a mask: the packet path pays no
// division, and the row must not pay one either.
void BM_Bump(benchmark::State &state) {
  const size_t keys = static_cast<size_t>(state.range(0));
  KeyedCounters counters(keys, /*cells_per_key=*/1, /*workers=*/1);
  const KeyedCounters::Plane plane = counters.ForWorker(kWorker);
  size_t key = 0;
  for (auto _ : state) {
    plane.Add(key, 0, 1);
    key = (key + 1) & (keys - 1);
  }
  state.SetItemsProcessed(state.iterations());
}

// The event path's per-packet tax when nothing crosses: the same write, plus
// the one compare the packet path makes after it (`Crossed`, D-152's contract).
// The compare is live and almost never true here (it can only be true once in
// a run, as the count passes the limit). This row is what the control-side
// rows are weighed against: the compare is not free, so an event path is
// cheaper than a sweep only when a key's packets are fewer than the keys a
// sweep must read.
void BM_BumpAndCheck(benchmark::State &state) {
  const size_t keys = static_cast<size_t>(state.range(0));
  KeyedCounters counters(keys, /*cells_per_key=*/1, /*workers=*/1);
  const KeyedCounters::Plane plane = counters.ForWorker(kWorker);
  size_t key = 0;
  uint64_t value = 0;  // this key's running count, as a slot keeps it
  for (auto _ : state) {
    const uint64_t before = value;
    plane.Add(key, 0, 1);
    value = before + 1;
    if (CrossingNotifier::Crossed(before, value, kLimit)) [[unlikely]] {
      benchmark::DoNotOptimize(value);
    }
    key = (key + 1) & (keys - 1);
  }
  state.SetItemsProcessed(state.iterations());
}

// The crossing: one WorkerEvent posted to the hub, which is the whole of the
// event path's packet-path cost when a key crosses its limit.
void BM_NotePost(benchmark::State &state) {
  EventHub hub(/*log_capacity=*/16384, /*ring_capacity=*/256);
  CrossingNotifier notifier(hub, "bench");
  uint64_t key = 0;
  for (auto _ : state) {
    // The ring holds 256 per worker: drain it (untimed) and post a batch that
    // all lands, so the row is the landing post rather than a refusal.
    state.PauseTiming();
    hub.DrainWorkers();
    state.ResumeTiming();
    for (int i = 0; i < 16; i++) {
      benchmark::DoNotOptimize(notifier.Note(kWorker, key++, /*value=*/kLimit, kLimit));
    }
  }
  state.SetItemsProcessed(state.iterations() * 16);
}

// Arming K keys: what a controller or a module pays to set the sweep path up.
void BM_Arm(benchmark::State &state) {
  const size_t keys = static_cast<size_t>(state.range(0));
  ThresholdTable table;
  for (auto _ : state) {
    for (size_t key = 0; key < keys; key++) {
      table.Arm(key, kLimit);
    }
    state.PauseTiming();
    table.Clear();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(keys));
}

// One full poll: the control side reads every key's total.
void BM_PollAllKeys(benchmark::State &state) {
  const size_t keys = static_cast<size_t>(state.range(0));
  KeyedCounters counters(keys, /*cells_per_key=*/1, /*workers=*/1);
  const KeyedCounters::Plane plane = counters.ForWorker(kWorker);
  for (size_t key = 0; key < keys; key++) {
    plane.Add(key, 0, 7);
  }
  for (auto _ : state) {
    uint64_t sum = 0;
    for (size_t key = 0; key < keys; key++) {
      sum += counters.Total(key, 0);
    }
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(keys));
}

// One full sweep with nothing crossing: the steady state of a module's tick.
void BM_SweepSteady(benchmark::State &state) {
  const size_t keys = static_cast<size_t>(state.range(0));
  KeyedCounters counters(keys, /*cells_per_key=*/1, /*workers=*/1);
  const KeyedCounters::Plane plane = counters.ForWorker(kWorker);
  ThresholdTable table;
  for (size_t key = 0; key < keys; key++) {
    plane.Add(key, 0, 7);  // below the limit
    table.Arm(key, kLimit);
  }
  for (auto _ : state) {
    const ThresholdTable::Result result = table.Sweep(
        [&](uint64_t key) { return counters.Total(key, 0); },
        [] { return false; });
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(keys));
}

// One sweep where one key has crossed: the entry fires and is removed, and the
// next iteration arms it again above its new total (untimed), so every
// iteration walks K entries with exactly one firing -- the row's name.
void BM_SweepCrossing(benchmark::State &state) {
  const size_t keys = static_cast<size_t>(state.range(0));
  KeyedCounters counters(keys, /*cells_per_key=*/1, /*workers=*/1);
  const KeyedCounters::Plane plane = counters.ForWorker(kWorker);
  ThresholdTable table;
  for (size_t key = 0; key < keys; key++) {
    plane.Add(key, 0, 7);
    table.Arm(key, kLimit);
  }
  size_t crossing = 0;
  for (auto _ : state) {
    plane.Add(crossing, 0, kLimit);
    const ThresholdTable::Result result = table.Sweep(
        [&](uint64_t key) { return counters.Total(key, 0); },
        [] { return false; });
    benchmark::DoNotOptimize(result);
    state.PauseTiming();
    // Above the new total: the key is armed again and does not fire until it
    // is bumped again, so the armed set stays K for the whole run.
    table.Arm(crossing, counters.Total(crossing, 0) + kLimit);
    crossing = (crossing + 1) % keys;
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(keys));
}

}  // namespace

BENCHMARK(BM_Bump)->Arg(1024)->Arg(16384);
BENCHMARK(BM_BumpAndCheck)->Arg(1024)->Arg(16384);
BENCHMARK(BM_NotePost);
BENCHMARK(BM_Arm)->Arg(1024)->Arg(16384);
BENCHMARK(BM_PollAllKeys)->Arg(1024)->Arg(16384);
BENCHMARK(BM_SweepSteady)->Arg(1024)->Arg(16384);
BENCHMARK(BM_SweepCrossing)->Arg(1024)->Arg(16384);
