// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_HANDOFF_H_
#define BESS_DATAPLANE_HANDOFF_H_

#include <rte_ring.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include <glog/logging.h>

#include "packet_handle.h"

// Ownership transfer of packets between threads (roadmap M11, Decision D-054;
// guide in docs/handoff.md). Experimental API.
//
// A handoff moves packets, each with a small typed context, from a producer
// (a worker, say, whose packets missed a policy) to a consumer (a service
// worker, a slow path, an accelerator thread) that does something the producer
// will not do in its packet loop. The channel knows nothing about why: no miss
// reason, policy or protocol is part of it. The application defines `Context`
// and what it means; a reply travels the other way on a second channel, with
// whatever it needs to resume (a `ContinuationHandle`, continuation.h, inside
// its context).
//
// Mechanism. A DPDK `rte_ring` of fixed-size elements (`rte_ring_elem`), each
// element a `PuntItem<Context>`: the packet and its context in one ring slot.
// No new queue algorithm: the ring is the one the Queue module uses, with the
// explicit sync-mode calls (never the flag-dispatching wrappers) and a slot
// that carries the context too. Why that, and not a ring of bare pointers with
// the context beside it, in the packet, or a purpose-built queue, is measured in
// D-054.
//
// Ownership. A packet belongs to exactly one side at every instant:
//
//   producer  --TryPuntBurst-->  channel  --Dequeue-->  consumer
//
// The producer hands over a burst as an array of items. The call returns how
// many it took, N, and the first N items are now the channel's: **their
// `packet` fields are set to nullptr** (moved-from, as `std::move` of an owning
// pointer would leave them), so a producer that frees or reuses a handed-off
// packet finds a null, not a dangling pointer. The items from N on were not
// taken: they are still the producer's, untouched, and the result says why
// (full, or closed). The caller retries, uses another path, or drops them; the
// channel never frees a refused packet and never keeps a reference to it. On
// `Dequeue` the packet belongs to the consumer from the moment it is written
// into the output array. A packet is delivered at most once, and a packet that
// is neither delivered nor refused is not possible: the channel's own teardown
// frees what is left (below).
//
// Back-pressure. A full channel refuses and says so; nothing blocks and
// nothing grows (the capacity is fixed at creation and so is the memory). Every
// refused item is counted (`refused_full`, `refused_closed`). The producer's
// choice is the policy: fail/drop (free the refused tail and count it) or
// retry/alternate path. There is no third, hidden one.
//
// Topology. Bound at creation, as a template argument, so the enqueue and
// dequeue are the ring's explicit single- or multi-producer entry points with
// no run-time inspection: `kSpSc` (one producer thread, one consumer thread),
// `kMpSc` (many producers, one consumer) or `kMpMc`. "One thread" means one at a
// time: a role may move between threads if the hand-over synchronises.
//
// Lifecycle (the ordering that makes teardown safe):
//   1. `Close()`: from now on `TryPuntBurst` refuses everything with
//      `kClosed`, so producers fall back at once. Idempotent, any thread.
//   2. Stop every producer and consumer thread: a quiescent point (workers
//      paused, joined, or past an RCU grace period). The channel cannot wait
//      for threads that are inside a call; destroying it under one is a use
//      after free, as for any object.
//   3. `Drain(fn)`: take what is left, one item at a time, and do with it what
//      the application wants (forward to the slow path's own queue, count,
//      free). Optional.
//   4. Destroy the channel. Whatever is still queued is freed (the packets
//      with `rte_pktmbuf_free`) and counted in `discarded`: nothing leaks,
//      nothing is delivered twice.
// A consumer that crashes or never drains is steps 1-4 run by the owner; until
// then its producers see `kFull` and drop, so memory stays bounded.
//
// Accounting. When no call is in progress,
//   enqueued == dequeued + discarded + occupancy
// and every item a producer offered was either enqueued, refused (full or
// closed) or is still the producer's.
//
// Memory. One allocation in `Create`, from the allocator in the config (DPDK's
// heap on the requested NUMA node by default): this object, then the ring.
// Afterwards nothing allocates. `placement()` reports the node asked for and the
// node the memory actually landed on, so a cross-NUMA channel is visible.

namespace bess::dataplane {

// The context of a handoff that carries none: the slot is the pointer alone.
struct NoContext {};

// What travels: a packet and its context, in one ring slot.
template <typename Context = NoContext>
struct PuntItem {
  PacketHandle packet = nullptr;
  [[no_unique_address]] Context context{};
};

enum class HandoffTopology : uint8_t {
  kSpSc,  // one producer, one consumer
  kMpSc,  // many producers, one consumer
  kMpMc,  // many producers, many consumers
};

// Why items were not taken.
enum class HandoffError : uint8_t {
  kFull,    // the channel holds `capacity` items
  kClosed,  // Close() was called
};

inline const char *ToString(HandoffError error) noexcept {
  switch (error) {
    case HandoffError::kFull:
      return "handoff channel full";
    case HandoffError::kClosed:
      return "handoff channel closed";
  }
  return "unknown handoff error";
}

// What TryPuntBurst did: the first `accepted` items of the burst belong to the
// channel now; if `refused` has a value, the rest were not taken and are still
// the caller's.
struct PuntResult {
  size_t accepted = 0;
  std::optional<HandoffError> refused;

  // Everything offered was taken.
  explicit operator bool() const noexcept { return !refused.has_value(); }
};

enum class HandoffCreateError : uint8_t {
  kInvalidCapacity,  // zero, or more than the ring can name
  kOutOfMemory,      // the allocator refused
  kRingInitFailed,   // the ring rejected its parameters
};

inline const char *ToString(HandoffCreateError error) noexcept {
  switch (error) {
    case HandoffCreateError::kInvalidCapacity:
      return "invalid handoff channel capacity";
    case HandoffCreateError::kOutOfMemory:
      return "out of memory creating the handoff channel";
    case HandoffCreateError::kRingInitFailed:
      return "the handoff ring could not be initialised";
  }
  return "unknown handoff create error";
}

// Where a channel gets its memory. Creation is the only place it allocates;
// this is the one seam for that (a NUMA-aware or hugepage allocator, a test
// that refuses the allocation).
struct HandoffAllocator {
  // `bytes` aligned to `align`, on NUMA node `socket` (SOCKET_ID_ANY: the
  // allocator's choice), or nullptr. `*placed` receives the node the memory is
  // actually on, or -1 if the allocator cannot tell.
  void *(*allocate)(size_t bytes, size_t align, int socket,
                    int *placed) noexcept;
  void (*deallocate)(void *block) noexcept;
};

// DPDK's heap (`rte_malloc_socket`, the dataplane's memory, D-029). Falls back
// to any node when the requested one is out of memory, and says so through
// `placed`.
const HandoffAllocator &DpdkHandoffAllocator() noexcept;

struct HandoffConfig {
  // Items the channel can hold. Exact: the ring is sized to the next power of
  // two above it, but refuses at `capacity`.
  size_t capacity = 1024;
  // NUMA node for the channel's memory. Place it where the consumer (or the
  // busier side) runs; a channel whose ends sit on different nodes pays the
  // interconnect on every item and `placement()` is how to see it.
  int socket = SOCKET_ID_ANY;
  // nullptr: DpdkHandoffAllocator().
  const HandoffAllocator *allocator = nullptr;
};

struct HandoffStats {
  uint64_t enqueued = 0;        // items taken from producers
  uint64_t dequeued = 0;        // items given to consumers
  uint64_t refused_full = 0;    // items refused because the channel was full
  uint64_t refused_closed = 0;  // items refused because it was closed
  uint64_t discarded = 0;       // items removed by Drain or the destructor
  size_t occupancy = 0;         // items queued now
  // The identity in the class comment holds when no call is in progress; a
  // snapshot taken while threads run can show a dequeue before its enqueue.
};

struct HandoffPlacement {
  int requested_socket = -1;  // what the config asked for (SOCKET_ID_ANY: -1)
  int placed_socket = -1;     // where the memory is; -1: unknown
  size_t bytes = 0;           // the whole channel: object and ring
  // True when a specific node was asked for and the memory is elsewhere or
  // its node is unknown.
  bool misplaced() const noexcept {
    return requested_socket >= 0 && placed_socket != requested_socket;
  }
};

namespace detail {

// The size of an `rte_ring` for `count` (a power of two) elements of `esize`
// bytes, or negative. DPDK 25.11's rte_ring_elem.h lacks its own `extern "C"`,
// so rte_ring_get_memsize_elem has C++ linkage there and cannot be linked from
// a header; handoff.cc reaches the C function.
ssize_t RingMemsize(unsigned esize, unsigned count) noexcept;

// Frees a packet (rte_pktmbuf_free).
void FreePacket(PacketHandle packet) noexcept;

// A counter whose writers are a single thread (a plain load and store: no
// read-modify-write on the hot path) or many (one relaxed fetch_add).
template <bool kShared>
class Counter {
 public:
  void Add(uint64_t n) noexcept {
    if constexpr (kShared) {
      value_.fetch_add(n, std::memory_order_relaxed);
    } else {
      value_.store(value_.load(std::memory_order_relaxed) + n,
                   std::memory_order_relaxed);
    }
  }
  uint64_t Load() const noexcept {
    return value_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<uint64_t> value_{0};
};

}  // namespace detail

template <typename Context = NoContext,
          HandoffTopology Topology = HandoffTopology::kSpSc>
class HandoffChannel {
 public:
  using context_type = Context;
  using Item = PuntItem<Context>;

  // The largest item, and with it the largest context: one cache line.
  static constexpr size_t kMaxItemBytes = 64;
  // The most items a channel can hold (the ring's 31-bit index space, less the
  // slot that EXACT_SZ rounds up with).
  static constexpr size_t kMaxCapacity = (size_t{1} << 30) - 1;
  static constexpr size_t kItemBytes = sizeof(Item);
  static constexpr HandoffTopology kTopology = Topology;

  static_assert(std::is_trivially_copyable_v<Item>,
                "a handoff context is copied through the ring: it must be "
                "trivially copyable (carry a ContinuationHandle or another "
                "generation-safe id for anything larger or non-trivial)");
  static_assert(std::is_trivially_destructible_v<Item>);
  static_assert(kItemBytes <= kMaxItemBytes,
                "a handoff context is bounded: the item (packet + context) "
                "must fit one cache line");
  static_assert(kItemBytes % 4 == 0, "ring elements are multiples of 4 bytes");

  struct Deleter {
    void operator()(HandoffChannel *channel) const noexcept {
      auto deallocate = channel->deallocate_;  // read before the object dies
      channel->~HandoffChannel();
      deallocate(channel);
    }
  };
  using Ptr = std::unique_ptr<HandoffChannel, Deleter>;

  // A new, empty, open channel. Its memory is one block from the config's
  // allocator; every failure frees what it had taken.
  static std::expected<Ptr, HandoffCreateError> Create(
      const HandoffConfig &config = {}) noexcept {
    if (config.capacity == 0 || config.capacity > kMaxCapacity) {
      return std::unexpected(HandoffCreateError::kInvalidCapacity);
    }
    const HandoffAllocator &allocator = config.allocator != nullptr
                                            ? *config.allocator
                                            : DpdkHandoffAllocator();
    const size_t slots = std::bit_ceil(config.capacity + 1);
    const ssize_t ring_bytes =
        detail::RingMemsize(static_cast<unsigned>(kItemBytes),
                            static_cast<unsigned>(slots));
    if (ring_bytes < 0) {
      return std::unexpected(HandoffCreateError::kInvalidCapacity);
    }
    const size_t total = RingOffset() + static_cast<size_t>(ring_bytes);
    int placed = -1;
    void *block = allocator.allocate(total, BlockAlign(), config.socket, &placed);
    if (block == nullptr) {
      return std::unexpected(HandoffCreateError::kOutOfMemory);
    }
    auto *ring = reinterpret_cast<rte_ring *>(static_cast<std::byte *>(block) +
                                              RingOffset());
    if (rte_ring_init(ring, "bess_handoff",
                      static_cast<unsigned>(config.capacity),
                      RING_F_EXACT_SZ | kRingFlags) != 0) {
      allocator.deallocate(block);
      return std::unexpected(HandoffCreateError::kRingInitFailed);
    }
    return Ptr(new (block) HandoffChannel(ring, allocator.deallocate,
                                          config.socket, placed, total));
  }

  HandoffChannel(const HandoffChannel &) = delete;
  HandoffChannel &operator=(const HandoffChannel &) = delete;

  // Frees whatever is still queued (step 4 of the lifecycle). Callers must
  // have stopped every producer and consumer.
  ~HandoffChannel() {
    Drain([](Item &item) noexcept { detail::FreePacket(item.packet); });
  }

  // -- producer side ------------------------------------------------------------

  // Hands over as many of `items` as fit, in order, and returns how many. The
  // first `accepted` items now belong to the channel and have their `packet`
  // set to nullptr; the rest are untouched and still the caller's (see
  // "Ownership"). Never blocks.
  [[nodiscard]] PuntResult TryPuntBurst(std::span<Item> items) noexcept {
    const size_t offered = items.size();
    if (offered == 0) {
      return {};
    }
    if (closed_.load(std::memory_order_acquire)) [[unlikely]] {
      refused_closed_.Add(offered);
      return {0, HandoffError::kClosed};
    }
    const unsigned n = static_cast<unsigned>(
        std::min<size_t>(offered, std::numeric_limits<unsigned>::max()));
    unsigned accepted;
    if constexpr (Topology == HandoffTopology::kSpSc) {
      accepted = rte_ring_sp_enqueue_burst_elem(ring_, items.data(), kItemBytes,
                                                n, nullptr);
    } else {
      accepted = rte_ring_mp_enqueue_burst_elem(ring_, items.data(), kItemBytes,
                                                n, nullptr);
    }
    for (unsigned i = 0; i < accepted; i++) {
      items[i].packet = nullptr;
    }
    if (accepted != 0) {
      enqueued_.Add(accepted);
    }
    if (accepted == offered) [[likely]] {
      return {accepted, std::nullopt};
    }
    refused_full_.Add(offered - accepted);
    return {accepted, HandoffError::kFull};
  }

  // One packet. On success `packet` is set to nullptr (it belongs to the
  // channel); on failure it is untouched and the error says why.
  std::expected<void, HandoffError> TryPunt(PacketHandle &packet,
                                            const Context &context) noexcept {
    Item item{packet, context};
    const PuntResult result = TryPuntBurst(std::span<Item>(&item, 1));
    if (result.accepted == 1) {
      packet = nullptr;
      return {};
    }
    return std::unexpected(*result.refused);
  }

  // -- consumer side --------------------------------------------------------------

  // Takes up to `out.size()` items, in the order they were handed over (per
  // producer, for a multi-producer channel), and returns how many. Each
  // returned packet belongs to the caller. Never blocks; 0 means empty.
  size_t Dequeue(std::span<Item> out) noexcept {
    const size_t taken = DequeueRaw(out);
    if (taken != 0) {
      dequeued_.Add(taken);
    }
    return taken;
  }

  // -- lifecycle ---------------------------------------------------------------------

  // Step 1 of teardown: refuses every later punt with kClosed. Idempotent; any
  // thread.
  void Close() noexcept { closed_.store(true, std::memory_order_release); }
  bool closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
  }

  // Step 3 of teardown: takes every queued item, one at a time, as the
  // consumer would, and passes it to `dispose(Item &)`, which owns its packet
  // from then on. Counted in `discarded`, not `dequeued`. Call it when the
  // producers have stopped (anything punted during the call may or may not be
  // seen) and from the consumer's role (the consumer thread, or after it
  // stopped). Returns the number of items removed.
  template <typename Dispose>
    requires std::invocable<Dispose &, Item &>
  size_t Drain(Dispose &&dispose) noexcept(
      std::is_nothrow_invocable_v<Dispose &, Item &>) {
    size_t removed = 0;
    Item items[kDrainBurst];
    for (;;) {
      const size_t n = DequeueRaw(std::span<Item>(items));
      if (n == 0) {
        break;
      }
      discarded_.Add(n);
      for (size_t i = 0; i < n; i++) {
        dispose(items[i]);
      }
      removed += n;
    }
    return removed;
  }

  // -- state (any thread) ------------------------------------------------------------

  size_t capacity() const noexcept { return ring_->capacity; }
  // Items queued now; exact only when no call is in progress.
  size_t size() const noexcept { return rte_ring_count(ring_); }
  size_t free_space() const noexcept { return rte_ring_free_count(ring_); }
  bool empty() const noexcept { return rte_ring_empty(ring_) != 0; }
  bool full() const noexcept { return rte_ring_full(ring_) != 0; }

  HandoffStats stats() const noexcept {
    HandoffStats s;
    s.enqueued = enqueued_.Load();
    s.dequeued = dequeued_.Load();
    s.refused_full = refused_full_.Load();
    s.refused_closed = refused_closed_.Load();
    s.discarded = discarded_.Load();
    s.occupancy = size();
    return s;
  }

  HandoffPlacement placement() const noexcept {
    return {requested_socket_, placed_socket_, block_bytes_};
  }
  // Bytes of the whole channel; per item at full occupancy it is
  // memory_bytes() / capacity().
  size_t memory_bytes() const noexcept { return block_bytes_; }

  // Testing only: moves the ring's indices to `start` (the channel must be
  // empty), to run the 32-bit index wrap-around without 2^32 items.
  void SetIndicesForTesting(uint32_t start) noexcept {
    CHECK(empty());
    ring_->prod.head = start;
    ring_->prod.tail = start;
    ring_->cons.head = start;
    ring_->cons.tail = start;
  }

 private:
  static constexpr bool kProducerShared = Topology != HandoffTopology::kSpSc;
  static constexpr bool kConsumerShared = Topology == HandoffTopology::kMpMc;
  static constexpr unsigned kRingFlags =
      Topology == HandoffTopology::kSpSc   ? (RING_F_SP_ENQ | RING_F_SC_DEQ)
      : Topology == HandoffTopology::kMpSc ? RING_F_SC_DEQ
                                           : 0u;
  static constexpr size_t kDrainBurst = 32;
  // The unit that separates data written by different threads: two cache lines.
  static constexpr size_t kGroupAlign = 2 * RTE_CACHE_LINE_SIZE;

  // The block starts on the stricter of the object's and the ring's alignment,
  // and the ring starts on its own alignment after the object. (Functions, not
  // constants: the class is incomplete where a static member is initialised.)
  static constexpr size_t BlockAlign() noexcept {
    return std::max(alignof(HandoffChannel), alignof(rte_ring));
  }
  static constexpr size_t RingOffset() noexcept {
    return (sizeof(HandoffChannel) + alignof(rte_ring) - 1) /
           alignof(rte_ring) * alignof(rte_ring);
  }

  HandoffChannel(rte_ring *ring, void (*deallocate)(void *) noexcept,
                 int requested_socket, int placed_socket, size_t block_bytes)
      : ring_(ring),
        deallocate_(deallocate),
        requested_socket_(requested_socket),
        placed_socket_(placed_socket),
        block_bytes_(block_bytes) {}

  size_t DequeueRaw(std::span<Item> out) noexcept {
    if (out.empty()) {
      return 0;
    }
    const unsigned n = static_cast<unsigned>(
        std::min<size_t>(out.size(), std::numeric_limits<unsigned>::max()));
    if constexpr (Topology == HandoffTopology::kMpMc) {
      return rte_ring_mc_dequeue_burst_elem(ring_, out.data(), kItemBytes, n,
                                            nullptr);
    } else {
      return rte_ring_sc_dequeue_burst_elem(ring_, out.data(), kItemBytes, n,
                                            nullptr);
    }
  }

  // Each group sits on its own pair of cache lines (128 bytes): Intel's
  // adjacent-line prefetcher fetches lines in aligned pairs, so a line written by
  // one thread next to a line read by another is invalidated under the reader
  // (measured in D-054: one-way throughput at burst 32 was 45% below the bare
  // ring with the groups 64 bytes apart).
  //
  // Read by every producer on every burst, written once by Close: a line pair
  // nobody invalidates in steady state.
  alignas(kGroupAlign) std::atomic<bool> closed_{false};
  rte_ring *const ring_;
  void (*const deallocate_)(void *) noexcept;
  const int requested_socket_;
  const int placed_socket_;
  const size_t block_bytes_;
  // Written by the producer side only.
  alignas(kGroupAlign) detail::Counter<kProducerShared> enqueued_;
  detail::Counter<kProducerShared> refused_full_;
  detail::Counter<kProducerShared> refused_closed_;
  // Written by the consumer side only.
  alignas(kGroupAlign) detail::Counter<kConsumerShared> dequeued_;
  detail::Counter<kConsumerShared> discarded_;
};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_HANDOFF_H_
