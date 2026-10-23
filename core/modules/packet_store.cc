// SPDX-License-Identifier: BSD-3-Clause

#include "packet_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "dataplane/resource_registry.h"
#include "utils/format.h"
#include "utils/rte_ring_alloc.h"
#include "utils/time.h"

namespace dataplane = bess::dataplane;
namespace stats = bess::stats;

// DPDK 25.11's rte_ring_elem.h lacks its own extern "C", so
// rte_ring_get_memsize_elem has C++ linkage there and cannot be called from a
// header; this reaches the C symbol directly, as handoff.cc does for the
// handoff channel's own element size. The pointer-ring size (rte_ring_get_memsize)
// is for 8-byte slots and would under-size an element ring.
extern "C" ssize_t BessPacketStoreRingMemsizeElem(unsigned esize,
                                                  unsigned count)
    __asm__("rte_ring_get_memsize_elem");

namespace {

using bess::utils::be32_t;

// A ring in caller-owned memory, the pattern Queue and DRR share (Queue's
// AllocRing, drr.cc:514): rte_ring_create would allocate an EAL memzone, which
// the malloc-pool module tests have no hugepages for.
struct rte_ring *AllocRing(uint32_t slots, const char *prefix, unsigned flags,
                           ssize_t bytes) {
  if (bytes < 0) {
    return nullptr;
  }
  struct rte_ring *ring = static_cast<rte_ring *>(
      bess::utils::AllocRingMem(static_cast<size_t>(bytes)));
  if (ring == nullptr) {
    return nullptr;
  }
  const std::string name = bess::utils::NewRingName(prefix);
  if (rte_ring_init(ring, name.c_str(), slots, flags) != 0) {
    std::free(ring);
    return nullptr;
  }
  return ring;
}

}  // namespace

// All three act on held packets, so none of them answers data: they hand a
// request to the owner (or, when no worker runs the module, apply it here).
// Control cannot see the store, so it accepts the action and reports what it
// was told to apply - refusing an action because a worker holds nothing would
// be a guess, and a false refusal is worse than a no-op a client can see at the
// module's gates.
const Commands PacketStore::cmds = {
    {"release", "PacketStoreCommandReleaseArg",
     MODULE_CMD_FUNC(&PacketStore::CommandRelease), Command::THREAD_SAFE},
    {"drop", "PacketStoreCommandDropArg",
     MODULE_CMD_FUNC(&PacketStore::CommandDrop), Command::THREAD_SAFE},
    {"clear", "PacketStoreCommandClearArg",
     MODULE_CMD_FUNC(&PacketStore::CommandClear), Command::THREAD_SAFE},
};

// Per-flow limits are the "<module>/flows" resource (D-129): configuration,
// not state the dataplane acts on directly. The owner reads the published table
// once per task run, so the table needs no paused workers, and an operation
// publishes a replacement instead of touching anything live.
const ResourceTables PacketStore::resource_tables = {
    ResourceTable::Map<bess::pb::PacketStoreFlowKey,
                       bess::pb::PacketStoreFlowValue>(
        "flows", /*requires_paused_workers=*/false)
        .Rebuilds()};

// ---------------------------------------------------------------------------
// The owner's side: the module's task
// ---------------------------------------------------------------------------

std::pair<uint64_t, uint32_t> PacketStore::LimitsFor(const LimitsMap &limits,
                                                     uint32_t flow_id) const {
  auto it = limits.find(flow_id);
  if (it == limits.end()) {
    return {timeout_ns_, max_packets_per_flow_};
  }
  const FlowLimits &entry = it->second;
  const uint64_t timeout =
      entry.timeout_ns.has_value() ? *entry.timeout_ns : timeout_ns_;
  // An explicit 0 is the module-wide default for the cap, not "hold nothing".
  uint32_t cap = max_packets_per_flow_;
  if (entry.max_packets_per_flow.has_value() &&
      *entry.max_packets_per_flow != 0) {
    cap = *entry.max_packets_per_flow;
  }
  return {timeout, cap};
}

void PacketStore::EvictExpired(const LimitsMap &limits, uint64_t now_ns) {
  for (auto it = store_.begin(); it != store_.end();) {
    auto &queue = it->second;
    // The flow's own timeout where its entry gives one: 0 means its packets
    // never expire, so nothing here is old enough.
    const uint64_t timeout = LimitsFor(limits, it->first).first;
    while (timeout != 0 && !queue.empty() &&
           (now_ns - queue.front().timestamp_ns > timeout)) {
      bess::PacketFree(queue.front().pkt.handle());
      queue.pop_front();
      total_packets_.store(total_packets_.load(std::memory_order_relaxed) - 1,
                           std::memory_order_relaxed);
    }
    if (queue.empty()) {
      it = store_.erase(it);
      flows_held_.store(flows_held_.load(std::memory_order_relaxed) - 1,
                        std::memory_order_relaxed);
    } else {
      ++it;
    }
  }
}

void PacketStore::EvictExpiredBounded(const LimitsMap &limits,
                                      uint64_t now_ns) {
  if (store_.empty()) {
    expire_cursor_ = 0;
    return;
  }
  size_t visited = 0;
  auto it = store_.lower_bound(expire_cursor_);
  while (visited < kExpireFlowsPerRun) {
    if (it == store_.end()) {
      it = store_.begin();
    }
    auto &queue = it->second;
    const uint64_t timeout = LimitsFor(limits, it->first).first;
    while (timeout != 0 && !queue.empty() &&
           (now_ns - queue.front().timestamp_ns > timeout)) {
      bess::PacketFree(queue.front().pkt.handle());
      queue.pop_front();
      total_packets_.store(total_packets_.load(std::memory_order_relaxed) - 1,
                           std::memory_order_relaxed);
    }
    visited++;
    if (queue.empty()) {
      it = store_.erase(it);
      flows_held_.store(flows_held_.load(std::memory_order_relaxed) - 1,
                        std::memory_order_relaxed);
      if (store_.empty()) {
        expire_cursor_ = 0;
        return;
      }
    } else {
      ++it;
    }
    // Resume at the next flow this sweep did not reach, or from the start once
    // it ran off the end.
    expire_cursor_ = it == store_.end() ? 0 : it->first;
  }
}

void PacketStore::StoreBatch(Context *ctx, bess::PacketBatch *batch,
                             const uint32_t *flow_ids, const LimitsMap &limits,
                             uint64_t now_ns) {
  // Evict expired packets if store is growing
  if (total_packets_.load(std::memory_order_relaxed) >= max_packets_) {
    EvictExpired(limits, now_ns);
  }

  // One writer at a time (the owner, or control while no worker runs), so the
  // counts are kept in registers and stored once a batch: a locked
  // read-modify-write per packet would protect nothing.
  size_t total = total_packets_.load(std::memory_order_relaxed);
  size_t held = flows_held_.load(std::memory_order_relaxed);

  for (int i = 0; i < batch->cnt(); i++) {
    const uint32_t flow_id = flow_ids[i];
    bess::PacketRef pkt = batch->packet(i);

    auto [it, inserted] = store_.try_emplace(flow_id);
    if (inserted) {
      held++;
    }
    std::deque<StoredPacket> &queue = it->second;
    const uint32_t cap = LimitsFor(limits, flow_id).second;

    // A flow's own cap where its `flows` entry gives one, the module-wide
    // default otherwise. A loop, not one eviction: an upsert can lower the cap
    // below what the queue already holds, and one arrival must bring it down.
    while (queue.size() >= cap) {
      // Evict oldest packet in this flow to gate 1
      bess::PacketRef oldest = queue.front().pkt;
      queue.pop_front();
      total--;
      EmitPacket(ctx, oldest, 1);
    }

    // Check global limit
    if (total >= max_packets_) {
      // Global overflow -> drop to gate 1
      EmitPacket(ctx, pkt, 1);
      continue;
    }

    // Buffer packet
    queue.push_back(StoredPacket{
        .pkt = pkt,
        .timestamp_ns = now_ns,
    });
    total++;
  }

  total_packets_.store(total, std::memory_order_relaxed);
  flows_held_.store(held, std::memory_order_relaxed);
}

uint32_t PacketStore::DrainIngress(Context *ctx, uint32_t budget) {
  const LimitsMap *limits = limits_.Read();
  const uint64_t now_ns = tsc_to_ns(current_worker.current_tsc());
  IngressPacket pkts[bess::PacketBatch::kMaxBurst];
  uint32_t taken = 0;

  while (budget > 0) {
    const unsigned n = rte_ring_sc_dequeue_burst_elem(
        ingress_, pkts, sizeof(IngressPacket),
        std::min<uint32_t>(budget, bess::PacketBatch::kMaxBurst), nullptr);
    if (n == 0) {
      break;
    }
    bess::PacketBatch batch;
    uint32_t flow_ids[bess::PacketBatch::kMaxBurst];
    batch.clear();
    for (unsigned i = 0; i < n; i++) {
      batch.add(bess::PacketRef(pkts[i].handle));
      flow_ids[i] = pkts[i].flow_id;
    }
    StoreBatch(ctx, &batch, flow_ids, *limits, now_ns);
    budget -= n;
    taken += n;
    if (n < bess::PacketBatch::kMaxBurst) {
      break;
    }
  }
  return taken;
}

void PacketStore::Apply(const Request &request) {
  switch (request.kind) {
    case Request::Kind::kRelease: {
      auto it = store_.find(request.id);
      if (it == store_.end()) {
        break;
      }
      std::deque<StoredPacket> &queue = it->second;
      while (!queue.empty()) {
        release_queue_.push_back(ReleasedPacket{
            .pkt = queue.front().pkt,
            .gate = request.gate,
        });
        queue.pop_front();
        total_packets_.store(total_packets_.load(std::memory_order_relaxed) - 1,
                             std::memory_order_relaxed);
      }
      store_.erase(it);
      flows_held_.store(flows_held_.load(std::memory_order_relaxed) - 1,
                        std::memory_order_relaxed);
      break;
    }
    case Request::Kind::kDrop: {
      auto it = store_.find(request.id);
      if (it == store_.end()) {
        break;
      }
      std::deque<StoredPacket> &queue = it->second;
      uint64_t dropped = 0;
      while (!queue.empty()) {
        bess::PacketFree(queue.front().pkt.handle());
        queue.pop_front();
        dropped++;
      }
      total_packets_.store(total_packets_.load(std::memory_order_relaxed) -
                               static_cast<size_t>(dropped),
                           std::memory_order_relaxed);
      dropped_packets_.store(
          dropped_packets_.load(std::memory_order_relaxed) + dropped,
          std::memory_order_relaxed);
      store_.erase(it);
      flows_held_.store(flows_held_.load(std::memory_order_relaxed) - 1,
                        std::memory_order_relaxed);
      break;
    }
    case Request::Kind::kClear:
      ClearStore();
      break;
  }
}

uint32_t PacketStore::DrainRequests(uint32_t max) {
  Request request;
  uint32_t applied = 0;
  while (applied < max && requests_ != nullptr &&
         rte_ring_sc_dequeue_elem(requests_, &request, sizeof(Request)) == 0) {
    Apply(request);
    applied++;
  }
  return applied;
}

void PacketStore::ClearStore() {
  size_t freed = 0;
  for (auto &[id, queue] : store_) {
    while (!queue.empty()) {
      bess::PacketFree(queue.front().pkt.handle());
      queue.pop_front();
      freed++;
    }
  }
  while (!release_queue_.empty()) {
    bess::PacketFree(release_queue_.front().pkt.handle());
    release_queue_.pop_front();
  }
  store_.clear();
  flows_held_.store(0, std::memory_order_relaxed);
  total_packets_.fetch_sub(freed, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Control's side
// ---------------------------------------------------------------------------

bool PacketStore::Deliver(const Request &request) {
  // Control owns the store only when no worker runs at all and nothing is in
  // flight. The module's own worker is the owner, but the active-worker set is
  // refreshed by the resume hook, which a per-worker ResumeWorker does not
  // rerun - so the safe question is whether any worker runs, not whether this
  // module's is marked active. The ingress ring must be empty too: packets a
  // worker handed over before this request are the request's to act on, and
  // control cannot store them (an eviction there needs a task context).
  // Otherwise the request is posted, and the owner applies it - in ring order,
  // after those packets - on its next task run.
  if (!is_any_worker_running() &&
      (ingress_ == nullptr || rte_ring_count(ingress_) == 0)) {
    // Control owns the store here, so the snapshot means nothing: it applies
    // what is queued and then the request, in that order.
    requests_due_ = 0;
    ingress_owed_ = 0;
    DrainRequests(kRequestSlots);
    Apply(request);
    return true;
  }
  Request copy = request;
  return rte_ring_sp_enqueue_elem(requests_, &copy, sizeof(Request)) == 0;
}

void PacketStore::BeginTransaction() {
  if (staged_limits_ == nullptr) {
    const LimitsMap *published = limits_.Read();
    staged_limits_ = std::make_unique<LimitsMap>(
        published == nullptr ? LimitsMap() : *published);
    published_staged_ = false;
  }
}

void PacketStore::PublishStaged(dataplane::Retirer &retirer) {
  // Every operation of the transaction edited the same private copy, so the
  // swap happens once: the published table becomes what that copy holds, and
  // no operation can publish over another's edit (meter.cc:307-322).
  if (!published_staged_) {
    retirer.Retire(limits_.Exchange(std::move(staged_limits_)));
    published_staged_ = true;
  }
}

void PacketStore::EndTransaction() noexcept {
  staged_limits_.reset();
  published_staged_ = false;
}

// ---------------------------------------------------------------------------
// The "<module>/flows" resource (D-129)
// ---------------------------------------------------------------------------

// Key the flow id (uint32), value its own limits, each field optional - an
// absent field leaves the module-wide argument in force, so the entry is
// stored as given and resolved on use. This is configuration: the table the
// owner reads is replaced as a whole, and an erase removes an entry without
// touching a packet (the `drop` command is what frees packets).
class PacketStore::FlowsResource final : public dataplane::Resource {
 public:
  explicit FlowsResource(PacketStore &owner)
      : Resource(owner.name() + "/flows"), owner_(owner) {}

  size_t LiveCount() const override {
    const LimitsMap *limits = owner_.limits_.Read();
    return limits == nullptr ? 0 : limits->size();
  }

  bool Contains(const dataplane::ResourceKey &key) const override {
    uint32_t id;
    const LimitsMap *limits = owner_.limits_.Read();
    return limits != nullptr && dataplane::DecodeKey(key, &id) &&
           limits->contains(id);
  }

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    uint32_t id;
    if (!dataplane::DecodeKey(op.key, &id)) {
      return std::unexpected("invalid flow id key");
    }
    const LimitsMap *published = owner_.limits_.Read();
    const bool existed = published != nullptr && published->contains(id);

    // Everything fallible happens here, on the transaction's private copy: the
    // first Reserve of the transaction clones the published table, and every
    // later one edits that clone.
    owner_.BeginTransaction();
    if (op.kind == dataplane::OpKind::kErase) {
      // An erased entry means the flow has no limits of its own, not that its
      // packets are gone: the deletion path drops what a flow holds, and a
      // transaction of many erases edits one copy and publishes once.
      owner_.staged_limits_->erase(id);
    } else {
      const FlowLimits *limits = std::any_cast<FlowLimits>(&op.value);
      if (limits == nullptr) {
        return std::unexpected("wrong value type (want flow limits)");
      }
      (*owner_.staged_limits_)[id] = *limits;
    }
    // Publishing replaces the table that is live now.
    Reservation reservation{std::make_unique<FlowsOp>(owner_), {},
                            Footprint{.retires = 1}};
    reservation.existed = existed;
    return reservation;
  }

  void EndTransaction() noexcept override { owner_.EndTransaction(); }

 private:
  class FlowsOp final : public dataplane::StagedOp {
   public:
    explicit FlowsOp(PacketStore &owner) : owner_(owner) {}

    void Publish(dataplane::Retirer &retirer) noexcept override {
      // The transaction's private copy becomes the published table, once.
      owner_.PublishStaged(retirer);
    }

   private:
    PacketStore &owner_;
  };

  PacketStore &owner_;
};

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

CommandResponse PacketStore::Init(const bess::pb::PacketStoreArg &arg) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  max_packets_ = arg.max_packets() ? arg.max_packets() : 4096;
  // The store holds mbuf handles, so its bound plus what the ingress ring can
  // hold must fit the pool the hot path allocates from. A bound near the
  // pool's size would starve every other module's batches, and the resulting
  // drops would look like a network fault rather than a configuration.
  if (const bess::PacketPool *pool = bess::PacketPool::GetDefaultPool();
      pool != nullptr &&
      pool->Capacity() <
          static_cast<size_t>(max_packets_) + size_t{kIngressSlots}) {
    return CommandFailure(
        EINVAL,
        "max_packets %u plus the %u the ingress ring can hold exceeds the "
        "packet pool's %zu buffers",
        max_packets_, kIngressSlots, pool->Capacity());
  }
  max_packets_per_flow_ =
      arg.max_packets_per_flow() ? arg.max_packets_per_flow() : 64;
  timeout_ns_ = arg.timeout_ms()
                    ? static_cast<uint64_t>(arg.timeout_ms()) * 1'000'000ULL
                    : 5'000'000'000ULL;

  task_id_t tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID) {
    return CommandFailure(ENOMEM, "Task creation failed");
  }

  // The flow key is the first of these the graph provides; the default pair is
  // the historical behaviour, store_id over vif_id.
  if (arg.key_attrs().empty()) {
    key_attrs_ = {"store_id", "vif_id"};
  } else {
    key_attrs_.assign(arg.key_attrs().begin(), arg.key_attrs().end());
  }
  key_attr_ids_.clear();
  for (const std::string &name : key_attrs_) {
    const int attr_id = AddMetadataAttr(name, sizeof(be32_t), AccessMode::kRead);
    if (attr_id < 0) {
      // Without this the id is an error code, and the packet path would index
      // attr_offsets_ with it: a duplicate, an empty name, more than
      // kMaxAttrsPerModule names, or a size clash with another module.
      return CommandFailure(-attr_id, "add_metadata_attr(%s) failed",
                            name.c_str());
    }
    key_attr_ids_.push_back(attr_id);
  }

  // Packets in from any worker, requests in from control. The ingress ring is
  // multi-producer (every worker whose graph reaches the module) and the owner
  // is its only consumer, so the default enqueue is the multi-producer one and
  // only the dequeue is flagged.
  ingress_ = AllocRing(kIngressSlots, "packet_store_ingress", RING_F_SC_DEQ,
                       BessPacketStoreRingMemsizeElem(sizeof(IngressPacket),
                                                      kIngressSlots));
  if (ingress_ == nullptr) {
    return CommandFailure(ENOMEM, "the ingress ring could not be created");
  }
  requests_ = AllocRing(kRequestSlots, "packet_store_requests",
                        RING_F_SP_ENQ | RING_F_SC_DEQ,
                        BessPacketStoreRingMemsizeElem(sizeof(Request),
                                                       kRequestSlots));
  if (requests_ == nullptr) {
    return CommandFailure(ENOMEM, "the request ring could not be created");
  }

  // No flow has limits of its own yet: every flow takes the module-wide
  // numbers until an entry says otherwise. Published before any worker can
  // read it (Init runs before the module is attached).
  limits_.Initialize(std::make_unique<const LimitsMap>());

  // The per-flow limits (D-129). The binding is declared after the resource it
  // binds and reset before it is (D-044). No worker_running callback: the
  // resource does not require paused workers - the owner reads the published
  // table once per task run, and a transaction publishes a replacement.
  flows_resource_ = std::make_unique<FlowsResource>(*this);
  flows_binding_ = init_context().codecs().Bind(
      *flows_resource_,
      std::make_shared<bess::framework::TypedCodec<
          bess::pb::PacketStoreFlowKey, bess::pb::PacketStoreFlowValue>>(
          [](const bess::pb::PacketStoreFlowKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            // The flow id is the metadata value the classifier wrote, and 0 is
            // every packet that has none: all keyless sessions share that
            // bucket, so a per-session entry for it would apply one session's
            // limits to all of them.
            if (key.id() == 0) {
              return std::unexpected("flow id 0 is every keyless packet");
            }
            return dataplane::EncodeKey(key.id());
          },
          [](const bess::pb::PacketStoreFlowValue &value)
              -> std::expected<std::any, std::string> {
            // Field presence is the point: an absent field leaves the
            // module-wide argument in force, and a present 0 is a decision.
            FlowLimits limits;
            if (value.has_timeout_ns()) {
              limits.timeout_ns = value.timeout_ns();
            }
            if (value.has_max_packets_per_flow()) {
              limits.max_packets_per_flow = value.max_packets_per_flow();
            }
            return std::any(limits);
          },
          // Read back as given: an entry carries exactly the fields it set,
          // so a client can tell an absent field from a present zero.
          [this](const auto &visit) {
            const LimitsMap *limits = limits_.Read();
            if (limits == nullptr) {
              return;
            }
            for (const auto &[id, entry] : *limits) {
              bess::pb::PacketStoreFlowKey key;
              key.set_id(id);
              bess::pb::PacketStoreFlowValue value;
              if (entry.timeout_ns.has_value()) {
                value.set_timeout_ns(*entry.timeout_ns);
              }
              if (entry.max_packets_per_flow.has_value()) {
                value.set_max_packets_per_flow(*entry.max_packets_per_flow);
              }
              visit(key, value);
            }
          }));

  // The engine reaches a resource only once it is registered (D-021); the
  // binding above wires its codec. A failure destroys the module, and DeInit
  // frees the rings.
  if (auto registered =
          init_context().resources().Register(flows_resource_.get());
      !registered) {
    flows_binding_.Reset();
    flows_resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  // Packets the drop command freed, for ListMetrics. Registered last, after the
  // resource whose operations may drop.
  dropped_metric_ = init_context().metrics().Register(
      [this](stats::MetricWriter &writer) {
        writer.Counter("bess_packet_store_dropped_packets_total",
                       "Packets freed by the drop command",
                       static_cast<double>(dropped_packets_.load()),
                       {{"module", name()}});
        // A second loss, and a different one: a worker's batch the ingress ring
        // had no room for, which the module freed rather than holding. Before
        // this change an overflow of the store went out on gate 1; a batch the
        // ring refuses cannot (downstream runs on the owner's worker only), so
        // it is freed and counted here instead.
        writer.Counter("bess_packet_store_ingress_dropped_packets_total",
                       "Packets a full ingress ring refused (freed, not "
                       "stored)",
                       static_cast<double>(ingress_dropped_.load()),
                       {{"module", name()}});
      });

  return CommandSuccess();
}

void PacketStore::DeInit() {
  // The metric source goes first: it reads dropped_packets_, and a source must
  // not outlive what it reads.
  dropped_metric_.Reset();
  if (flows_resource_) {
    // The flows may still be live: the module leaves with them. Released
    // before the binding and the resource go (arp_responder.cc's order), and
    // the release is checked: a failure would leave the engine holding a
    // registration for a resource this module is about to free.
    const std::string name = flows_resource_->name();
    auto released = init_context().resources().ReleaseForTeardown(
        std::span<const std::string>(&name, 1));
    CHECK(released) << released.error();
  }
  // The binding goes before the resource it binds (D-044).
  flows_binding_.Reset();
  flows_resource_.reset();

  // Teardown runs with the workers paused (destroying a module is structural),
  // so the published table can go without waiting for a grace period, and every
  // packet in flight to the owner is control's to free.
  EndTransaction();
  if (limits_.Read() != nullptr) {
    limits_.ResetQuiesced();
  }
  ClearStore();
  // What is still in flight to the owner: a partial burst is the common case,
  // so the count the ring returns is the count that is freed - a full-burst
  // free would free stale handles. The ring carries elements (handle and flow
  // id), so it is drained as elements.
  IngressPacket pkts[bess::PacketBatch::kMaxBurst];
  unsigned n = 0;
  while (ingress_ != nullptr &&
         (n = rte_ring_sc_dequeue_burst_elem(ingress_, pkts,
                                             sizeof(IngressPacket),
                                             bess::PacketBatch::kMaxBurst,
                                             nullptr)) > 0) {
    bess::PacketHandle handles[bess::PacketBatch::kMaxBurst];
    for (unsigned i = 0; i < n; i++) {
      handles[i] = pkts[i].handle;
    }
    bess::PacketFreeBulk(handles, static_cast<int>(n));
  }
  for (struct rte_ring **ring : {&ingress_, &requests_}) {
    if (*ring != nullptr) {
      std::free(*ring);
      *ring = nullptr;
    }
  }
  total_packets_.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The module's surface
// ---------------------------------------------------------------------------

CommandResponse PacketStore::CommandRelease(
    const bess::pb::PacketStoreCommandReleaseArg &arg) {
  const Request request{
      .kind = Request::Kind::kRelease,
      .id = arg.id(),
      .gate = static_cast<gate_idx_t>(arg.gate()),
  };
  if (!Deliver(request)) {
    return CommandFailure(ENOSPC, "the module's request ring is full");
  }
  return CommandSuccess();
}

CommandResponse PacketStore::CommandDrop(
    const bess::pb::PacketStoreCommandDropArg &arg) {
  const Request request{.kind = Request::Kind::kDrop, .id = arg.id()};
  if (!Deliver(request)) {
    return CommandFailure(ENOSPC, "the module's request ring is full");
  }
  return CommandSuccess();
}

CommandResponse PacketStore::CommandClear(
    const bess::pb::PacketStoreCommandClearArg &) {
  const Request request{.kind = Request::Kind::kClear};
  if (!Deliver(request)) {
    return CommandFailure(ENOSPC, "the module's request ring is full");
  }
  return CommandSuccess();
}

struct task_result PacketStore::RunTask(Context *ctx, bess::PacketBatch *batch,
                                       void *) {
  // The owner's three jobs, in one place: what the workers handed over, what
  // control asked for, and what was released and has to go out. A request must
  // not pass a packet that was in the ring before it, so the owner takes a
  // snapshot of both rings (the request count first, so a request posted after
  // it waits for a later run), stores what it owes the ingress at
  // kIngressPerRun a run, and only then applies the requests it saw. The budget
  // is what keeps the run bounded: emptying a 4095-slot ring in one run would
  // take about 128 batches from the task's pool (MAX_PBATCH_CNT), which every
  // downstream module in the same run also draws on.
  if (requests_due_ == 0 && requests_ != nullptr &&
      rte_ring_count(requests_) > 0) {
    const unsigned due = rte_ring_count(requests_);
    ingress_owed_ = ingress_ != nullptr ? rte_ring_count(ingress_) : 0;
    requests_due_ = due;
  }
  const uint32_t handed = DrainIngress(ctx, kIngressPerRun);
  ingress_owed_ = handed >= ingress_owed_ ? 0 : ingress_owed_ - handed;
  // What expired is freed whether or not the store is under pressure: a held
  // packet's lifetime is the flow's timeout, and the store's bound is not the
  // only thing that frees. The sweep is bounded, so this run stays a run.
  const LimitsMap *limits = limits_.Read();
  if (limits != nullptr) {
    EvictExpiredBounded(*limits, tsc_to_ns(current_worker.current_tsc()));
  }
  uint32_t applied = 0;
  if (ingress_owed_ == 0 && requests_due_ > 0) {
    applied = DrainRequests(requests_due_);
    requests_due_ = 0;
  }
  // An eviction goes out through EmitPacket, which defers the packet into the
  // task's per-gate batch and records the gate in the shared Context
  // (module.h:589). Task::operator() flushes those after ProcessBatch; a
  // module's own task must flush them itself, or the next module's
  // ProcessOGates would consume this module's gate indexes.
  ProcessOGates(ctx);

  uint32_t released = 0;
  if (!release_queue_.empty()) {
    const gate_idx_t out_gate = release_queue_.front().gate;
    batch->clear();
    while (!release_queue_.empty() &&
           release_queue_.front().gate == out_gate &&
           released < bess::PacketBatch::kMaxBurst) {
      batch->add(release_queue_.front().pkt);
      release_queue_.pop_front();
      released++;
    }
    if (released > 0) {
      RunChooseModule(ctx, out_gate, batch);
    }
  }

  // What a run did must be visible to the scheduler, but only what it *sends
  // downstream* counts as packets: a stored packet is charged to this traffic
  // class again when it is released (Queue and DRR count the same way), and a
  // rate limit on the owner's class must not throttle storing. Both schedulers
  // back off only on a run with no packets and block set, so block=false alone
  // is what keeps a working run from being counted as idle.
  const bool work = handed > 0 || released > 0 || applied > 0;
  return {.block = !work,
          .packets = released,
          .bits = static_cast<uint64_t>(released) * 64 * 8};
}

void PacketStore::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  (void)ctx;
  const int cnt = batch->cnt();
  if (cnt <= 0) {
    return;
  }
  // The key is read here, on the feeder's thread, and travels with the handle:
  // the offsets are this worker's, and a pause that reassigns them while a
  // packet is in flight must not change which flow it belongs to. Which of the
  // configured names is the key is fixed when the graph is compiled, so it is
  // resolved once per batch: the first one this graph provides.
  bess::metadata::mt_offset_t key_offset = bess::metadata::kMetadataOffsetNoRead;
  for (const int attr_id : key_attr_ids_) {
    const bess::metadata::mt_offset_t offset = attr_offset(attr_id);
    if (bess::metadata::IsValidOffset(offset)) {
      key_offset = offset;
      break;
    }
  }
  IngressPacket items[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    uint32_t flow_id = 0;
    if (bess::metadata::IsValidOffset(key_offset)) {
      flow_id = get_attr_with_offset<be32_t>(key_offset, pkt).value();
    }
    items[i] = IngressPacket{.handle = pkt.handle(), .flow_id = flow_id};
  }
  // Hand the batch to the owner. A batch that does not fit is freed here - the
  // module's bound is the store, and a full ingress ring means the owner is
  // behind, so the alternative would be unbounded memory. It is counted: this
  // is a loss, and a metric and GetDesc show it.
  const unsigned sent = rte_ring_mp_enqueue_burst_elem(
      ingress_, items, sizeof(IngressPacket), static_cast<unsigned>(cnt),
      nullptr);
  if (static_cast<int>(sent) < cnt) {
    const int refused = cnt - static_cast<int>(sent);
    bess::PacketFreeBulk(batch->handles() + sent, refused);
    ingress_dropped_.fetch_add(static_cast<uint64_t>(refused),
                               std::memory_order_relaxed);
  }
}

std::string PacketStore::GetDesc() const {
  const size_t ingress_dropped = ingress_dropped_.load(std::memory_order_relaxed);
  // Not const: the common case returns it, and a const local blocks the
  // implicit move (clang-tidy's performance-no-automatic-move).
  std::string base = bess::utils::Format(
      "%zu packets stored across %zu flows (max %u)",
      total_packets_.load(std::memory_order_relaxed),
      flows_held_.load(std::memory_order_relaxed), max_packets_);
  if (ingress_dropped == 0) {
    return base;
  }
  return base + bess::utils::Format(", %zu dropped (ingress full)",
                                    ingress_dropped);
}

ADD_MODULE(PacketStore, "packet_store",
           "Bounded, ownership-safe packet buffer for paging and pending queues")
