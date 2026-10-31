// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_PACKET_STORE_H_
#define BESS_MODULES_PACKET_STORE_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "dataplane/packet_store.h"
#include "framework/memory_scope.h"
#include "framework/resource_bindings.h"
#include "module.h"
#include "stats/metric_registry.h"
#include "pb/module_msg.pb.h"
#include "utils/endian.h"
#include "utils/dpdk_memory.h"

// PacketStore: Bounded, ownership-safe packet buffer (K9).
// Buffers packets indexed by flow ID: the first of `key_attrs` that the graph
// provides, which by default is the metadata 'store_id' and then 'vif_id'.
// Supports bounded capacity, per-flow limits, aging eviction, and on-demand
// release/discard via commands. Per-flow limits are the "<module>/flows"
// resource (D-129), key PacketStoreFlowKey, value PacketStoreFlowValue.
//
// An adapter (D-201): the store, its limits and expiry are
// dataplane::PacketStore and the flows resource is
// dataplane::PacketStoreFlowsResource; this module adds the handoff to one
// owner, the command requests, the gates and the counters.
//
// Concurrency (D-129 amendment). The store has one owner: this module's own
// task, which the scheduler runs on one worker. Any worker may feed it - a
// batch reaching the module is handed to the owner over an MPSC ring, DRR's
// ingress shape - so `max_allowed_workers_` is no longer a pin, and the owner
// is the only thread that touches the store, the release queue or the counts.
// Control reaches the owner with a request on an SPSC ring, or applies it
// itself when no worker runs the module (then control owns the state, as a
// THREAD_UNSAFE command already assumes). The per-flow limits are a dense
// table of flow ids, each entry published on its own and read per packet.
// Nothing here pauses the dataplane, no command is refused with EBUSY, and no
// counter is shared between packet paths: the only shared state on a packet
// path is the ingress ring, which is what it is for (D-054's handoff shape).
// A command is accepted, not completed: with a worker running it is applied by
// the owner's next task run, in the order the client sent it, so a client that
// needs the state it changed reads it back (a count, a gate, a GetDesc). A full
// request ring is ENOSPC, which is retryable; a flow that holds nothing is not
// an error. A packet a worker hands over is the owner's to store, so the
// ingress ring's bound is the in-flight budget: what it refuses is freed and
// counted (the packet path never blocks on the store).
class PacketStore final : public Module {
 public:
  static const gate_idx_t kNumOGates = 2;  // 0: released, 1: evicted
  static const Commands cmds;
  static const ResourceTables resource_tables;
  // Every feeding worker enqueues to one MPSC ingress ring; the owner task
  // alone changes the store.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kShared,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  PacketStore() : Module() {
    // The module's own task is the owner, so the graph treats it as a task
    // boundary (as DRR and Queue do), and what it sends downstream runs on the
    // owner's worker - not on every worker that feeds it.
    is_task_ = true;
    propagate_workers_ = false;
    max_allowed_workers_ = Worker::kMaxWorkers;
  }
  ~PacketStore() override { DeInit(); }

  CommandResponse Init(const bess::pb::PacketStoreArg &arg);
  void DeInit() override;

  CommandResponse CommandRelease(
      const bess::pb::PacketStoreCommandReleaseArg &arg);
  CommandResponse CommandDrop(const bess::pb::PacketStoreCommandDropArg &arg);
  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;
  CommandResponse CommandClear(const bess::pb::PacketStoreCommandClearArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  std::string GetDesc() const override;

 private:
  using LimitsTable = bess::dataplane::PacketStoreLimitsTable;

  struct ReleasedPacket {
    bess::PacketRef pkt;
    gate_idx_t gate = 0;
  };

  // One control operation, posted to the owner. Fixed size and trivially
  // copyable: the ring copies bytes, and the element API stores the struct
  // itself rather than a pointer to it.
  struct Request {
    enum class Kind : uint8_t { kRelease, kDrop, kClear };

    Kind kind = Kind::kClear;
    uint32_t id = 0;      // flow id (release, drop)
    gate_idx_t gate = 0;  // release's target gate
  };
  static_assert(std::is_trivially_copyable_v<Request>,
                "a request is copied through the ring as bytes");
  static_assert(sizeof(Request) % 4 == 0,
                "ring elements are a multiple of 4 bytes");

  // Requests the owner's ring holds. One command is one request, so this is
  // headroom for a burst of commands, not a per-transaction budget.
  static constexpr unsigned kRequestSlots = 256;
  // Packets in flight to the owner, and the most one task run takes out
  // (DRR's ingress sizes: kIngressSize / kIngressPerRun).
  static constexpr unsigned kIngressSlots = 4096;
  static constexpr uint32_t kIngressPerRun = 1024;
  // Flow ids the flows table gives limits to when PacketStoreArg.flow_capacity
  // is 0: 1..65,536 (about 576 KiB of slots).
  static constexpr uint32_t kDefaultFlowCapacity = 65536;

  // The owner's state: the store (admitted as "store", D-198) and what was
  // released and waits to go out. Only the owner's task changes them; the
  // store's counts are relaxed loads control may make (GetDesc).
  bess::framework::Sized<bess::dataplane::PacketStore> store_;
  bess::framework::Sized<bess::utils::DpdkArray<ReleasedPacket>> release_queue_;
  uint32_t release_head_ = 0, release_count_ = 0;
  uint32_t max_packets_ = 4096;

  // Packets the `drop` command freed (the owner's count) and packets the
  // ingress ring refused because it was full (any worker's count: a batch had
  // nowhere to go). Shared observations keep the losses distinct:
  // requested_drop_packets is what a client asked for;
  // ingress_dropped_packets is the owner falling behind.
  // The first has one writer at a time (the owner, or control while no
  // worker runs), so both are plain stores; only the second is a real
  // multi-writer atomic.
  std::atomic<uint64_t> dropped_packets_{0};
  std::atomic<uint64_t> ingress_dropped_{0};
  bess::stats::MetricSource dropped_metric_;

  // The flow key: the attribute names to try in order (default: store_id, then
  // vif_id) and the ids AddMetadataAttr returned for them. The first one this
  // graph provides is the key, and the owner reads it from the packet.
  std::vector<std::string> key_attrs_;
  std::vector<int> key_attr_ids_;

  // Per-flow limits: flow ids 1..flow_capacity, each entry published by the
  // flows resource on its own (BP-6); the owner looks a flow up with one
  // acquire load. Declared before the resource, which refers to it.
  bess::framework::Sized<LimitsTable> limits_;

  // Packets in: many producers (every worker whose graph reaches the module),
  // one consumer (the owner's task). The element carries the flow id with the
  // handle, because the key is metadata the *feeder* has: the owner must file a
  // packet under the flow it belonged to when it was handed over, not under
  // whatever the offsets mean after a pause reassigned them.
  struct IngressPacket {
    bess::PacketHandle handle = 0;
    uint32_t flow_id = 0;
  };
  static_assert(std::is_trivially_copyable_v<IngressPacket>,
                "an ingress element is copied through the ring as bytes");
  static_assert(sizeof(IngressPacket) % 4 == 0,
                "ring elements are a multiple of 4 bytes");
  struct rte_ring *ingress_ = nullptr;
  // Control's requests: one producer (control - commands run under the
  // control-plane lock, so they are serialised), one consumer (the owner).
  struct rte_ring *requests_ = nullptr;
  // The admission of both rings ("rings", D-198), taken before Init allocates
  // them and released after DeInit frees them. The store is admitted
  // separately ("store").
  bess::framework::MemoryLease rings_lease_;

  // The owner's snapshot of both rings: how many requests were posted and how
  // many packets were in flight when it last looked. A request must not pass a
  // packet that was in the ring before it, so the owner stores `ingress_owed_`
  // packets first - at most kIngressPerRun a run, which also bounds the batches
  // an eviction takes from the task's pool - and applies `requests_due_`
  // requests once that reaches 0. Written by the owner's task and by control's
  // direct path only.
  uint32_t requests_due_ = 0;
  uint32_t ingress_owed_ = 0;

  // The `flows` resource and its binding: the binding is declared after the
  // resource it binds and reset before the resource is (D-044).
  bess::framework::Sized<bess::dataplane::PacketStoreFlowsResource> flows_resource_;
  bess::framework::ResourceBinding flows_binding_;

  // --- the owner's side (the module's task) --------------------------------

  // Takes what the ingress ring holds, up to `budget` packets, and stores it,
  // evicting to gate 1 as the limits say. Returns how many it took.
  uint32_t DrainIngress(Context *ctx, uint32_t budget);

  // Applies at most `max` posted requests in order. A release remains on the
  // ring until the fixed release queue has room for all held packets (a
  // conservative reservation). Later requests cannot pass a blocked release.
  uint32_t DrainRequests(uint32_t max);
  bool CanApply(const Request &request) const noexcept;

  // Applies one request to the store: the bodies the commands used to run
  // inline, now on the owner's thread (or on control, when no worker runs).
  void Apply(const Request &request);

  // Frees everything held, stored and queued for release.
  void ClearStore();

  // --- control's side -------------------------------------------------------

  // Posts `request` to the owner, or applies it directly when no worker runs
  // and nothing is in flight (after draining what is queued, so order is kept).
  // False only when the ring is full, which a client may retry.
  bool Deliver(const Request &request);
};

#endif  // BESS_MODULES_PACKET_STORE_H_
