// Copyright (c) 2017, Joshua Stone.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_DRR_H_
#define BESS_MODULES_DRR_H_

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>

#include <rte_hash_crc.h>
#include <rte_ring.h>

#include "flow/worker_flow_table.h"
#include "framework/memory_scope.h"
#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "pktbatch.h"
#include "stats/counter_set.h"
#include "stats/metric_registry.h"
#include "utils/ip.h"

using bess::utils::Ipv4Prefix;

//
// This module implements Deficit Round Robin, a fair queueing algorithm, for
// flows. On receiving each packet, they are assigned to a flow's queue based
// on their 5 tuple. On running a task, the module goes through the flows in
// round robin fashion adding a quantum to their deficit(allocated bytes).
// The module will then query each of those flows for packets until their
// deficit falls below the next packet's size. After a obtaining a 32
// packets(a full batch), the module passes these packets onto the next module.
//
// based on this:
//  https://en.wikipedia.org/wiki/Deficit_round_robin
// EXPECTS: Input packets in any format. Flows are the IPv4 5-tuple
// (see GetId); packets that are not untagged, well-formed IPv4 share one
// fallback flow.
//
// MODIFICATIONS: None
//
// INPUT GATES: 1
//
// OUTPUT GATES: 1
//
// PARAMETERS:
//    * quantum: the number of bytes allocated to each flow on each round
//    * Max Number of flows: max number of flows the module will handle
//    * Max Flow Queue Size: the maximum size that any Flows queue can get
//          before the module will start dropping the flows packets
// COMMANDS
//    The "<name>/settings" resource (DRRSettings, D-129) applies live,
//    without pausing workers; the legacy commands
//    set_quantum_size and set_max_flow_queue_size went with the legacy
//    service (D-140).
//
// THREADING: every packet enters through an ingress ring that any number of
// upstream workers may feed; the module's task, on one worker, drains it and
// is the only reader and writer of the flow table and flow queues
// (worker-owned state; decision D-019, docs/decisions.md).
//
class DRR final : public Module {
 public:
  // the default max number of flows allowed + 1
  static const int kDefaultNumFlows = 4096;
  // Upper bound for the num_flows argument (the round-robin ring's size).
  static constexpr uint32_t kMaxNumFlows = uint32_t{1} << 24;
  // Packets queued in all flows at once, at most: the queue slab's size (the
  // smaller of this and num_flows x kFlowQueueMax). A full slab drops.
  static constexpr uint32_t kMaxQueuedPackets = uint32_t{1} << 18;
  static const int kFlowQueueMax =
      8192;                     // the max flow queue size if non-specified
  static const int kTtl = 300;  // time to live for flow entries
  static const int kDefaultQuantum =
      1500;  // default value to initialize qauntum_ to
  static const int kPacketOverhead =
      24;  // additional bytes associated with packets
  // Ingress ring slots: packets handed over by upstream workers and not yet
  // classified by the task. A full ingress ring drops.
  static const int kIngressSize = 4096;
  // At most this many ingress packets are classified per task run, so a
  // flood cannot starve scheduling.
  static const int kIngressPerRun = 1024;

  // 5 tuple id to identify a flow from a packet header information.
  struct FlowId {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t src_port;
    uint32_t dst_port;
    uint8_t protocol;
  };

  // A flow: its deficit, when it last queued a packet, and its queue, a FIFO
  // of slots in the module's queue slab (head first). Lives in the flow
  // table's slot; the slab owns the packets.
  struct Flow {
    uint32_t deficit = 0;      // the allocated bytes to the flow
    uint32_t count = 0;        // packets queued
    uint32_t head = 0;         // first queued slot (when count > 0)
    uint32_t tail = 0;         // last queued slot (when count > 0)
    bool active = false;       // on the round-robin ring, or current_flow_
    uint64_t last_ns = 0;      // when it last queued a packet
    FlowId id{};               // allows the flow to remove itself from the map
  };

  // hashes a FlowId: its fields' bytes, as the flow table's default hash
  // does for a key without padding (FlowId has some).
  struct Hash {
    uint64_t operator()(const FlowId &id) const noexcept {
      const uint64_t words[3] = {uint64_t{id.src_ip} << 32 | id.dst_ip,
                                 uint64_t{id.src_port} << 32 | id.dst_port,
                                 id.protocol};
      return bess::flow::detail::HashFixedBytes<sizeof(words)>(words);
    }
  };

  // to compare two FlowId for equality in a hash table
  struct EqualTo {
    bool operator()(const FlowId &id1, const FlowId &id2) const noexcept {
      bool ips = (id1.src_ip == id2.src_ip) && (id1.dst_ip == id2.dst_ip);
      bool ports =
          (id1.src_port == id2.src_port) && (id1.dst_port == id2.dst_port);
      return (ips && ports) && (id1.protocol == id2.protocol);
    }
  };

  DRR();   // constructor
  ~DRR();  // deconstructor

  static const Commands cmds;
  // "settings" (DRRSettings): quantum and max flow queue size, applied live
  // (D-129).
  static const ResourceTables resource_tables;
  // Any worker enqueues into the multi-producer ingress ring; one task drains it.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kShared,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  CommandResponse Init(const bess::pb::DRRArg &arg);
  void DeInit() override;

  // Any worker: hands the batch to the task through the ingress ring.
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;

 private:
  //  Sets the quantum: the number of bytes allocated to each flow on every
  //  round
  //  Takes the size to set the quantum to. Returns 0 on success and error value
  //  otherwise.
  CommandResponse SetQuantumSize(uint32_t size);

  //  Sets the maximum size that any Flows queue can get before the module will
  //  start dropping the flows packets. Takes the maximum size for any Flows
  //  queue.
  //  Returns 0 on success and error value otherwise.
  CommandResponse SetMaxFlowQueueSize(uint32_t queue_size);

  //  Appends the packet to the flow's queue, and puts the flow on the
  //  round-robin ring if it was idle. Drops the packet when the flow's queue is
  //  at the max flow queue size or the queue slab is full.
  void Enqueue(Flow *f, bess::PacketHandle pkt, uint64_t now);

  //  Moves up to `limit` packets from the ingress ring into their flows' queues
  //  (the task's worker only).
  void DrainIngress(uint32_t limit, uint64_t now);

  //  Puts one packet into its flow, creating the flow if needed (a full flow
  //  table drops the packet).
  void Classify(bess::PacketHandle pkt, uint64_t now);

  //  Takes a PacketRef to get a flow id for. Returns the 5 element identifier
  //  for the packet's flow.
  FlowId GetId(bess::PacketRef pkt);

  //  Removes the idle flows (not on the ring, nothing queued) older than kTtl
  //  among the next kSweepSlots slots of the flow table.
  void ExpireIdle(uint64_t now);

  //  Obtain the next batch of packets from the next flows in round robin.
  //  Takes a PacketBatch to insert the packets into. Returns total bytes
  //  added to batch.
  uint32_t GetNextBatch(bess::PacketBatch *batch);

  //  gets the next set of packets from flow given allocated bytes
  //  Takes the PacketBatch to put the packets into and the flow to get the
  //  packets from. Returns the total bytes put in batch
  uint32_t GetNextPackets(bess::PacketBatch *batch, Flow *f);

  //  gets the next flow from the round-robin ring of flows with packets, and
  //  adds a quantum to its deficit. Returns nullptr when the ring is empty.
  Flow *GetNextFlow();

  //  allocates ring space for `slots` entries. Sets the integer pointer on
  //  error. `flags` defaults to single-producer/single-consumer.
  rte_ring *AddQueue(uint32_t slots, int *err,
                     unsigned flags = RING_F_SP_ENQ | RING_F_SC_DEQ);

  friend class DrrTestPeer;

  // Admits (D-198) and allocates what the task uses: the round-robin and
  // ingress rings ("rings"), the flow table ("flows") and the queue slab
  // ("queue") (Init, after the task). A refusal throws MemoryRefused.
  CommandResponse AllocateState();

  // A queued packet: a slot of the queue slab, linked to the next one of its
  // flow (or of the free list).
  struct Queued {
    bess::PacketHandle pkt = nullptr;
    uint32_t next = 0;
  };
  // Flow table slots ExpireIdle visits per task run.
  static constexpr size_t kSweepSlots = 64;
  // The flow table: fixed at Init, nothing allocated or rehashed on the
  // worker (MA-B1). Unchecked ownership: the task may move between workers
  // while they are paused, and D-019 (draining_ below) already guarantees one
  // user at a time.
  struct FlowTableTraits : bess::flow::DefaultFlowTableTraits {
    using Owner = bess::flow::UncheckedOwner;
  };
  using FlowTable = bess::flow::WorkerFlowTable<FlowId, Flow, Hash, EqualTo, FlowTableTraits>;

  // the number of bytes to allocate to each flow in each round. Set by
  // commands, read by the task.
  std::atomic<uint32_t> quantum_;

  // max size of a flow's queue before the module will start dropping
  // the flow's packets. Set by commands, read by the task.
  std::atomic<uint32_t> max_queue_size_;

  // max number of flow's that the module will handle.
  uint32_t max_number_flows_;

  // The flows (max_number_flows_ - 1: what the round-robin ring holds),
  // admitted as "flows".
  bess::framework::Sized<FlowTable> flows_;
  rte_ring *ingress_;     // multi-producer, single-consumer: packets in.
  // The round robin: only the flows with packets queued (MA-B2), each once.
  rte_ring *flow_ring_;
  // The admission of flow_ring_ and ingress_ ("rings", D-198), released with
  // the module after ~DRR frees them.
  bess::framework::MemoryLease rings_lease_;
  // The queue slab ("queue"): every queued packet, linked per flow; free_
  // heads the free list (queue_slots_ when the slab is full).
  bess::framework::MemoryLease queue_lease_;
  std::unique_ptr<Queued[]> queued_;
  uint32_t queue_slots_ = 0;
  uint32_t free_ = 0;
  size_t sweep_cursor_ = 0;  // ExpireIdle's next flow table slot
  Flow *current_flow_;  // store current flow between batch rounds.

  // The D-019 ownership invariant, checked in debug builds: the task (its
  // worker owns the flow state and is the ingress ring's only consumer) never
  // has two drain loops running at once. A violated invariant is what makes a
  // ring hand out a slot nobody wrote (the intermittent live-test crash).
  // Per run, not per worker: a task may move between workers while they are
  // paused, and D-019 only requires one consumer at a time.
#ifndef NDEBUG
  std::atomic<bool> draining_{false};
#endif

  // Runs of the module's task, per worker, for ListMetrics. A task is placed on
  // one worker, so a second worker's count growing means the task is reachable
  // from two schedulers at once -- the state the D-019 invariant above
  // describes, which a release build (no assertion) can otherwise not see. Read
  // by the metric below, which is declared after it.
  //
  // A CounterSet (K6), not a plain array: the metric reads it on the control
  // thread while workers increment it, and CounterSet is the type that makes
  // that read safe (worker-local cells, a snapshot for the reader).
  bess::stats::CounterSet task_runs_{{"runs"}};
  bess::stats::MetricSource task_runs_metric_;

  struct Settings {
    uint32_t quantum;
    uint32_t max_queue_size;
  };
  // "<name>/settings"; after the state it sets.
  bess::framework::Singleton<bess::pb::DRRSettings, Settings> settings_;
};
#endif  // BESS_MODULES_DRR_H_
