// SPDX-License-Identifier: BSD-3-Clause

// Handoff mechanisms compared (roadmap M11, Decision D-054).
//
// A handoff moves a burst of packets, each with a small typed context, from a
// worker to another thread (a slow path, a service worker) and, after that
// thread has done its work, back. The mechanism was chosen by running the same
// round trip through each candidate, not by preference.
//
// Candidates (the ring underneath; every call is the explicit sync-mode entry
// point, never the flag-dispatching wrapper, except where the name says so):
//
//   ptr-ring      rte_ring of bare pointers, SP enqueue / SC dequeue: exactly
//                 what the Queue module does (D-036). The baseline. It cannot
//                 carry a context.
//   elem8..elem64 rte_ring_elem with 8, 16, 24, 32 and 64 byte elements:
//                 {packet, context} in the ring slot, no second ring and no
//                 side array. 8 bytes is the pointer alone through the element
//                 API (what a handoff with no context costs).
//   elem16-generic the 16-byte element through rte_ring_enqueue_burst_elem /
//                 rte_ring_dequeue_burst_elem, which dispatch on the ring's
//                 creation flags at run time: the cost of inspecting the
//                 topology per enqueue.
//   elem16-mpsc / elem16-mpmc  the same element with multi-producer enqueue
//                 (CAS on the producer head) and single / multi consumer.
//   cached-spsc   a purpose-built single-producer single-consumer ring with the
//                 producer and consumer each caching the other's index (the
//                 textbook cache-line-traffic optimisation). Here only as a
//                 yardstick: the roadmap says not to add another queue
//                 algorithm, and this measures what that rule costs.
//   channel-*     the real HandoffChannel (dataplane/handoff.h).
//
// Round trip: thread A enqueues a burst on the forward ring, thread B (pinned
// to a second CPU) dequeues it and enqueues it unchanged on the return ring, A
// dequeues it. One burst is in flight at a time, so this is the latency of a
// handoff and its resume, per burst and per packet. Two threads are pinned
// from the CPUs the process may use (run under `taskset -c A,B`); with fewer
// than two CPUs the benchmark is skipped, because two threads spinning on one
// CPU measure the scheduler.
//
// Counters: ns_per_burst (round trip), ns_per_item, bytes_per_item (ring
// element), ring_bytes. Every run first moves a verified stream (each item's
// pointer and context checked on return) before the timed loop.

#include "utils/logging.h"
#include <benchmark/benchmark.h>

#include <rte_ring.h>
#include <rte_ring_elem.h>

#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "arch/cpu.h"
#include "dataplane/handoff.h"
#include "utils/dpdk_memory.h"
#include "utils/rte_ring_alloc.h"

namespace {

constexpr unsigned kSlots = 1024;
constexpr unsigned kMaxBurst = 32;
constexpr size_t kMaxItemBytes = 64;

// The process's CPU allowance, snapshotted before any thread pins itself.
cpu_set_t SnapshotAffinity() {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  return set;
}
const cpu_set_t g_allowed = SnapshotAffinity();

// Pins the calling thread to the nth allowed CPU; false if there is none.
bool PinToNth(int nth) {
  int seen = -1;
  for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
    if (CPU_ISSET(cpu, &g_allowed) && ++seen == nth) {
      cpu_set_t one;
      CPU_ZERO(&one);
      CPU_SET(cpu, &one);
      return sched_setaffinity(0, sizeof(one), &one) == 0;
    }
  }
  return false;
}

// A spin that cannot hang a session: it gives up (and fails the run) after
// twenty seconds without progress.
class SpinGuard {
 public:
  void Check() {
    if ((++spins_ & 0xFFFFF) == 0 &&
        std::chrono::steady_clock::now() - start_ > std::chrono::seconds(20)) {
      LOG(FATAL) << "handoff_bench: a spin made no progress for 20 s";
    }
  }

 private:
  uint64_t spins_ = 0;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// -- ring candidates --------------------------------------------------------------
//
// Each candidate: Create(), Destroy(), Enq(ring, items, n) and Deq(ring, items,
// n) returning how many moved, and kItemBytes. Items are kItemBytes each; the
// first 8 bytes are a pointer, the rest a context.

// DPDK 25.11's rte_ring_elem.h lacks its own `extern "C"` (rte_ring.h includes
// it before opening its block), so rte_ring_get_memsize_elem is declared with
// C++ linkage and does not link. This declares the same C function under
// another name.
extern "C" ssize_t BessRingMemsizeElem(unsigned esize, unsigned count)
    __asm__("rte_ring_get_memsize_elem");

// Where a ring's memory comes from: 0 = aligned_alloc (what the Queue module
// does), 1 = DPDK's heap (what the channel uses) at 64-byte alignment, 2 = DPDK's
// heap at 128.
template <unsigned Flags, int Source = 0>
rte_ring *NewRing(size_t esize) {
  const ssize_t bytes = esize == 0 ? rte_ring_get_memsize(kSlots)
                                   : BessRingMemsizeElem(static_cast<unsigned>(esize), kSlots);
  CHECK_GT(bytes, 0);
  void *memory = Source == 0 ? bess::utils::AllocRingMem(static_cast<size_t>(bytes))
                             : bess::utils::DpdkAllocate(static_cast<size_t>(bytes),
                                                         Source == 1 ? 64 : 128);
  CHECK(memory != nullptr);
  auto *ring = static_cast<rte_ring *>(memory);
  const std::string name = bess::utils::NewRingName("handoffbench");
  CHECK_EQ(0, rte_ring_init(ring, name.c_str(), kSlots, Flags));
  return ring;
}

template <int Source>
void FreeRing(rte_ring *r) {
  if (Source == 0) {
    std::free(r);
  } else {
    bess::utils::DpdkFree(r);
  }
}

struct PtrRing {
  static constexpr size_t kItemBytes = 8;
  using Ring = rte_ring;
  static Ring *Create() { return NewRing<RING_F_SP_ENQ | RING_F_SC_DEQ>(0); }
  static void Destroy(Ring *r) { std::free(r); }
  static unsigned Enq(Ring *r, const void *items, unsigned n) {
    return rte_ring_sp_enqueue_burst(r, static_cast<void *const *>(items), n,
                                     nullptr);
  }
  static unsigned Deq(Ring *r, void *items, unsigned n) {
    return rte_ring_sc_dequeue_burst(r, static_cast<void **>(items), n, nullptr);
  }
  static size_t RingBytes() { return rte_ring_get_memsize(kSlots); }
};

enum class Mode { kSpSc, kMpSc, kMpMc, kGeneric };

template <size_t E, Mode M, int Source = 0>
struct ElemRing {
  static constexpr size_t kItemBytes = E;
  using Ring = rte_ring;
  static constexpr unsigned kFlags =
      M == Mode::kSpSc  ? (RING_F_SP_ENQ | RING_F_SC_DEQ)
      : M == Mode::kMpSc ? RING_F_SC_DEQ
      : M == Mode::kMpMc ? 0u
                         : (RING_F_SP_ENQ | RING_F_SC_DEQ);
  static Ring *Create() { return NewRing<kFlags, Source>(E); }
  static void Destroy(Ring *r) { FreeRing<Source>(r); }
  static unsigned Enq(Ring *r, const void *items, unsigned n) {
    if constexpr (M == Mode::kSpSc) {
      return rte_ring_sp_enqueue_burst_elem(r, items, E, n, nullptr);
    } else if constexpr (M == Mode::kMpSc || M == Mode::kMpMc) {
      return rte_ring_mp_enqueue_burst_elem(r, items, E, n, nullptr);
    } else {
      return rte_ring_enqueue_burst_elem(r, items, E, n, nullptr);
    }
  }
  static unsigned Deq(Ring *r, void *items, unsigned n) {
    if constexpr (M == Mode::kMpMc) {
      return rte_ring_mc_dequeue_burst_elem(r, items, E, n, nullptr);
    } else if constexpr (M == Mode::kGeneric) {
      return rte_ring_dequeue_burst_elem(r, items, E, n, nullptr);
    } else {
      return rte_ring_sc_dequeue_burst_elem(r, items, E, n, nullptr);
    }
  }
  static size_t RingBytes() {
    return static_cast<size_t>(BessRingMemsizeElem(E, kSlots));
  }
};

// A single-producer single-consumer ring in which each side caches the other's
// index and re-reads it only when the cache says full (producer) or empty
// (consumer).
template <size_t E>
struct CachedSpsc {
  static constexpr size_t kItemBytes = E;
  struct Ring {
    static constexpr size_t kLine = bess::arch::kCacheLineSize;
    alignas(kLine) std::atomic<uint32_t> head{0};  // producer writes
    uint32_t cached_tail = 0;                      // producer's copy of tail
    alignas(kLine) std::atomic<uint32_t> tail{0};  // consumer writes
    uint32_t cached_head = 0;                      // consumer's copy of head
    alignas(kLine) uint32_t mask = kSlots - 1;
    alignas(kLine) std::byte slots[kSlots * E];
  };
  static Ring *Create() {
    void *memory = std::aligned_alloc(64, (sizeof(Ring) + 63) / 64 * 64);
    CHECK(memory != nullptr);
    return new (memory) Ring();
  }
  static void Destroy(Ring *r) {
    r->~Ring();
    std::free(r);
  }
  static unsigned Enq(Ring *r, const void *items, unsigned n) {
    const uint32_t head = r->head.load(std::memory_order_relaxed);
    uint32_t free = kSlots - (head - r->cached_tail);
    if (free < n) {
      r->cached_tail = r->tail.load(std::memory_order_acquire);
      free = kSlots - (head - r->cached_tail);
    }
    const unsigned k = std::min<unsigned>(n, free);
    const uint32_t at = head & r->mask;
    const unsigned first = std::min<unsigned>(k, kSlots - at);
    std::memcpy(r->slots + at * E, items, first * E);
    std::memcpy(r->slots, static_cast<const std::byte *>(items) + first * E,
                (k - first) * E);
    r->head.store(head + k, std::memory_order_release);
    return k;
  }
  static unsigned Deq(Ring *r, void *items, unsigned n) {
    const uint32_t tail = r->tail.load(std::memory_order_relaxed);
    uint32_t avail = r->cached_head - tail;
    if (avail < n) {
      r->cached_head = r->head.load(std::memory_order_acquire);
      avail = r->cached_head - tail;
    }
    const unsigned k = std::min<unsigned>(n, avail);
    const uint32_t at = tail & r->mask;
    const unsigned first = std::min<unsigned>(k, kSlots - at);
    std::memcpy(items, r->slots + at * E, first * E);
    std::memcpy(static_cast<std::byte *>(items) + first * E, r->slots,
                (k - first) * E);
    r->tail.store(tail + k, std::memory_order_release);
    return k;
  }
  static size_t RingBytes() { return sizeof(Ring); }
};

// The real HandoffChannel (dataplane/handoff.h), same Ops interface, so that its
// bookkeeping (the closed check, the counters, nulling the handed-over
// pointers) is measured against the bare ring. Items are
// PuntItem<Context>: the packet pointer first, then the context's words.
template <class Context, bess::dataplane::HandoffTopology Topology>
struct ChannelOps {
  using Channel = bess::dataplane::HandoffChannel<Context, Topology>;
  using Item = bess::dataplane::PuntItem<Context>;
  using Ring = Channel;
  static constexpr size_t kItemBytes = sizeof(Item);
  static Ring *Create() {
    bess::dataplane::HandoffConfig config;
    config.capacity = kSlots - 1;  // the raw rings above hold kSlots - 1 too
    auto created = Channel::Create(config);
    CHECK(created.has_value());
    return created->release();
  }
  static void Destroy(Ring *r) { typename Channel::Ptr owner(r); }
  static unsigned Enq(Ring *r, const void *items, unsigned n) {
    return static_cast<unsigned>(
        r->TryPuntBurst(std::span<Item>(
                            const_cast<Item *>(static_cast<const Item *>(items)), n))
            .accepted);
  }
  static unsigned Deq(Ring *r, void *items, unsigned n) {
    return static_cast<unsigned>(
        r->Dequeue(std::span<Item>(static_cast<Item *>(items), n)));
  }
  static size_t RingBytes() { return r_bytes(); }

 private:
  static size_t r_bytes() {
    return sizeof(Channel) + static_cast<size_t>(BessRingMemsizeElem(
                                 static_cast<unsigned>(kItemBytes), kSlots));
  }
};

// The roadmap's sketch as an API: one packet per call, on both sides
// (TryPunt(packet, context), Dequeue of one). The same channel and ring, so the
// difference from ChannelOps is the cost of a per-packet interface.
template <class Context, bess::dataplane::HandoffTopology Topology>
struct ChannelSingleOps : ChannelOps<Context, Topology> {
  using Base = ChannelOps<Context, Topology>;
  using typename Base::Item;
  using typename Base::Ring;
  static unsigned Enq(Ring *r, const void *items, unsigned n) {
    const Item *in = static_cast<const Item *>(items);
    unsigned taken = 0;
    for (; taken < n; taken++) {
      bess::PacketHandle packet = in[taken].packet;
      if (!r->TryPunt(packet, in[taken].context).has_value()) {
        break;
      }
    }
    return taken;
  }
  static unsigned Deq(Ring *r, void *items, unsigned n) {
    Item *out = static_cast<Item *>(items);
    unsigned taken = 0;
    for (; taken < n; taken++) {
      if (r->Dequeue(std::span<Item>(out + taken, 1)) == 0) {
        break;
      }
    }
    return taken;
  }
};

// -- the round trip -----------------------------------------------------------------

// Fills n items: pointer = a recognisable non-null value, the rest of the item
// a function of (seq, slot) so a corrupted or reordered item is detected.
template <size_t E>
void Fill(std::byte *items, unsigned n, uint64_t seq) {
  for (unsigned i = 0; i < n; i++) {
    std::byte *item = items + i * E;
    const uint64_t pointer = 0x10000 + ((seq + i) & 0xFFFFF) * 64;
    std::memcpy(item, &pointer, sizeof(pointer));
    for (size_t b = sizeof(pointer); b + 8 <= E; b += 8) {
      const uint64_t word = (seq + i) * 0x9E3779B97F4A7C15ull + b;
      std::memcpy(item + b, &word, 8);
    }
  }
}

template <size_t E>
bool Check(const std::byte *items, unsigned n, uint64_t seq) {
  alignas(8) std::byte expect[kMaxBurst * kMaxItemBytes];
  Fill<E>(expect, n, seq);
  return std::memcmp(items, expect, n * E) == 0;
}

template <class Ops>
void RoundTrip(benchmark::State &state) {
  constexpr size_t E = Ops::kItemBytes;
  const unsigned burst = static_cast<unsigned>(state.range(0));
  if (CPU_COUNT(&g_allowed) < 2) {
    state.SkipWithMessage("needs two CPUs: run under taskset -c A,B");
    return;
  }
  typename Ops::Ring *forward = Ops::Create();
  typename Ops::Ring *back = Ops::Create();
  std::atomic<bool> stop{false}, ready{false};
  std::thread service([&] {
    PinToNth(1);
    alignas(64) std::byte buffer[kMaxBurst * kMaxItemBytes];
    SpinGuard guard;
    ready.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_relaxed)) {
      const unsigned m = Ops::Deq(forward, buffer, kMaxBurst);
      for (unsigned sent = 0; sent < m;) {
        sent += Ops::Enq(back, buffer + sent * E, m - sent);
      }
      guard.Check();
    }
  });
  PinToNth(0);
  while (!ready.load(std::memory_order_acquire)) {
  }

  alignas(64) std::byte buffer[kMaxBurst * kMaxItemBytes];
  SpinGuard guard;
  auto one_round_trip = [&](uint64_t seq) {
    for (unsigned sent = 0; sent < burst;) {
      sent += Ops::Enq(forward, buffer + sent * E, burst - sent);
      guard.Check();
    }
    for (unsigned got = 0; got < burst;) {
      got += Ops::Deq(back, buffer + got * E, burst - got);
      guard.Check();
    }
    benchmark::DoNotOptimize(seq);
  };

  // Untimed: a verified stream.
  uint64_t seq = 0;
  bool intact = true;
  for (int i = 0; i < 20000; i++, seq += burst) {
    Fill<E>(buffer, burst, seq);
    one_round_trip(seq);
    intact &= Check<E>(buffer, burst, seq);
  }
  CHECK(intact) << "an item came back changed";

  Fill<E>(buffer, burst, seq);
  const auto start = std::chrono::steady_clock::now();
  for (auto _ : state) {
    one_round_trip(seq);
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  stop.store(true, std::memory_order_relaxed);
  service.join();

  const double iterations = static_cast<double>(state.iterations());
  state.counters["ns_per_burst"] = seconds * 1e9 / iterations;
  state.counters["ns_per_item"] = seconds * 1e9 / iterations / burst;
  state.counters["bytes_per_item"] = E;
  state.counters["ring_bytes"] = static_cast<double>(Ops::RingBytes());
  state.SetItemsProcessed(state.iterations() * burst);
  Ops::Destroy(forward);
  Ops::Destroy(back);
}

// One-way: A produces bursts, B (the consumer) drains them. `pace` is the
// number of pause loops the consumer burns per dequeued burst: 0 is a
// consumer that keeps up (balanced; the producer retries a refused tail, so the
// time is the pipeline's throughput), and a positive value is a consumer slower
// than the producer (the producer outruns it: one attempt per burst, the
// refused tail is dropped and counted, so the time is the producer loop's cost
// when the queue is full).
template <class Ops>
void OneWay(benchmark::State &state) {
  constexpr size_t E = Ops::kItemBytes;
  const unsigned burst = static_cast<unsigned>(state.range(0));
  const int64_t pace = state.range(1);
  if (CPU_COUNT(&g_allowed) < 2) {
    state.SkipWithMessage("needs two CPUs: run under taskset -c A,B");
    return;
  }
  typename Ops::Ring *ring = Ops::Create();
  std::atomic<bool> stop{false}, ready{false};
  std::atomic<uint64_t> consumed{0}, nonempty_polls{0}, empty_polls{0};
  std::thread consumer([&] {
    PinToNth(1);
    alignas(64) std::byte buffer[kMaxBurst * kMaxItemBytes];
    SpinGuard guard;
    uint64_t count = 0, nonempty = 0, empty = 0;
    ready.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_relaxed)) {
      const unsigned m = Ops::Deq(ring, buffer, kMaxBurst);
      count += m;
      if (m != 0) {
        nonempty++;
        for (int64_t i = 0; i < pace; i++) {
          bess::arch::CpuRelax();
        }
      } else {
        empty++;
      }
      guard.Check();
    }
    consumed.store(count, std::memory_order_relaxed);
    nonempty_polls.store(nonempty, std::memory_order_relaxed);
    empty_polls.store(empty, std::memory_order_relaxed);
  });
  PinToNth(0);
  while (!ready.load(std::memory_order_acquire)) {
  }

  alignas(64) std::byte buffer[kMaxBurst * kMaxItemBytes];
  SpinGuard guard;
  uint64_t accepted = 0, refused = 0, seq = 0, enq_calls = 0;
  const auto start = std::chrono::steady_clock::now();
  for (auto _ : state) {
    Fill<E>(buffer, burst, seq);
    seq += burst;
    if (pace == 0) {
      for (unsigned sent = 0; sent < burst;) {
        const unsigned k = Ops::Enq(ring, buffer + sent * E, burst - sent);
        enq_calls++;
        sent += k;
        accepted += k;
        guard.Check();
      }
    } else {
      const unsigned k = Ops::Enq(ring, buffer, burst);
      accepted += k;
      refused += burst - k;
    }
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  stop.store(true, std::memory_order_relaxed);
  consumer.join();
  // What the consumer did not take is still in the ring; take it so that a
  // ring that owns its items (the channel) is destroyed empty.
  uint64_t left = 0;
  for (unsigned m; (m = Ops::Deq(ring, buffer, kMaxBurst)) != 0;) {
    left += m;
  }
  CHECK_EQ(accepted, consumed.load() + left) << "an item was lost or duplicated";

  const double iterations = static_cast<double>(state.iterations());
  state.counters["ns_per_burst"] = seconds * 1e9 / iterations;
  state.counters["ns_per_item"] = seconds * 1e9 / iterations / burst;
  state.counters["refused_fraction"] =
      static_cast<double>(refused) / (iterations * burst);
  state.counters["bytes_per_item"] = E;
  // The closed loop's phase (D-054 streaming item, H2): how the consumer's
  // batches and empty polls, and the producer's retries, settle for this ring.
  const double items = static_cast<double>(consumed.load());
  state.counters["consumer_items_per_nonempty_deq"] =
      nonempty_polls.load() ? items / static_cast<double>(nonempty_polls.load()) : 0;
  state.counters["consumer_empty_polls_per_item"] =
      items > 0 ? static_cast<double>(empty_polls.load()) / items : 0;
  state.counters["producer_enq_calls_per_burst"] =
      pace == 0 ? static_cast<double>(enq_calls) / iterations : 1;
  state.SetItemsProcessed(state.iterations() * burst);
  Ops::Destroy(ring);
}

// The software cost, with the cross-core transfers taken out: one thread
// enqueues a burst and dequeues it again. Everything the ring and the channel do
// per burst (index arithmetic, the copy, the closed check, the counters, nulling
// the handed-over pointers) is in this number and none of the coherence traffic
// is, so it is the difference between a bare ring and the channel. (The channel
// nulls the pointers on enqueue and the dequeue restores them, so the buffer is
// the same on every iteration.)
template <class Ops>
void Loopback(benchmark::State &state) {
  constexpr size_t E = Ops::kItemBytes;
  const unsigned burst = static_cast<unsigned>(state.range(0));
  typename Ops::Ring *ring = Ops::Create();
  alignas(64) std::byte buffer[kMaxBurst * kMaxItemBytes];
  Fill<E>(buffer, burst, 0);
  for (auto _ : state) {
    const unsigned in = Ops::Enq(ring, buffer, burst);
    const unsigned out = Ops::Deq(ring, buffer, burst);
    benchmark::DoNotOptimize(in + out);
    benchmark::ClobberMemory();
  }
  CHECK(Check<E>(buffer, burst, 0)) << "an item came back changed";
  state.counters["bytes_per_item"] = E;
  state.SetItemsProcessed(state.iterations() * burst);
  Ops::Destroy(ring);
}

// A full ring refuses: the cost of the refused call, single thread.
template <class Ops>
void RefuseFull(benchmark::State &state) {
  constexpr size_t E = Ops::kItemBytes;
  const unsigned burst = static_cast<unsigned>(state.range(0));
  typename Ops::Ring *ring = Ops::Create();
  alignas(64) std::byte buffer[kMaxBurst * kMaxItemBytes];
  Fill<E>(buffer, kMaxBurst, 0);
  uint64_t filled = 0;
  for (unsigned k; (k = Ops::Enq(ring, buffer, kMaxBurst)) != 0;) {
    filled += k;
    Fill<E>(buffer, kMaxBurst, filled);
  }
  unsigned accepted = 0;
  for (auto _ : state) {
    accepted += Ops::Enq(ring, buffer, burst);
    benchmark::ClobberMemory();
  }
  CHECK_EQ(0u, accepted) << "a full ring accepted an item";
  state.counters["ring_items"] = static_cast<double>(filled);
  state.SetItemsProcessed(state.iterations() * burst);
  for (unsigned m; (m = Ops::Deq(ring, buffer, kMaxBurst)) != 0;) {
  }
  Ops::Destroy(ring);
}

// A round trip in which both sides touch the packet, as a real slow path does,
// so that where the context lives has its true cost. A fake packet is an
// rte_mbuf-sized header (two cache lines) and a private area (one line):
//   fast path   writes line 0 (it processed the packet), writes the context,
//   service     reads line 0 and the context, writes a result into line 0,
//   fast path   reads the result.
// `kInPacket` puts the context in the packet's private area (a line of its
// own: the ring then carries only the pointer); otherwise the context is part
// of the ring item (Ops::kItemBytes = 8 + 8 * kCtxWords).
struct alignas(64) FakePacket {
  uint64_t line0[8];
  uint64_t line1[8];
  uint64_t priv[8];
};

template <class Ops, unsigned kCtxWords, bool kInPacket>
void PacketRoundTrip(benchmark::State &state) {
  constexpr size_t E = Ops::kItemBytes;
  static_assert(E == (kInPacket ? 8 : 8 + 8 * kCtxWords));
  const unsigned burst = static_cast<unsigned>(state.range(0));
  if (CPU_COUNT(&g_allowed) < 2) {
    state.SkipWithMessage("needs two CPUs: run under taskset -c A,B");
    return;
  }
  std::vector<FakePacket> packets(kMaxBurst);
  typename Ops::Ring *forward = Ops::Create();
  typename Ops::Ring *back = Ops::Create();
  std::atomic<bool> stop{false}, ready{false};
  std::thread service([&] {
    PinToNth(1);
    alignas(64) std::byte buffer[kMaxBurst * kMaxItemBytes];
    SpinGuard guard;
    ready.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_relaxed)) {
      const unsigned m = Ops::Deq(forward, buffer, kMaxBurst);
      for (unsigned i = 0; i < m; i++) {
        FakePacket *packet;
        std::memcpy(&packet, buffer + i * E, sizeof(packet));
        uint64_t sum = packet->line0[0];
        for (unsigned w = 0; w < kCtxWords; w++) {
          uint64_t word;
          if constexpr (kInPacket) {
            word = packet->priv[w];
          } else {
            std::memcpy(&word, buffer + i * E + 8 + 8 * w, 8);
          }
          sum += word;
        }
        packet->line0[1] = sum;
      }
      for (unsigned sent = 0; sent < m;) {
        sent += Ops::Enq(back, buffer + sent * E, m - sent);
      }
      guard.Check();
    }
  });
  PinToNth(0);
  while (!ready.load(std::memory_order_acquire)) {
  }

  alignas(64) std::byte buffer[kMaxBurst * kMaxItemBytes];
  SpinGuard guard;
  uint64_t seq = 0, observed = 0;
  auto one_round_trip = [&] {
    for (unsigned i = 0; i < burst; i++) {
      FakePacket *packet = &packets[i];
      packet->line0[0] = seq + i;
      std::memcpy(buffer + i * E, &packet, sizeof(packet));
      for (unsigned w = 0; w < kCtxWords; w++) {
        const uint64_t word = (seq + i) * 3 + w;
        if constexpr (kInPacket) {
          packet->priv[w] = word;
        } else {
          std::memcpy(buffer + i * E + 8 + 8 * w, &word, 8);
        }
      }
    }
    seq += burst;
    for (unsigned sent = 0; sent < burst;) {
      sent += Ops::Enq(forward, buffer + sent * E, burst - sent);
      guard.Check();
    }
    for (unsigned got = 0; got < burst;) {
      got += Ops::Deq(back, buffer + got * E, burst - got);
      guard.Check();
    }
    for (unsigned i = 0; i < burst; i++) {
      FakePacket *packet;
      std::memcpy(&packet, buffer + i * E, sizeof(packet));
      observed += packet->line0[1];
    }
  };

  // Untimed: the service's result must be the sum it was asked for.
  for (int i = 0; i < 20000; i++) {
    const uint64_t first = seq;
    one_round_trip();
    uint64_t expect = 0;
    for (unsigned j = 0; j < burst; j++) {
      expect += (first + j);
      for (unsigned w = 0; w < kCtxWords; w++) {
        expect += (first + j) * 3 + w;
      }
    }
    uint64_t got = 0;
    for (unsigned j = 0; j < burst; j++) {
      got += packets[j].line0[1];
    }
    CHECK_EQ(expect, got) << "the service saw a wrong packet or context";
  }

  const auto start = std::chrono::steady_clock::now();
  for (auto _ : state) {
    one_round_trip();
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  benchmark::DoNotOptimize(observed);
  stop.store(true, std::memory_order_relaxed);
  service.join();

  const double iterations = static_cast<double>(state.iterations());
  state.counters["ns_per_burst"] = seconds * 1e9 / iterations;
  state.counters["ns_per_item"] = seconds * 1e9 / iterations / burst;
  state.counters["ring_bytes_per_item"] = E;
  state.counters["ctx_words"] = kCtxWords;
  state.SetItemsProcessed(state.iterations() * burst);
  Ops::Destroy(forward);
  Ops::Destroy(back);
}

#define HANDOFF_ROUND_TRIP(NAME, TYPE)                             \
  void BM_RoundTrip_##NAME(benchmark::State &state) {              \
    RoundTrip<TYPE>(state);                                        \
  }                                                                \
  BENCHMARK(BM_RoundTrip_##NAME)                                   \
      ->Arg(1)->Arg(8)->Arg(16)->Arg(32)->UseRealTime()

// Balanced (pace 0) and producer-outruns-consumer (pace 400 pause loops, a few
// microseconds per dequeued burst), burst 1, 8, 32.
#define HANDOFF_ONE_WAY(NAME, TYPE)                                \
  void BM_OneWay_##NAME(benchmark::State &state) {                 \
    OneWay<TYPE>(state);                                           \
  }                                                                \
  BENCHMARK(BM_OneWay_##NAME)                                      \
      ->ArgsProduct({{1, 8, 32}, {0, 400}})->UseRealTime()

#define HANDOFF_LOOPBACK(NAME, TYPE)                               \
  void BM_Loopback_##NAME(benchmark::State &state) {               \
    Loopback<TYPE>(state);                                         \
  }                                                                \
  BENCHMARK(BM_Loopback_##NAME)->Arg(1)->Arg(8)->Arg(32)

#define HANDOFF_REFUSE(NAME, TYPE)                                 \
  void BM_RefuseFull_##NAME(benchmark::State &state) {             \
    RefuseFull<TYPE>(state);                                       \
  }                                                                \
  BENCHMARK(BM_RefuseFull_##NAME)->Arg(1)->Arg(8)->Arg(32)

using Elem8 = ElemRing<8, Mode::kSpSc>;
using Elem16 = ElemRing<16, Mode::kSpSc>;
using Elem24 = ElemRing<24, Mode::kSpSc>;
using Elem32 = ElemRing<32, Mode::kSpSc>;
using Elem64 = ElemRing<64, Mode::kSpSc>;
using Elem16Generic = ElemRing<16, Mode::kGeneric>;
using Elem16Mpsc = ElemRing<16, Mode::kMpSc>;
using Elem16Mpmc = ElemRing<16, Mode::kMpMc>;
using Elem16Dpdk = ElemRing<16, Mode::kSpSc, 1>;
using Elem16Dpdk128 = ElemRing<16, Mode::kSpSc, 2>;
using Cached16 = CachedSpsc<16>;

namespace dp = bess::dataplane;
struct Ctx8 {
  uint64_t w[1];
};
struct Ctx24 {
  uint64_t w[3];
};
using ChanSpsc0 = ChannelOps<dp::NoContext, dp::HandoffTopology::kSpSc>;
using ChanSpsc8 = ChannelOps<Ctx8, dp::HandoffTopology::kSpSc>;
using ChanSpsc24 = ChannelOps<Ctx24, dp::HandoffTopology::kSpSc>;
using ChanMpsc8 = ChannelOps<Ctx8, dp::HandoffTopology::kMpSc>;
using ChanMpmc8 = ChannelOps<Ctx8, dp::HandoffTopology::kMpMc>;
using ChanSingle8 = ChannelSingleOps<Ctx8, dp::HandoffTopology::kSpSc>;
HANDOFF_ROUND_TRIP(channel_spsc_noctx, ChanSpsc0);
HANDOFF_ROUND_TRIP(channel_spsc_ctx8, ChanSpsc8);
HANDOFF_ROUND_TRIP(channel_spsc_ctx24, ChanSpsc24);
HANDOFF_ROUND_TRIP(channel_mpsc_ctx8, ChanMpsc8);
HANDOFF_ROUND_TRIP(channel_mpmc_ctx8, ChanMpmc8);
HANDOFF_ROUND_TRIP(channel_single_ctx8, ChanSingle8);
HANDOFF_ONE_WAY(channel_spsc_ctx8, ChanSpsc8);
HANDOFF_ONE_WAY(channel_spsc_noctx, ChanSpsc0);
HANDOFF_ONE_WAY(channel_mpsc_ctx8, ChanMpsc8);
HANDOFF_ONE_WAY(channel_single_ctx8, ChanSingle8);
HANDOFF_REFUSE(channel_spsc_ctx8, ChanSpsc8);
HANDOFF_REFUSE(channel_mpsc_ctx8, ChanMpsc8);

HANDOFF_LOOPBACK(ptr_ring, PtrRing);
HANDOFF_LOOPBACK(elem8, Elem8);
HANDOFF_LOOPBACK(elem16, Elem16);
HANDOFF_LOOPBACK(elem16_generic, Elem16Generic);
HANDOFF_LOOPBACK(elem16_mpsc, Elem16Mpsc);
HANDOFF_LOOPBACK(elem16_mpmc, Elem16Mpmc);
HANDOFF_LOOPBACK(cached_spsc16, Cached16);
HANDOFF_LOOPBACK(channel_spsc_noctx, ChanSpsc0);
HANDOFF_LOOPBACK(channel_spsc_ctx8, ChanSpsc8);
HANDOFF_LOOPBACK(channel_mpsc_ctx8, ChanMpsc8);
HANDOFF_LOOPBACK(channel_mpmc_ctx8, ChanMpmc8);
HANDOFF_LOOPBACK(channel_single_ctx8, ChanSingle8);

HANDOFF_ROUND_TRIP(ptr_ring, PtrRing);
HANDOFF_ROUND_TRIP(elem8, Elem8);
HANDOFF_ROUND_TRIP(elem16, Elem16);
HANDOFF_ROUND_TRIP(elem24, Elem24);
HANDOFF_ROUND_TRIP(elem32, Elem32);
HANDOFF_ROUND_TRIP(elem64, Elem64);
HANDOFF_ROUND_TRIP(elem16_generic, Elem16Generic);
HANDOFF_ROUND_TRIP(elem16_mpsc, Elem16Mpsc);
HANDOFF_ROUND_TRIP(elem16_mpmc, Elem16Mpmc);
HANDOFF_ROUND_TRIP(cached_spsc16, Cached16);

HANDOFF_ONE_WAY(ptr_ring, PtrRing);
HANDOFF_ONE_WAY(elem16, Elem16);
HANDOFF_ONE_WAY(elem16_mpsc, Elem16Mpsc);
HANDOFF_ONE_WAY(cached_spsc16, Cached16);
HANDOFF_ONE_WAY(elem16_dpdkheap, Elem16Dpdk);
HANDOFF_ONE_WAY(elem16_dpdkheap128, Elem16Dpdk128);

HANDOFF_REFUSE(ptr_ring, PtrRing);
HANDOFF_REFUSE(elem16, Elem16);
HANDOFF_REFUSE(elem16_mpsc, Elem16Mpsc);
HANDOFF_REFUSE(cached_spsc16, Cached16);

// Where the context lives, with both sides touching the packet. Item sizes: the
// ring item is the pointer alone when the context is in the packet.
#define HANDOFF_PACKET_ROUND_TRIP(NAME, TYPE, WORDS, IN_PACKET)    \
  void BM_PacketRoundTrip_##NAME(benchmark::State &state) {        \
    PacketRoundTrip<TYPE, WORDS, IN_PACKET>(state);                \
  }                                                                \
  BENCHMARK(BM_PacketRoundTrip_##NAME)                             \
      ->Arg(1)->Arg(8)->Arg(32)->UseRealTime()

HANDOFF_PACKET_ROUND_TRIP(no_ctx, Elem8, 0, false);
HANDOFF_PACKET_ROUND_TRIP(ctx8_in_ring, Elem16, 1, false);
HANDOFF_PACKET_ROUND_TRIP(ctx8_in_packet, PtrRing, 1, true);
HANDOFF_PACKET_ROUND_TRIP(ctx24_in_ring, Elem32, 3, false);
HANDOFF_PACKET_ROUND_TRIP(ctx24_in_packet, PtrRing, 3, true);
HANDOFF_PACKET_ROUND_TRIP(channel_noctx, ChanSpsc0, 0, false);
HANDOFF_PACKET_ROUND_TRIP(channel_ctx8, ChanSpsc8, 1, false);
HANDOFF_PACKET_ROUND_TRIP(channel_ctx24, ChanSpsc24, 3, false);

}  // namespace
