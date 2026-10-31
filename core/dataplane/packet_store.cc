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
  try {
    utils::DpdkArray<Slot> slots(config.max_packets);
    utils::DpdkArray<Flow> table(capacity);
    return std::unique_ptr<PacketStore>(new PacketStore(
        config, std::move(slots), std::move(table), capacity));
  } catch (const utils::MemoryRefused &) {
    throw;  // the policy's refusal names its reason (D-198)
  } catch (const std::bad_alloc &) {
    return std::unique_ptr<PacketStore>();
  }
}

void *PacketStore::operator new(size_t bytes) {
  return utils::DpdkAllocate(
      bytes, std::max<size_t>(alignof(PacketStore), RTE_CACHE_LINE_SIZE));
}

void PacketStore::operator delete(void *p) noexcept { utils::DpdkFree(p); }

PacketStore::PacketStore(const PacketStoreConfig &config,
                         utils::DpdkArray<Slot> slots,
                         utils::DpdkArray<Flow> table,
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

void PacketStore::ExpireAll(const PacketStoreLimitsTable &limits,
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

void PacketStore::ExpireBounded(const PacketStoreLimitsTable &limits,
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

// An upsert's object, made by Reserve, published in place by Publish.
class PacketStoreFlowsResource::PublishOp final : public StagedOp {
 public:
  PublishOp(PacketStoreLimitsTable &table, PacketStoreFlowId id,
            PacketStoreLimitsTable::Owned limits)
      : table_(table), id_(id), limits_(std::move(limits)) {}
  void Publish(Retirer &retirer) noexcept override {
    retirer.Retire(table_.Publish(id_, std::move(limits_)));
  }

 private:
  PacketStoreLimitsTable &table_;
  PacketStoreFlowId id_;
  PacketStoreLimitsTable::Owned limits_;
};

// Empties the entry, if there is one, when the transaction publishes. No
// other resource refers to a flow id, so the id is free at once; the object
// goes after a grace period.
class PacketStoreFlowsResource::EraseOp final : public StagedOp {
 public:
  EraseOp(PacketStoreLimitsTable &table, PacketStoreFlowId id)
      : table_(table), id_(id) {}
  void Publish(Retirer &retirer) noexcept override {
    if (table_.Contains(id_)) {
      table_.Retire(id_);
      retirer.Retire(table_.Unpublish(id_));
    }
  }

 private:
  PacketStoreLimitsTable &table_;
  PacketStoreFlowId id_;
};

bool PacketStoreFlowsResource::Contains(const ResourceKey &key) const {
  uint32_t id;
  return DecodeKey(key, &id) && table_.Contains(PacketStoreFlowId(id));
}

std::expected<Resource::Reservation, std::string>
PacketStoreFlowsResource::Reserve(const Op &op) {
  uint32_t raw;
  if (!DecodeKey(op.key, &raw)) {
    return std::unexpected("invalid flow id key");
  }
  const PacketStoreFlowId id(raw);
  Reservation reservation;
  reservation.existed = table_.Contains(id);
  // Each operation retires at most the one object it replaces or removes.
  reservation.footprint = Footprint{.retires = 1};
  if (op.kind == OpKind::kErase) {
    // An id outside the table has no entry either: a no-op.
    reservation.staged = std::make_unique<EraseOp>(table_, id);
    return reservation;
  }
  if (!table_.ValidId(id)) {
    return std::unexpected("flow id " + std::to_string(raw) +
                           " is outside the flows table (1.." +
                           std::to_string(table_.capacity()) + ")");
  }
  const PacketStoreLimits *limits = std::any_cast<PacketStoreLimits>(&op.value);
  if (limits == nullptr) {
    return std::unexpected("wrong value type (want flow limits)");
  }
  // Make may add a slab chunk, admitted first through the table's growth
  // hook (D-198): its MemoryRefused refuses the transaction.
  reservation.staged =
      std::make_unique<PublishOp>(table_, id, table_.Make(*limits));
  return reservation;
}

}  // namespace dataplane
}  // namespace bess
