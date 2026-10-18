// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_PACKET_STORE_H_
#define BESS_MODULES_PACKET_STORE_H_

#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "dataplane/resource.h"
#include "framework/resource_bindings.h"
#include "module.h"
#include "rcu/rcu_ptr.h"
#include "stats/metric_registry.h"
#include "pb/module_msg.pb.h"
#include "utils/endian.h"

// PacketStore: Bounded, ownership-safe packet buffer (K9).
// Buffers packets indexed by flow ID: the first of `key_attrs` that the graph
// provides, which by default is the metadata 'store_id' and then 'vif_id'.
// Supports bounded capacity, per-flow limits, aging eviction, and on-demand
// release/discard via commands. Per-flow limits are the "<module>/flows"
// resource (D-129), key PacketStoreFlowKey, value PacketStoreFlowValue.
//
// Concurrency (D-129 amendment). The store has one owner: this module's own
// task, which the scheduler runs on one worker. Any worker may feed it - a
// batch reaching the module is handed to the owner over an MPSC ring, DRR's
// ingress shape - so `max_allowed_workers_` is no longer a pin, and the owner
// is the only thread that touches the store, the release queue or the counts.
// Control reaches the owner with a request on an SPSC ring, or applies it
// itself when no worker runs the module (then control owns the state, as a
// THREAD_UNSAFE command already assumes). The per-flow limits are one
// immutable table published through an RcuPtr and read once per task run.
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

  struct StoredPacket {
    bess::PacketRef pkt;
    uint64_t timestamp_ns = 0;
  };

  struct ReleasedPacket {
    bess::PacketRef pkt;
    gate_idx_t gate = 0;
  };

  PacketStore() : Module(), limits_(init_context().rcu()) {
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
  // The module's `flows` resource.
  class FlowsResource;
  friend class FlowsResource;

  // One flow's effective limits, as its `flows` entry gives them: an absent
  // field leaves the module-wide argument in force, and timeout_ns present as
  // 0 means this flow's packets never expire.
  struct FlowLimits {
    std::optional<uint64_t> timeout_ns;
    std::optional<uint32_t> max_packets_per_flow;
  };
  using LimitsMap = std::map<uint32_t, FlowLimits>;

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

  // The owner's state. Only the owner's task touches the container; the two
  // counts are relaxed atomics because control reads them (GetDesc, metrics)
  // while the owner writes them - one writer, so no contention.
  std::map<uint32_t, std::deque<StoredPacket>> store_;
  std::deque<ReleasedPacket> release_queue_;
  std::atomic<size_t> total_packets_{0};
  std::atomic<size_t> flows_held_{0};

  uint32_t max_packets_ = 4096;
  uint32_t max_packets_per_flow_ = 64;
  uint64_t timeout_ns_ = 5'000'000'000ULL;  // 5 seconds

  // Flows one run's expiry sweep looks at (see EvictExpiredBounded): a run's
  // cost is this, whatever the store holds. Expiry is honoured within one
  // rotation, which is this many runs.
  static constexpr size_t kExpireFlowsPerRun = 256;
  // Where the last sweep stopped, so the next one resumes there. Only the
  // owner touches it.
  uint32_t expire_cursor_ = 0;

  // Packets the `drop` command freed (the owner's count) and packets the
  // ingress ring refused because it was full (any worker's count: a batch had
  // nowhere to go). Each has its own metric, because each is a different loss:
  // bess_packet_store_dropped_packets_total is what a client asked for,
  // bess_packet_store_ingress_dropped_packets_total is the owner falling
  // behind. The first has one writer at a time (the owner, or control while no
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

  // Per-flow limits: one immutable table, published by control and read by the
  // owner once per task run (one acquire load, valid for that run).
  bess::rcu::RcuPtr<LimitsMap> limits_;
  // The transaction's private copy (meter.cc's staging): every operation of a
  // transaction edits this one table and the first publish swaps it in, so a
  // transaction of many operations is one copy and one grace period - and no
  // operation can publish over another's edit.
  std::unique_ptr<LimitsMap> staged_limits_;
  bool published_staged_ = false;

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
  std::unique_ptr<bess::dataplane::Resource> flows_resource_;
  bess::framework::ResourceBinding flows_binding_;

  // --- the owner's side (the module's task) --------------------------------

  // The flow's timeout and per-flow cap: the `flows` entry where it gives
  // them, the module-wide arguments otherwise. A cap of 0 in an entry means
  // the module-wide per-flow default; a timeout of 0 means no expiry.
  std::pair<uint64_t, uint32_t> LimitsFor(const LimitsMap &limits,
                                          uint32_t flow_id) const;

  // Takes what the ingress ring holds, up to `budget` packets, and stores it,
  // evicting to gate 1 as the limits say. Returns how many it took.
  uint32_t DrainIngress(Context *ctx, uint32_t budget);

  // Stores one batch with the flow ids the feeders read: the per-flow caps, the
  // module-wide bound and the aging eviction, as the module always did - now on
  // the owner's thread.
  void StoreBatch(Context *ctx, bess::PacketBatch *batch,
                  const uint32_t *flow_ids, const LimitsMap &limits,
                  uint64_t now_ns);

  // Applies at most `max` posted requests, in order. Returns how many it
  // applied: the ring may hold more, and a request posted after the run's
  // snapshot waits for a later run.
  uint32_t DrainRequests(uint32_t max);

  // Applies one request to the store: the bodies the commands used to run
  // inline, now on the owner's thread (or on control, when no worker runs).
  void Apply(const Request &request);

  // Frees the store's expired packets, by the limits in force for each flow.
  void EvictExpired(const LimitsMap &limits, uint64_t now_ns);

  // Frees expired packets without pressure, at a bounded cost per run: the
  // sweep walks kExpireFlowsPerRun flows from `expire_cursor_` and resumes
  // where it stopped, so a run's cost does not grow with the store's size (a
  // full sweep per run is what this replaces: with 100K flows it would be the
  // run's whole budget) and a flow is visited once per rotation.
  void EvictExpiredBounded(const LimitsMap &limits, uint64_t now_ns);

  // Frees everything the store holds, stored and queued.
  void ClearStore();

  // --- control's side -------------------------------------------------------

  // Posts `request` to the owner, or applies it directly when no worker runs
  // and nothing is in flight (after draining what is queued, so order is kept).
  // False only when the ring is full, which a client may retry.
  bool Deliver(const Request &request);

  // Starts the transaction's private copy of the limits table, if this is its
  // first operation.
  void BeginTransaction();
  // Publishes the private copy, once per transaction, retiring the table it
  // replaced against the retirer's grace period.
  void PublishStaged(bess::dataplane::Retirer &retirer);
  // Drops the private copy at the end of a transaction (meter.cc:334).
  void EndTransaction() noexcept;
};

#endif  // BESS_MODULES_PACKET_STORE_H_
