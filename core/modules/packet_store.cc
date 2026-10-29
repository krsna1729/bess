// SPDX-License-Identifier: BSD-3-Clause

#include "packet_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <rte_ring_peek.h>

#include "framework/packet_store_codec.h"
#include "dataplane/resource_registry.h"
#include "utils/format.h"
#include "utils/rte_ring_alloc.h"
#include "utils/time.h"

namespace dataplane = bess::dataplane;
namespace stats = bess::stats;

namespace {

using bess::utils::be32_t;

// A ring in caller-owned memory, the pattern Queue and DRR share (Queue's
// AllocRing, drr.cc:514): rte_ring_create would allocate an EAL memzone, which
// the malloc-pool module tests have no hugepages for. `bytes` is
// utils::RingElemBytes for the ring (0: DPDK refuses the size); the
// pointer-ring size (rte_ring_get_memsize) is for 8-byte slots and would
// under-size an element ring.
struct rte_ring *AllocRing(uint32_t slots, const char *prefix, unsigned flags,
                           size_t bytes) {
  if (bytes == 0) {
    return nullptr;
  }
  struct rte_ring *ring =
      static_cast<rte_ring *>(bess::utils::AllocRingMem(bytes));
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

uint32_t PacketStore::DrainIngress(Context *ctx, uint32_t budget) {
  const LimitsMap *limits = limits_.Read();
  const uint64_t now_ns = tsc_to_ns(current_worker.current_tsc());
  IngressPacket pkts[bess::PacketBatch::kMaxBurst];
  uint32_t taken = 0;
  // An eviction (a flow at its cap, or the store full) leaves on gate 1.
  auto evict = [this, ctx](bess::PacketHandle handle) {
    EmitPacket(ctx, bess::PacketRef(handle), 1);
  };

  while (budget > 0) {
    const unsigned n = rte_ring_sc_dequeue_burst_elem(
        ingress_, pkts, sizeof(IngressPacket),
        std::min<uint32_t>(budget, bess::PacketBatch::kMaxBurst), nullptr);
    if (n == 0) {
      break;
    }
    bess::PacketHandle handles[bess::PacketBatch::kMaxBurst];
    uint32_t flow_ids[bess::PacketBatch::kMaxBurst];
    for (unsigned i = 0; i < n; i++) {
      handles[i] = pkts[i].handle;
      flow_ids[i] = pkts[i].flow_id;
    }
    store_->Store(handles, flow_ids, n, *limits, now_ns, evict);
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
    case Request::Kind::kRelease:
      store_->Release(request.id, [this, &request](bess::PacketHandle handle) {
        CHECK_LT(release_count_, max_packets_);
        uint32_t tail = release_head_ + release_count_;
        if (tail >= max_packets_) tail -= max_packets_;
        (*release_queue_)[tail] = ReleasedPacket{
            .pkt = bess::PacketRef(handle), .gate = request.gate};
        ++release_count_;
      });
      break;
    case Request::Kind::kDrop: {
      const size_t dropped = store_->Drop(request.id);
      dropped_packets_.store(
          dropped_packets_.load(std::memory_order_relaxed) + dropped,
          std::memory_order_relaxed);
      break;
    }
    case Request::Kind::kClear:
      ClearStore();
      break;
  }
}

bool PacketStore::CanApply(const Request &request) const noexcept {
  return request.kind != Request::Kind::kRelease ||
         max_packets_ - release_count_ >= store_->packets();
}

uint32_t PacketStore::DrainRequests(uint32_t max) {
  Request request;
  uint32_t applied = 0;
  while (applied < max && requests_ != nullptr &&
         rte_ring_dequeue_bulk_elem_start(requests_, &request, sizeof(Request), 1, nullptr) != 0) {
    if (!CanApply(request)) {
      rte_ring_dequeue_elem_finish(requests_, 0);
      break;
    }
    rte_ring_dequeue_elem_finish(requests_, 1);
    Apply(request);
    applied++;
  }
  return applied;
}

void PacketStore::ClearStore() {
  if (store_) {
    store_->Clear();
  }
  while (release_count_ != 0) {
    bess::PacketFree((*release_queue_)[release_head_].pkt.handle());
    if (++release_head_ == max_packets_) release_head_ = 0;
    --release_count_;
  }
  release_head_ = 0;
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
    // Drain in order; a blocked release stays pending even on the direct path.
    ingress_owed_ = 0;
    const uint32_t pending = requests_ != nullptr ? rte_ring_count(requests_) : 0;
    requests_due_ = pending - DrainRequests(pending);
    if (requests_due_ == 0 && CanApply(request)) {
      Apply(request);
      return true;
    }
  }
  Request copy = request;
  return rte_ring_sp_enqueue_elem(requests_, &copy, sizeof(Request)) == 0;
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

CommandResponse PacketStore::Init(const bess::pb::PacketStoreArg &arg) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  max_packets_ = arg.max_packets() ? arg.max_packets() : 4096;
  // Held, released-but-not-emitted, and ingress packets can coexist. Both
  // fixed queues must fit the packet pool, not just the held store.
  if (const bess::PacketPool *pool = bess::PacketPool::GetDefaultPool();
      pool != nullptr &&
      pool->Capacity() <
          size_t{2} * max_packets_ + size_t{kIngressSlots}) {
    return CommandFailure(
        EINVAL,
        "twice max_packets %u plus the %u the ingress ring can hold exceeds "
        "the packet pool's %zu buffers",
        max_packets_, kIngressSlots, pool->Capacity());
  }
  bess::dataplane::PacketStoreConfig config;
  config.max_packets = max_packets_;
  config.max_packets_per_flow =
      arg.max_packets_per_flow() ? arg.max_packets_per_flow() : 64;
  config.timeout_ns =
      arg.timeout_ms() ? static_cast<uint64_t>(arg.timeout_ms()) * 1'000'000ULL
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
    // These are alternatives: use the first bound key, or flow 0 if none is
    // provided. An absent alternative must not invalidate the graph.
    const int attr_id =
        AddMetadataAttr(name, sizeof(be32_t), AccessMode::kRead,
                        bess::metadata::Attribute::Contract::kBestEffort);
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
  // only the dequeue is flagged. Both rings are admitted before either is
  // allocated (D-198; a refusal throws MemoryRefused).
  const size_t ingress_bytes =
      bess::utils::RingElemBytes(sizeof(IngressPacket), kIngressSlots);
  const size_t request_bytes =
      bess::utils::RingElemBytes(sizeof(Request), kRequestSlots);
  rings_lease_ = init_context().memory().Lease(
      "rings", ingress_bytes + request_bytes, ingress_bytes + request_bytes);
  ingress_ = AllocRing(kIngressSlots, "packet_store_ingress", RING_F_SC_DEQ,
                       ingress_bytes);
  if (ingress_ == nullptr) {
    return CommandFailure(ENOMEM, "the ingress ring could not be created");
  }
  requests_ = AllocRing(kRequestSlots, "packet_store_requests",
                        RING_F_SP_ENQ | RING_F_SC_DEQ, request_bytes);
  if (requests_ == nullptr) {
    return CommandFailure(ENOMEM, "the request ring could not be created");
  }

  // The store: every slot and flow entry it will ever use, admitted before it
  // is allocated (D-198; a refusal throws MemoryRefused).
  bess::framework::MemoryLease store_lease =
      init_context().memory().LeaseFor<dataplane::PacketStore>("store", config);
  auto created = dataplane::PacketStore::Create(config);
  if (!created) {
    return CommandFailure(EINVAL, "%s", created.error().c_str());
  }
  if (*created == nullptr) {
    return CommandFailure(ENOMEM, "the packet store could not be allocated");
  }
  store_ = bess::framework::MemoryScope::Adopt(std::move(store_lease),
                                               std::move(*created));
  release_queue_ = init_context().memory().Create<
      bess::utils::DpdkArray<ReleasedPacket>>("release_queue", max_packets_);

  // No flow has limits of its own yet: every flow takes the module-wide
  // numbers until an entry says otherwise. The resource publishes that first
  // empty table before any worker can read it (Init runs before the module is
  // attached), and admits it and every later map through the lease (D-198;
  // a refusal throws MemoryRefused).
  //
  // The per-flow limits (D-129). The binding is declared after the resource it
  // binds and reset before it is (D-044). No worker_running callback: the
  // resource does not require paused workers - the owner reads the published
  // table once per task run, and a transaction publishes a replacement.
  bess::framework::MemoryLease flows_lease =
      init_context().memory().LeaseFor<dataplane::PacketStoreFlowsResource>(
          "flows");
  auto flows = std::make_unique<dataplane::PacketStoreFlowsResource>(
      name() + "/flows", limits_, flows_lease.growth());
  flows_resource_ = bess::framework::MemoryScope::Adopt(std::move(flows_lease),
                                                        std::move(flows));
  flows_binding_ = init_context().codecs().Bind(
      *flows_resource_, dataplane::MakePacketStoreFlowsCodec(limits_));

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
        WriteObservation(writer, "requested_drop_packets", dropped_packets_.load());
        // A second loss, and a different one: a worker's batch the ingress ring
        // had no room for, which the module freed rather than holding. Before
        // this change an overflow of the store went out on gate 1; a batch the
        // ring refuses cannot (downstream runs on the owner's worker only), so
        // it is freed and counted here instead.
        WriteObservation(writer, "ingress_dropped_packets", ingress_dropped_.load());
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
  // packet in flight to the owner is control's to free. (The resource, and any
  // transaction copy it staged, went above.)
  if (limits_.Read() != nullptr) {
    limits_.ResetQuiesced();
  }
  ClearStore();
  release_queue_.reset();
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
  rings_lease_.reset();
  // The store is empty now; it goes, then its admission.
  store_.reset();
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
  // Once the ingress snapshot is satisfied, leave later arrivals queued until
  // all its requests complete, including releases blocked on output capacity.
  const uint32_t budget = requests_due_ != 0
      ? std::min(kIngressPerRun, ingress_owed_) : kIngressPerRun;
  const uint32_t handed = DrainIngress(ctx, budget);
  ingress_owed_ = handed >= ingress_owed_ ? 0 : ingress_owed_ - handed;
  // What expired is freed whether or not the store is under pressure: a held
  // packet's lifetime is the flow's timeout, and the store's bound is not the
  // only thing that frees. The sweep is bounded, so this run stays a run.
  const LimitsMap *limits = limits_.Read();
  if (limits != nullptr) {
    store_->ExpireBounded(*limits, tsc_to_ns(current_worker.current_tsc()));
  }
  uint32_t applied = 0;
  if (ingress_owed_ == 0 && requests_due_ > 0) {
    applied = DrainRequests(requests_due_);
    requests_due_ -= applied;
  }
  // An eviction goes out through EmitPacket, which defers the packet into the
  // task's per-gate batch and records the gate in the shared Context
  // (module.h:589). Task::operator() flushes those after ProcessBatch; a
  // module's own task must flush them itself, or the next module's
  // ProcessOGates would consume this module's gate indexes.
  ProcessOGates(ctx);

  uint32_t released = 0;
  if (release_count_ != 0) {
    const gate_idx_t out_gate = (*release_queue_)[release_head_].gate;
    batch->clear();
    while (release_count_ != 0 &&
           (*release_queue_)[release_head_].gate == out_gate &&
           released < bess::PacketBatch::kMaxBurst) {
      batch->add((*release_queue_)[release_head_].pkt);
      if (++release_head_ == max_packets_) release_head_ = 0;
      --release_count_;
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
      store_ ? store_->packets() : size_t{0},
      store_ ? store_->flows() : size_t{0}, max_packets_);
  if (ingress_dropped == 0) {
    return base;
  }
  return base + bess::utils::Format(", %zu dropped (ingress full)",
                                    ingress_dropped);
}

ADD_MODULE(PacketStore, "packet_store",
           "Bounded, ownership-safe packet buffer for paging and pending queues")
