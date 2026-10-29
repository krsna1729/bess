// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/packet_store.h"

#include <algorithm>
#include <any>
#include <bit>
#include <iterator>
#include <new>

#include "packet.h"

namespace bess {
namespace dataplane {

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------

size_t PacketStore::FlowCapacity(uint32_t max_packets) noexcept {
  return std::bit_ceil(std::max<size_t>(16, size_t{max_packets} * 2));
}

size_t PacketStore::Bytes(const PacketStoreConfig &config) noexcept {
  return sizeof(PacketStore) + size_t{config.max_packets} * sizeof(Slot) +
         FlowCapacity(config.max_packets) * sizeof(Flow);
}

std::expected<std::unique_ptr<PacketStore>, std::string> PacketStore::Create(
    const PacketStoreConfig &config) {
  if (config.max_packets == 0) {
    return std::unexpected("max_packets must be positive");
  }
  if (config.max_packets > (uint32_t{1} << 30)) {
    return std::unexpected("max_packets must be at most 2^30");
  }
  if (config.max_packets_per_flow == 0) {
    return std::unexpected("max_packets_per_flow must be positive");
  }
  const size_t capacity = FlowCapacity(config.max_packets);
  std::unique_ptr<Slot[]> slots(new (std::nothrow) Slot[config.max_packets]);
  std::unique_ptr<Flow[]> table(new (std::nothrow) Flow[capacity]);
  if (slots == nullptr || table == nullptr) {
    return std::unique_ptr<PacketStore>();
  }
  return std::unique_ptr<PacketStore>(new (std::nothrow) PacketStore(
      config, std::move(slots), std::move(table), capacity));
}

PacketStore::PacketStore(const PacketStoreConfig &config,
                         std::unique_ptr<Slot[]> slots,
                         std::unique_ptr<Flow[]> table,
                         size_t capacity) noexcept
    : config_(config),
      slots_(std::move(slots)),
      table_(std::move(table)),
      mask_(capacity - 1),
      shift_(64 - static_cast<unsigned>(std::countr_zero(capacity))) {
  for (size_t i = 0; i < capacity; i++) {
    table_[i] = Flow{0, kNone, kNone, 0};
  }
  // Every slot starts on the free list, in index order.
  for (uint32_t s = 0; s < config_.max_packets; s++) {
    slots_[s] = Slot{PacketHandle{}, 0,
                     s + 1 < config_.max_packets ? s + 1 : kNone};
  }
  free_ = 0;
}

PacketStore::~PacketStore() { Clear(); }

void PacketStore::EraseAt(size_t i) noexcept {
  // Unlink the held flow before relocating its probe chain.
  const Flow &removed = table_[i];
  if (removed.next_active == i) {
    cursor_ = kNone;
  } else {
    table_[removed.previous_active].next_active = removed.next_active;
    table_[removed.next_active].previous_active = removed.previous_active;
    if (cursor_ == i) {
      cursor_ = removed.next_active;
    }
  }
  // Backward-shift deletion: every later entry of the probe chain whose home
  // is not cyclically in (hole, j] moves into the hole, so a lookup never
  // meets an empty entry before its key.
  size_t hole = i;
  for (size_t j = (i + 1) & mask_; table_[j].count != 0; j = (j + 1) & mask_) {
    const size_t home = Home(table_[j].id);
    if (((j - home) & mask_) >= ((j - hole) & mask_)) {
      table_[hole] = table_[j];
      Flow &moved = table_[hole];
      if (moved.next_active == j) {
        moved.next_active = static_cast<uint32_t>(hole);
      } else {
        table_[moved.next_active].previous_active = static_cast<uint32_t>(hole);
      }
      if (moved.previous_active == j) {
        moved.previous_active = static_cast<uint32_t>(hole);
      } else {
        table_[moved.previous_active].next_active = static_cast<uint32_t>(hole);
      }
      if (cursor_ == j) {
        cursor_ = static_cast<uint32_t>(hole);
      }
      hole = j;
    }
  }
  table_[hole].count = 0;
}

bool PacketStore::ExpireFlow(Flow &flow, uint64_t timeout, uint64_t now_ns,
                             size_t &total) {
  while (timeout != 0 && flow.count != 0 &&
         now_ns - slots_[flow.head].timestamp_ns > timeout) {
    PacketHandle handle = PopFront(flow);
    NotifyDiscard(handle, PacketStoreDiscardReason::kExpired);
    PacketFree(handle);
    total--;
  }
  return flow.count == 0;
}

void PacketStore::ExpireAll(const PacketStoreLimitsMap &limits,
                            uint64_t now_ns) {
  size_t total = total_.load(std::memory_order_relaxed);
  size_t held = held_.load(std::memory_order_relaxed);
  for (size_t i = 0; i <= mask_ && held != 0;) {
    Flow &flow = table_[i];
    if (flow.count != 0 &&
        ExpireFlow(flow, LimitsFor(limits, flow.id).first, now_ns, total)) {
      held--;
      EraseAt(i);
      continue;  // an entry may have shifted into i
    }
    i++;
  }
  total_.store(total, std::memory_order_relaxed);
  held_.store(held, std::memory_order_relaxed);
}

void PacketStore::ExpireBounded(const PacketStoreLimitsMap &limits,
                                uint64_t now_ns) {
  size_t total = total_.load(std::memory_order_relaxed);
  size_t held = held_.load(std::memory_order_relaxed);
  const size_t budget = std::min(held, kExpireFlowsPerRun);
  for (size_t examined = 0; examined < budget && held != 0; examined++) {
    const size_t i = cursor_;
    Flow &flow = table_[i];
    cursor_ = flow.next_active;
    if (ExpireFlow(flow, LimitsFor(limits, flow.id).first, now_ns, total)) {
      held--;
      EraseAt(i);
    }
  }
  total_.store(total, std::memory_order_relaxed);
  held_.store(held, std::memory_order_relaxed);
}

size_t PacketStore::Drop(uint32_t flow_id) {
  const size_t at = Probe(flow_id);
  Flow &flow = table_[at];
  if (flow.count == 0) {
    return 0;
  }
  const size_t dropped = flow.count;
  PacketHandle burst[32];
  while (flow.count != 0) {
    size_t n = 0;
    while (flow.count != 0 && n < std::size(burst)) {
      PacketHandle handle = PopFront(flow);
      NotifyDiscard(handle, PacketStoreDiscardReason::kDropped);
      burst[n++] = handle;
    }
    PacketFreeBulk(burst, n);
  }
  EraseAt(at);
  total_.store(total_.load(std::memory_order_relaxed) - dropped,
               std::memory_order_relaxed);
  held_.store(held_.load(std::memory_order_relaxed) - 1,
              std::memory_order_relaxed);
  return dropped;
}

size_t PacketStore::Clear() {
  size_t freed = 0;
  if (held_.load(std::memory_order_relaxed) != 0) {
    for (size_t i = 0; i <= mask_; i++) {
      Flow &flow = table_[i];
      while (flow.count != 0) {
        PacketHandle handle = PopFront(flow);
        NotifyDiscard(handle, PacketStoreDiscardReason::kCleared);
        PacketFree(handle);
        freed++;
      }
    }
  }
  cursor_ = kNone;
  total_.store(0, std::memory_order_relaxed);
  held_.store(0, std::memory_order_relaxed);
  return freed;
}

// ---------------------------------------------------------------------------
// The flows resource
// ---------------------------------------------------------------------------

// Every operation of a transaction stages one of these; the first to publish
// swaps the private copy in, once, and retires the table it replaced.
PacketStoreFlowsResource::PacketStoreFlowsResource(
    std::string name, rcu::RcuPtr<PacketStoreLimitsMap> &published,
    utils::MemoryGrowth *growth)
    : Resource(std::move(name)),
      memory_(std::make_shared<PacketStoreMemory>()),
      published_(published) {
  memory_->Attach(growth);
  published_.Initialize(std::make_unique<const PacketStoreLimitsMap>(
      PacketStoreAllocator<std::pair<const uint32_t, PacketStoreLimits>>(memory_)));
}

PacketStoreFlowsResource::~PacketStoreFlowsResource() {
  memory_->Attach(nullptr);
}

class PacketStoreFlowsResource::SwapOp final : public StagedOp {
 public:
  explicit SwapOp(PacketStoreFlowsResource &owner) : owner_(owner) {}

  void Publish(Retirer &retirer) noexcept override {
    if (!owner_.swapped_) {
      retirer.Retire(owner_.published_.Exchange(std::move(owner_.staged_)));
      owner_.swapped_ = true;
    }
  }

 private:
  PacketStoreFlowsResource &owner_;
};

size_t PacketStoreFlowsResource::LiveCount() const {
  const PacketStoreLimitsMap *limits = published_.Read();
  return limits == nullptr ? 0 : limits->size();
}

bool PacketStoreFlowsResource::Contains(const ResourceKey &key) const {
  uint32_t id;
  const PacketStoreLimitsMap *limits = published_.Read();
  return limits != nullptr && DecodeKey(key, &id) && limits->contains(id);
}

std::expected<Resource::Reservation, std::string>
PacketStoreFlowsResource::Reserve(const Op &op) {
  uint32_t id;
  if (!DecodeKey(op.key, &id)) {
    return std::unexpected("invalid flow id key");
  }
  const PacketStoreLimitsMap *published = published_.Read();
  const bool existed = published != nullptr && published->contains(id);

  // Everything fallible happens here, on the transaction's private copy: the
  // first Reserve clones the published table, every later one edits it.
  if (staged_ == nullptr) {
    staged_ = std::make_unique<PacketStoreLimitsMap>(*published);
    swapped_ = false;
  }
  if (op.kind == OpKind::kErase) {
    staged_->erase(id);
  } else {
    const PacketStoreLimits *limits = std::any_cast<PacketStoreLimits>(&op.value);
    if (limits == nullptr) {
      return std::unexpected("wrong value type (want flow limits)");
    }
    (*staged_)[id] = *limits;
  }
  // Publishing replaces the table that is live now.
  Reservation reservation{std::make_unique<SwapOp>(*this), {},
                          Footprint{.retires = 1}};
  reservation.existed = existed;
  return reservation;
}

void PacketStoreFlowsResource::EndTransaction() noexcept {
  staged_.reset();
  swapped_ = false;
}

}  // namespace dataplane
}  // namespace bess
