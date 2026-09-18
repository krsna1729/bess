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
// DPDK or topology changes the answer.) Zero-copy reservation is
// deliberately *not* benchmarked: DPDK 25.11's `rte_ring.h` has no
// zero-copy burst API (verified by grep -- no `zc_burst`/`zero_copy`
// symbols), so there is nothing to adopt; revisit if a future DPDK bump
// adds one.
//
// All calls below are the *explicit* sync-mode entry points
// (`mp_enqueue_burst`, `sc_dequeue_burst`, `mp_rts/hts_enqueue_burst`),
// never the generic `enqueue_burst`/`dequeue_burst` wrappers: the generic
// ones dispatch on the ring's creation flags at runtime (measured 10-30%
// slower across the board -- pure dispatch overhead for a caller like
// `Queue` that already knows its mode), and the explicit forms are
// exactly what the migrated `Queue` calls.
//
// Needs no EAL, no hugepages, no daemon: both rings live in plain
// `aligned_alloc` memory (`rte_ring_init` on caller-supplied memory,
// same as `Queue` already allocates its `llring` memory). `DRR`'s ring is
// SP/SC and intentionally excluded -- it can't show the MP/SC effect and
// must not muddy the result.

#include <benchmark/benchmark.h>
#include <glog/logging.h>

#include <rte_ring.h>
#include <rte_ring_hts.h>
#include <rte_ring_rts.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

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
    static std::atomic<int> id{0};
    char name[32];
    snprintf(name, sizeof(name), "ringbench_%d", id.fetch_add(1));
    size_t bytes = static_cast<size_t>(rte_ring_get_memsize(kSlots));
    const size_t align = 64;
    void *mem = std::aligned_alloc(align, (bytes + align - 1) / align * align);
    CHECK(mem != nullptr);
    Ring *r = static_cast<Ring *>(mem);
    CHECK(rte_ring_init(r, name, kSlots, Flags) == 0);
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

template <typename Ops>
void RunRingBenchmark(benchmark::State &state) {
  const int num_producers = static_cast<int>(state.range(0));
  const uint64_t total = kItemsPerProducer * static_cast<uint64_t>(num_producers);

  for (auto _ : state) {
    state.PauseTiming();
    typename Ops::Ring *ring = Ops::Create();

    // Per-producer private object tables (distinct addresses, no sharing).
    std::vector<std::vector<void *>> bufs(num_producers,
                                          std::vector<void *>(kBurst));
    for (int p = 0; p < num_producers; p++) {
      for (unsigned i = 0; i < kBurst; i++) {
        bufs[p][i] = reinterpret_cast<void *>(
            static_cast<uintptr_t>(0x1000 * (p + 1) + i + 1));
      }
    }

    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < num_producers; p++) {
      producers.emplace_back([&, p]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        // Items carry (producer_id, sequence) so the consumer can verify
        // exact delivery, not just the total count: a ring that dropped
        // one item and delivered another twice would still pass a pure
        // count check. Same encoding cost on every variant measured.
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
    state.ResumeTiming();

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
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
