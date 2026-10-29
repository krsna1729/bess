// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_PACKET_STORE_H_
#define BESS_DATAPLANE_PACKET_STORE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "dataplane/resource.h"
#include "packet_handle.h"
#include "rcu/rcu_ptr.h"
#include "utils/dpdk_memory.h"

namespace bess {
namespace dataplane {

// One flow's own limits, as a `flows` entry gives them (D-129): an absent
// field leaves the store-wide number in force, and timeout_ns present as 0
// means this flow's packets never expire. The value of the flows resource;
// on the wire bess.pb.PacketStoreFlowValue, field presence kept.
struct PacketStoreLimits {
  std::optional<uint64_t> timeout_ns;
  std::optional<uint32_t> max_packets_per_flow;

  bool operator==(const PacketStoreLimits &) const = default;
};

// The admission shared by a flows resource and every limits map it makes
// (D-198). A retired map is freed after a grace period, possibly after the
// resource, so maps hold this and give bytes back through it; the resource
// detaches the owner's hook when it dies, and later frees give nothing back.
class PacketStoreMemory {
 public:
  void Grow(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hook_ != nullptr) {
      hook_->Grow(bytes, bytes);
    }
    held_.fetch_add(bytes, std::memory_order_relaxed);
  }
  void Shrink(size_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hook_ != nullptr) {
      hook_->Shrink(bytes, bytes);
    }
    held_.fetch_sub(bytes, std::memory_order_relaxed);
  }
  void Attach(utils::MemoryGrowth *hook) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    hook_ = hook;
  }
  size_t held() const noexcept { return held_.load(std::memory_order_relaxed); }

 private:
  std::mutex mutex_;
  utils::MemoryGrowth *hook_ = nullptr;  // guarded by mutex_
  std::atomic<size_t> held_{0};
};

// Admits each node before allocating it and gives it back when freed. A
// default allocator has no admission (tests, library users).
template <typename T>
class PacketStoreAllocator {
 public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  PacketStoreAllocator() = default;
  explicit PacketStoreAllocator(std::shared_ptr<PacketStoreMemory> memory) noexcept
      : memory_(std::move(memory)) {}
  template <typename U>
  PacketStoreAllocator(const PacketStoreAllocator<U> &o) noexcept
      : memory_(o.memory()) {}

  T *allocate(size_t n) {
    const size_t bytes = n * sizeof(T);
    if (memory_ != nullptr) {
      memory_->Grow(bytes);  // refusal throws before anything is allocated
    }
    try {
      return std::allocator<T>().allocate(n);
    } catch (...) {
      if (memory_ != nullptr) {
        memory_->Shrink(bytes);
      }
      throw;
    }
  }
  void deallocate(T *p, size_t n) noexcept {
    std::allocator<T>().deallocate(p, n);
    if (memory_ != nullptr) {
      memory_->Shrink(n * sizeof(T));
    }
  }
  const std::shared_ptr<PacketStoreMemory> &memory() const noexcept {
    return memory_;
  }
  template <typename U>
  bool operator==(const PacketStoreAllocator<U> &o) const noexcept {
    return memory_ == o.memory();
  }

 private:
  std::shared_ptr<PacketStoreMemory> memory_;
};

// The per-flow limits by flow id: one immutable table, published whole
// through an RcuPtr the store's owner holds and read once per run. With an
// admitting allocator it also admits its own header, so a clone costs its
// header and every node before any is allocated. Not movable or assignable:
// a table is built, published and freed.
class PacketStoreLimitsMap
    : public std::map<uint32_t, PacketStoreLimits, std::less<uint32_t>,
                      PacketStoreAllocator<std::pair<const uint32_t, PacketStoreLimits>>> {
  using Base = std::map<uint32_t, PacketStoreLimits, std::less<uint32_t>,
                        PacketStoreAllocator<std::pair<const uint32_t, PacketStoreLimits>>>;

 public:
  using Base::Base;
  PacketStoreLimitsMap() = default;
  explicit PacketStoreLimitsMap(const allocator_type &alloc) : Base(alloc) {
    Admit();
  }
  PacketStoreLimitsMap(const PacketStoreLimitsMap &o) : Base(o) { Admit(); }
  PacketStoreLimitsMap &operator=(const PacketStoreLimitsMap &) = delete;
  ~PacketStoreLimitsMap() {
    if (counted_) {
      get_allocator().memory()->Shrink(sizeof(*this));
    }
  }

 private:
  void Admit() {
    if (get_allocator().memory() != nullptr) {
      get_allocator().memory()->Grow(sizeof(*this));
      counted_ = true;
    }
  }
  bool counted_ = false;
};

struct PacketStoreConfig {
  uint32_t max_packets = 4096;           // packets held, all flows (> 0)
  uint32_t max_packets_per_flow = 64;    // a flow's default cap (> 0)
  uint64_t timeout_ns = 5'000'000'000ULL;  // default age limit; 0: no expiry
};

// Why the store frees a packet internally, rather than handing it to an
// eviction or release sink. An observer may use this to retire caller-owned
// state associated with the handle.
enum class PacketStoreDiscardReason : uint8_t { kExpired, kDropped, kCleared };

// A bounded, single-owner packet buffer (D-201): per-flow FIFOs of packet
// handles keyed by a uint32 flow id, with a store-wide bound, a per-flow cap
// and an age limit, either of which a flow's PacketStoreLimits may override.
// Extracted from the packet_store module, which is now an adapter over it
// (ingress ring, request ring, gates, counters); an appliance that owns its
// own threads calls it directly.
//
// Memory. Everything is allocated by Create and fixed for the store's life
// (Bytes(config), all glibc): a slot per packet (`max_packets` slots, linked
// per flow, a free list for the rest) and an open-addressing flow table of
// FlowCapacity(max_packets) entries, at most half full because a flow is
// held only while it holds a packet. Storing, releasing, dropping and expiry
// allocate nothing. The eviction and release sinks are template callables,
// inlined into the caller; only an installed discard observer is indirect.
//
// Threads. One owner: every member that changes the store must be called by
// one thread at a time (the module's task, or control while no worker runs).
// packets() and flows() are relaxed loads any thread may make.
class PacketStore {
 public:
  // Held flows one ExpireBounded call examines, independent of table sparsity.
  // A full rotation takes ceil(flows() / kExpireFlowsPerRun) calls.
  static constexpr size_t kExpireFlowsPerRun = 256;

  // A config with max_packets or max_packets_per_flow 0, or max_packets
  // above 2^30, is refused with the reason; null-free otherwise, or nullptr
  // in the expected when the arrays cannot be allocated.
  static std::expected<std::unique_ptr<PacketStore>, std::string> Create(
      const PacketStoreConfig &config);

  // What Create allocates (D-198), all of it from glibc.
  static size_t Bytes(const PacketStoreConfig &config) noexcept;
  static size_t GlibcBytes(const PacketStoreConfig &config) noexcept {
    return Bytes(config);
  }
  // Flow-table entries for a bound: a power of two at least twice it.
  static size_t FlowCapacity(uint32_t max_packets) noexcept;
  size_t bytes() const noexcept { return Bytes(config_); }

  PacketStore(const PacketStore &) = delete;
  PacketStore &operator=(const PacketStore &) = delete;
  // Frees every packet it still holds.
  ~PacketStore();

  const PacketStoreConfig &config() const noexcept { return config_; }
  size_t packets() const noexcept {
    return total_.load(std::memory_order_relaxed);
  }
  size_t flows() const noexcept { return held_.load(std::memory_order_relaxed); }

  // Called on the owner thread, exactly once before each internal free
  // (expiry, Drop, Clear, including the destructor's Clear). Never called for
  // a handle transferred to an eviction or release sink. The observer only
  // retires associated caller state: it must not free the packet, throw,
  // reenter the store or change the observer. It may inspect the still-live
  // packet; counts are not yet committed. No observer is installed by default.
  //
  // `opaque` must outlive the store, or the owner must Clear while it is alive
  // and then SetDiscardObserver(nullptr, nullptr) before destroying it.
  using DiscardObserver =
      void (*)(void *, PacketHandle, PacketStoreDiscardReason);
  void SetDiscardObserver(void *opaque, DiscardObserver observer) noexcept {
    discard_opaque_ = opaque;
    discard_observer_ = observer;
  }

  // The flow's timeout and cap: its entry in `limits` where the entry gives
  // them, the config otherwise. A cap of 0 in an entry is the config's cap,
  // not "hold nothing"; a timeout of 0 is no expiry.
  std::pair<uint64_t, uint32_t> LimitsFor(const PacketStoreLimitsMap &limits,
                                          uint32_t flow_id) const {
    auto it = limits.find(flow_id);
    if (it == limits.end()) {
      return {config_.timeout_ns, config_.max_packets_per_flow};
    }
    const PacketStoreLimits &entry = it->second;
    const uint64_t timeout = entry.timeout_ns.value_or(config_.timeout_ns);
    uint32_t cap = config_.max_packets_per_flow;
    if (entry.max_packets_per_flow.value_or(0) != 0) {
      cap = *entry.max_packets_per_flow;
    }
    return {timeout, cap};
  }

  // Stores `n` packets, packet i under flow_ids[i], stamped `now_ns`. The
  // store takes every handle: a packet it holds, or one it gives to
  // `evict(PacketHandle)` - the oldest of a flow at its cap (as many as it
  // takes to bring the flow under a cap an upsert lowered), or the arriving
  // packet when the store holds max_packets. When the store is full on entry
  // it first frees what expired (ExpireAll). The caller owns what `evict`
  // receives.
  template <typename Evict>
  void Store(const PacketHandle *handles, const uint32_t *flow_ids, size_t n,
             const PacketStoreLimitsMap &limits, uint64_t now_ns,
             Evict &&evict);

  // Hands every packet flow `flow_id` holds to `sink(PacketHandle)`, oldest
  // first, and forgets the flow. Returns how many; 0 for a flow it does not
  // hold. The caller owns what `sink` receives.
  template <typename Sink>
  size_t Release(uint32_t flow_id, Sink &&sink);

  // Frees every packet flow `flow_id` holds and forgets the flow. Returns how
  // many it freed.
  size_t Drop(uint32_t flow_id);

  // Frees every packet the store holds. Returns how many.
  size_t Clear();

  // Frees, in every flow, the packets older than the flow's timeout.
  void ExpireAll(const PacketStoreLimitsMap &limits, uint64_t now_ns);

  // As ExpireAll, at a bounded cost: examines at most kExpireFlowsPerRun held
  // flows from where the last call stopped (a tick's sweep).
  void ExpireBounded(const PacketStoreLimitsMap &limits, uint64_t now_ns);

 private:
  static constexpr uint32_t kNone = UINT32_MAX;

  // One stored packet; `next` links a flow's FIFO, or the free list.
  struct Slot {
    PacketHandle handle;
    uint64_t timestamp_ns;
    uint32_t next;
  };
  // One held flow; count 0 marks an empty table entry (a flow is forgotten
  // when it holds nothing).
  struct Flow {
    uint32_t id;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t next_active = kNone;
    uint32_t previous_active = kNone;
  };

  PacketStore(const PacketStoreConfig &config, std::unique_ptr<Slot[]> slots,
              std::unique_ptr<Flow[]> table, size_t capacity) noexcept;

  size_t Home(uint32_t id) const noexcept {
    return static_cast<size_t>((uint64_t{id} * 0x9E3779B97F4A7C15ULL) >>
                               shift_);
  }
  // The table index of `id`, or the empty entry where it would go.
  size_t Probe(uint32_t id) const noexcept {
    size_t i = Home(id);
    while (table_[i].count != 0 && table_[i].id != id) {
      i = (i + 1) & mask_;
    }
    return i;
  }
  // Forgets the entry at `i` (its flow holds nothing now) and
  // shifts the probe chain after it back (linear probing, no tombstones).
  void EraseAt(size_t i) noexcept;

  // Unlinks the flow's oldest slot onto the free list; returns its handle.
  PacketHandle PopFront(Flow &flow) noexcept {
    const uint32_t s = flow.head;
    flow.head = slots_[s].next;
    flow.count--;
    slots_[s].next = free_;
    free_ = s;
    return slots_[s].handle;
  }
  // Frees the flow's packets older than `timeout`; true if it emptied.
  bool ExpireFlow(Flow &flow, uint64_t timeout, uint64_t now_ns, size_t &total);

  void NotifyDiscard(PacketHandle handle, PacketStoreDiscardReason reason) noexcept {
    if (discard_observer_ != nullptr) {
      discard_observer_(discard_opaque_, handle, reason);
    }
  }

  PacketStoreConfig config_;
  std::unique_ptr<Slot[]> slots_;
  std::unique_ptr<Flow[]> table_;
  size_t mask_;
  unsigned shift_;
  uint32_t free_ = kNone;
  uint32_t cursor_ = kNone;  // next held flow for ExpireBounded
  void *discard_opaque_ = nullptr;
  DiscardObserver discard_observer_ = nullptr;

  // One writer (the owner); relaxed atomics so control may read them.
  std::atomic<size_t> total_{0};
  std::atomic<size_t> held_{0};
};

template <typename Evict>
void PacketStore::Store(const PacketHandle *handles, const uint32_t *flow_ids,
                        size_t n, const PacketStoreLimitsMap &limits,
                        uint64_t now_ns, Evict &&evict) {
  if (total_.load(std::memory_order_relaxed) >= config_.max_packets) {
    ExpireAll(limits, now_ns);
  }
  // One writer, so the counts are kept in registers and stored once a call.
  size_t total = total_.load(std::memory_order_relaxed);
  size_t held = held_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < n; i++) {
    const uint32_t id = flow_ids[i];
    const size_t at = Probe(id);
    Flow &flow = table_[at];
    const bool fresh = flow.count == 0;  // empty entries are never held
    if (fresh) {
      flow = Flow{id, kNone, kNone, 0};
    }
    const uint32_t cap = LimitsFor(limits, id).second;
    // A loop, not one eviction: an upsert can lower the cap below what the
    // flow already holds, and one arrival brings it down.
    while (flow.count >= cap) {
      total--;
      evict(PopFront(flow));
    }
    if (total >= config_.max_packets) {
      // The store is full: the arriving packet is the one evicted. A flow
      // the cap loop emptied is forgotten now (a fresh one was never held),
      // so the table never holds an empty flow and stays at most half full.
      if (flow.count == 0 && !fresh) {
        held--;
        EraseAt(at);
      }
      evict(handles[i]);
      continue;
    }
    // total < max_packets, so the free list has a slot.
    const uint32_t s = free_;
    free_ = slots_[s].next;
    slots_[s] = Slot{handles[i], now_ns, kNone};
    if (flow.count == 0) {
      if (fresh) {
        if (cursor_ == kNone) {
          flow.next_active = flow.previous_active = static_cast<uint32_t>(at);
          cursor_ = static_cast<uint32_t>(at);
        } else {
          flow.next_active = cursor_;
          flow.previous_active = table_[cursor_].previous_active;
          table_[flow.previous_active].next_active = static_cast<uint32_t>(at);
          table_[cursor_].previous_active = static_cast<uint32_t>(at);
        }
        held++;
      }
      flow.head = s;
    } else {
      slots_[flow.tail].next = s;
    }
    flow.tail = s;
    flow.count++;
    total++;
  }
  total_.store(total, std::memory_order_relaxed);
  held_.store(held, std::memory_order_relaxed);
}

template <typename Sink>
size_t PacketStore::Release(uint32_t flow_id, Sink &&sink) {
  const size_t at = Probe(flow_id);
  Flow &flow = table_[at];
  if (flow.count == 0) {
    return 0;
  }
  const size_t released = flow.count;
  while (flow.count != 0) {
    sink(PopFront(flow));
  }
  EraseAt(at);
  total_.store(total_.load(std::memory_order_relaxed) - released,
               std::memory_order_relaxed);
  held_.store(held_.load(std::memory_order_relaxed) - 1,
              std::memory_order_relaxed);
  return released;
}

// The flows table as a transactional resource (D-129, D-201): key
// EncodeKey(uint32_t flow id), value PacketStoreLimits. Configuration, not
// state the packet path changes: a transaction edits one private copy of the
// owner's published table (cloned by its first Reserve) and its first Publish
// swaps the copy in and retires the old table, so a transaction is one copy
// and one grace period, and workers never pause. Erasing an id that has no
// entry is a no-op, and an erase removes limits, never packets. The owner
// keeps the RcuPtr (Initialize it before registering), declares the table
// `.Rebuilds()` with requires_paused_workers false, and binds
// MakePacketStoreFlowsCodec (framework/packet_store_codec.h) for the wire.
// Memory (D-198): Bytes() is the resource object, admitted before it is
// constructed (MemoryCost<PacketStoreFlowsResource>); the maps it builds
// (the first empty one, each transaction's clone and every insert into it)
// are admitted through `growth` before they are allocated and given back
// when freed, a retired map when its grace period ends. A refusal throws
// utils::MemoryRefused from Reserve, which refuses the transaction; the
// published table is untouched. Detached at destruction: the owner resets
// `published` after the resource, then releases its admission.
class PacketStoreFlowsResource final : public Resource {
 public:
  // Publishes the first, empty table; `published` must be uninitialized.
  // Throws what `growth` throws if that is refused.
  PacketStoreFlowsResource(std::string name,
                           rcu::RcuPtr<PacketStoreLimitsMap> &published,
                           utils::MemoryGrowth *growth = nullptr);
  ~PacketStoreFlowsResource() override;

  static size_t Bytes() noexcept { return sizeof(PacketStoreFlowsResource); }
  static size_t GlibcBytes() noexcept { return Bytes(); }
  // Held now: Bytes() plus every map admitted and not yet freed.
  size_t bytes() const noexcept { return Bytes() + memory_->held(); }

  size_t LiveCount() const override;
  bool Contains(const ResourceKey &key) const override;
  std::expected<Reservation, std::string> Reserve(const Op &op) override;
  void EndTransaction() noexcept override;

 private:
  class SwapOp;

  std::shared_ptr<PacketStoreMemory> memory_;
  rcu::RcuPtr<PacketStoreLimitsMap> &published_;
  std::unique_ptr<PacketStoreLimitsMap> staged_;
  bool swapped_ = false;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_PACKET_STORE_H_
