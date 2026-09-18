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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

// rte_ring benchmark: guards the ring fast path after the llring removal
// (see MODERNIZATION.md benchmark-backlog item 2). Originally written as
// an llring-vs-rte_ring comparison in exactly the `Queue` module's mode --
// MP enqueue-burst from N producer threads, SC dequeue-burst from one
// consumer -- at 1/2/4/8/16 producers; with explicit sync-mode entry
// points, rte_ring MP/SC measured faster-or-equal to llring everywhere
// (e.g. +26% at 1 producer, equal at 16), so `core/utils/llring.h` was
// deleted and `Queue`/`DRR`/`LockLessQueue` moved to `rte_ring`. The
// llring variant was removed along with the header; the rte variants stay
// as the regression guard for Stage 2 and the mempool experiment, which
// both touch allocation/queueing behavior.
//
// Variants: rte_ring MP/SC (`RING_F_SC_DEQ` -- Queue's mode); rte_ring
// MP_RTS/SC; rte_ring MP_HTS/SC. (RTS/HTS measured no better than classic
// MP/SC here, so nothing adopted them; kept as coverage in case a future
// DPDK or topology changes the answer.) Zero-copy reservation
// (`rte_ring_{en,de}queue_zc_burst_{start,finish}` in `rte_ring_peek_zc.h`)
// is deliberately *not* benchmarked: it wasn't needed to justify the
// removal, and its win applies to producers that can write directly into
// reserved slots, not to this pointer-handoff shape; a separate HTS+ZC
// optimization experiment may evaluate it later.
//
// All calls below are the *explicit* sync-mode entry points
// (`mp_enqueue_burst`, `sc_dequeue_burst`, `mp_rts/hts_enqueue_burst`),
// never the generic `enqueue_burst`/`dequeue_burst` wrappers: the generic
// ones dispatch on the ring's creation flags at runtime (measured 10-30%
// slower across the board -- pure dispatch overhead for a caller like
// `Queue` that already knows its mode), and the explicit forms are
// exactly what the migrated `Queue` calls.
//
// Each (variant, producer count) runs in two phases: an untimed
// correctness phase moving the full quota with exact per-producer
// sequence verification, then the timed loop where the consumer only
// counts (a verifying consumer becomes the ceiling at high producer
// counts and would hide producer-side differences). Pass --pin_threads
// to pin threads best-effort to distinct allowed CPUs; the default is
// unpinned, because on a shared host pinning can force threads onto busy
// cores (measured far slower here -- use it on quiet machines only).
//
// Needs no EAL, no hugepages, no daemon: rings live in plain caller-owned
// memory via the shared `utils/rte_ring_alloc.h` helpers.

#include <benchmark/benchmark.h>
#include <glog/logging.h>

#include <rte_ring.h>
#include <rte_ring_hts.h>
#include <rte_ring_rts.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sched.h>
#include <thread>
#include <vector>

#include "utils/rte_ring_alloc.h"

namespace {

const unsigned kSlots = 4096;
const unsigned kBurst = 32;
const uint64_t kItemsPerProducer = 1 << 18;  // 256K; 16 producers -> 4M/run

// Per-variant ring operations. Enqueue/Dequeue return the actual count
// moved.

template <unsigned Flags>
struct RteRingOps {
  using Ring = rte_ring;

  static Ring *Create() {
    ssize_t bytes = rte_ring_get_memsize(kSlots);
    CHECK(bytes > 0);
    void *mem = bess::utils::AllocRingMem(static_cast<size_t>(bytes));
    CHECK(mem != nullptr);
    Ring *r = static_cast<Ring *>(mem);
    std::string name = bess::utils::NewRingName("ringbench");
    CHECK(rte_ring_init(r, name.c_str(), kSlots, Flags) == 0);
    return r;
  }

  static void Destroy(Ring *r) { std::free(r); }
};

struct RteMpscOps : public RteRingOps<RING_F_SC_DEQ> {
  static unsigned EnqueueBurst(Ring *r, void *const *objs, unsigned n) {
    return rte_ring_mp_enqueue_burst(r, objs, n, nullptr);
  }

  static unsigned DequeueBurst(Ring *r, void **objs, unsigned n) {
    return rte_ring_sc_dequeue_burst(r, objs, n, nullptr);
  }
};

struct RteRtsOps : public RteRingOps<RING_F_SC_DEQ | RING_F_MP_RTS_ENQ> {
  static unsigned EnqueueBurst(Ring *r, void *const *objs, unsigned n) {
    return rte_ring_mp_rts_enqueue_burst(r, objs, n, nullptr);
  }

  static unsigned DequeueBurst(Ring *r, void **objs, unsigned n) {
    // RTS changes only the producer-side protocol; with RING_F_SC_DEQ the
    // consumer side is plain single-consumer.
    return rte_ring_sc_dequeue_burst(r, objs, n, nullptr);
  }
};

struct RteHtsOps : public RteRingOps<RING_F_SC_DEQ | RING_F_MP_HTS_ENQ> {
  static unsigned EnqueueBurst(Ring *r, void *const *objs, unsigned n) {
    return rte_ring_mp_hts_enqueue_burst(r, objs, n, nullptr);
  }

  static unsigned DequeueBurst(Ring *r, void **objs, unsigned n) {
    // Same as RTS above: HTS is producer-side only here.
    return rte_ring_sc_dequeue_burst(r, objs, n, nullptr);
  }
};

// Best-effort thread pinning: spread producers and consumer over the
// process's allowed CPUs so scheduler placement isn't an uncontrolled
// variable in a benchmark whose entire subject is inter-core contention.
// Silently runs unpinned where affinity is unavailable; BESS is
// Linux-only, so no portability shim.
// Set via --pin_threads (stripped from argv in main before Google
// Benchmark sees it). Default off: on a shared/noisy host, hard pinning
// forces threads onto whatever cores happen to be busy (measured ~500x
// slower in this sandbox), while the scheduler migrates away from them.
// Enable explicitly for scaling studies on a quiet/dedicated machine.
bool g_pin_threads = false;

void PinThread(int idx) {
  if (!g_pin_threads) {
    return;
  }
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
    return;
  }
  int ncpu = CPU_COUNT(&allowed);
  if (ncpu <= 0) {
    return;
  }
  int want = idx % ncpu;
  int seen = -1;
  int target = -1;
  for (int c = 0; c < CPU_SETSIZE; c++) {
    if (!CPU_ISSET(c, &allowed)) {
      continue;
    }
    if (++seen == want) {
      target = c;
      break;
    }
  }
  if (target < 0) {
    return;
  }
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(target, &one);
  (void)sched_setaffinity(0, sizeof(one), &one);
}

// Spawns parked producers; the caller sets `start` and joins. NOTE: `ring`
// is captured by value, not by reference -- it is this helper's parameter
// and dies on return, while the spawned threads outlive the call.
template <typename Ops>
std::vector<std::thread> SpawnProducers(
    typename Ops::Ring *ring, int num_producers,
    std::vector<std::vector<void *>> &bufs, std::atomic<bool> &start) {
  std::vector<std::thread> producers;
  for (int p = 0; p < num_producers; p++) {
    producers.emplace_back([&, p, ring]() {
      PinThread(p);
      while (!start.load(std::memory_order_acquire)) {
      }
      // Items carry (producer_id, sequence). The encoding work is part of
      // every variant's cost equally; the consumer side decides whether to
      // verify it (correctness phase) or just count (timed phase).
      uint64_t seq = 0;
      while (seq < kItemsPerProducer) {
        // Cap the last request so producers enqueue exactly their quota
        // (a full 32-wide final burst would overshoot it).
        uint64_t left = kItemsPerProducer - seq;
        unsigned want = (left < kBurst) ? static_cast<unsigned>(left) : kBurst;
        for (unsigned i = 0; i < want; i++) {
          bufs[p][i] = reinterpret_cast<void *>((static_cast<uint64_t>(p) << 32) | (seq + i));
        }
        seq += Ops::EnqueueBurst(ring, bufs[p].data(), want);
      }
    });
  }
  return producers;
}

// Untimed correctness phase, run once per (variant, producer count):
// moves the full quota through a fresh ring checking exact per-producer
// sequences, so the timed phase below can count cheaply. A ring that
// dropped one item and delivered another twice would still pass a pure
// count check -- this phase exists so the timed loop doesn't have to.
template <typename Ops>
void VerifyRing(int num_producers) {
  const uint64_t total =
      kItemsPerProducer * static_cast<uint64_t>(num_producers);
  typename Ops::Ring *ring = Ops::Create();
  std::vector<std::vector<void *>> bufs(num_producers,
                                        std::vector<void *>(kBurst));
  std::atomic<bool> start{false};
  std::vector<std::thread> producers =
      SpawnProducers<Ops>(ring, num_producers, bufs, start);
  PinThread(num_producers);
  start.store(true, std::memory_order_release);
  std::vector<void *> out(kBurst);
  std::vector<uint64_t> expected(num_producers, 0);
  uint64_t consumed = 0;
  while (consumed < total) {
    unsigned n = Ops::DequeueBurst(ring, out.data(), kBurst);
    for (unsigned i = 0; i < n; i++) {
      uint64_t v = reinterpret_cast<uint64_t>(out[i]);
      int src = static_cast<int>(v >> 32);
      CHECK(src >= 0 && src < num_producers);
      CHECK((v & 0xffffffffu) == expected[src]);
      expected[src]++;
    }
    consumed += n;
  }
  CHECK(consumed == total);
  for (auto &t : producers) {
    t.join();
  }
  Ops::Destroy(ring);
}

template <typename Ops>
void RunRingBenchmark(benchmark::State &state) {
  const int num_producers = static_cast<int>(state.range(0));
  const uint64_t total = kItemsPerProducer * static_cast<uint64_t>(num_producers);

  VerifyRing<Ops>(num_producers);

  for (auto _ : state) {
    state.PauseTiming();
    typename Ops::Ring *ring = Ops::Create();
    std::vector<std::vector<void *>> bufs(num_producers,
                                          std::vector<void *>(kBurst));
    std::atomic<bool> start{false};
    std::vector<std::thread> producers =
        SpawnProducers<Ops>(ring, num_producers, bufs, start);
    state.ResumeTiming();

    PinThread(num_producers);
    start.store(true, std::memory_order_release);
    std::vector<void *> out(kBurst);
    uint64_t consumed = 0;
    while (consumed < total) {
      consumed += Ops::DequeueBurst(ring, out.data(), kBurst);
    }
    benchmark::DoNotOptimize(consumed);

    for (auto &t : producers) {
      t.join();
    }
    CHECK(consumed == total);

    state.PauseTiming();
    Ops::Destroy(ring);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * total);
}

void BM_RingRteMpsc(benchmark::State &state) {
  RunRingBenchmark<RteMpscOps>(state);
}
BENCHMARK(BM_RingRteMpsc)->RangeMultiplier(2)->Range(1, 16);

void BM_RingRteRtsSc(benchmark::State &state) {
  RunRingBenchmark<RteRtsOps>(state);
}
BENCHMARK(BM_RingRteRtsSc)->RangeMultiplier(2)->Range(1, 16);

void BM_RingRteHtsSc(benchmark::State &state) {
  RunRingBenchmark<RteHtsOps>(state);
}
BENCHMARK(BM_RingRteHtsSc)->RangeMultiplier(2)->Range(1, 16);

}  // namespace

int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);
  // Strip our own flag before Google Benchmark parses argv (it errors on
  // unrecognized arguments).
  int w = 1;
  for (int r = 1; r < argc; r++) {
    if (std::string(argv[r]) == "--pin_threads") {
      g_pin_threads = true;
    } else {
      argv[w++] = argv[r];
    }
  }
  argc = w;
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
