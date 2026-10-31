// Copyright (c) 2017, Joshua Stone.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "drr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "packet_cursor.h"
#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/rte_ring_alloc.h"
#include "utils/udp.h"

namespace {

// The flow and round-robin rings are single-producer/single-consumer (the
// AddQueue default): only the module's task touches them. The ingress ring is
// multi-producer (any upstream worker), single-consumer (the task).
// Decision D-019 (docs/decisions.md).
const unsigned kIngressRingFlags = RING_F_SC_DEQ;

// The one counter in DRR's task_runs_ set (drr.h): the module's task runs.
constexpr size_t kTaskRuns = 0;

// A handover invariant, checked in debug builds only: a packet handed to the
// module, or drained from the ingress ring, satisfies the field invariants
// every live mbuf has (a buffer address, at least one segment, the head's data
// within its buffer, data_len within pkt_len). A stale handle -- a pointer to
// memory the allocator has since reused, whose bytes read as the MALLOC_PERTURB_
// pattern -- is what turned the intermittent CI crash in DRR::GetId
// (live_classifier_traffic, 2026-10-15) into a SIGSEGV far from the write that
// caused it; this reports it at the boundary, with the ring's state.
//
// Deliberately not rte_mbuf_sanity_check() (which panics without this context)
// and not RTE_MBUF_DIRECT/nb_segs==1: an indirect (cloned) or multi-segment
// packet is legitimate here.
bool PlausiblePacket(bess::PacketHandle pkt) {
  return pkt != nullptr && pkt->buf_addr != nullptr && pkt->nb_segs >= 1 &&
         pkt->data_off <= pkt->buf_len && pkt->data_len <= pkt->pkt_len &&
         static_cast<uint32_t>(pkt->data_off) + pkt->data_len <= pkt->buf_len;
}

// The refusal a settings write reports for a quantum below 1 (nullptr when
// `size` is valid).
const char *QuantumError(uint32_t size) {
  return size == 0 ? "quantum size must be at least 1" : nullptr;
}

// The refusal a settings write reports for a max queue size below 1 (nullptr
// when `queue_size` is valid).
const char *QueueSizeError(uint32_t queue_size) {
  return queue_size == 0 ? "max queue size must be at least 1" : nullptr;
}

}  // namespace

uint32_t RoundToPowerTwo(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
}

// The setters edit what "settings" holds (DRRSettings); the class has no
// actions.
const Commands DRR::cmds = {};

const ResourceTables DRR::resource_tables = {
    ResourceTable::Singleton<bess::pb::DRRSettings>("settings")};

DRR::DRR()
    : quantum_(kDefaultQuantum),
      max_queue_size_(kFlowQueueMax),
      max_number_flows_(kDefaultNumFlows),
      ingress_(nullptr),
      flow_ring_(nullptr),
      current_flow_(nullptr) {
  is_task_ = true;
  max_allowed_workers_ = Worker::kMaxWorkers;
}

DRR::~DRR() {
  if (ingress_ != nullptr) {
    bess::PacketHandle pkt;
    while (rte_ring_sc_dequeue(ingress_, reinterpret_cast<void **>(&pkt)) ==
           0) {
      bess::PacketFree(pkt);
    }
    std::free(ingress_);
  }
  // The slab holds every queued packet (a free slot holds none).
  for (uint32_t slot = 0; slot < queue_slots_; slot++) {
    if (queued_[slot].pkt != nullptr) {
      bess::PacketFree(queued_[slot].pkt);
    }
  }
  std::free(flow_ring_);
}

CommandResponse DRR::Init(const bess::pb::DRRArg &arg) {
  CommandResponse err;
  task_id_t tid;

  // Wire ranges before narrowing: quantum is uint64 on the wire but stored
  // as uint32 (2^32 + 1 became 1), and num_flows + 1 overflowed
  // RoundToPowerTwo to 0 at UINT32_MAX.
  if (arg.quantum() > std::numeric_limits<uint32_t>::max()) {
    return CommandFailure(EINVAL, "quantum %llu is out of range",
                          static_cast<unsigned long long>(arg.quantum()));
  }
  if (arg.num_flows() > kMaxNumFlows) {
    return CommandFailure(EINVAL, "num_flows must be at most %u",
                          kMaxNumFlows);
  }
  if (arg.num_flows() != 0) {
    max_number_flows_ = RoundToPowerTwo(arg.num_flows() + 1);
  }

  if (arg.max_flow_queue_size() != 0) {
    err = SetMaxFlowQueueSize(arg.max_flow_queue_size());
    if (err.error().code() != 0) {
      return err;
    }
  }

  if (arg.quantum() != 0) {
    err = SetQuantumSize(static_cast<uint32_t>(arg.quantum()));
    if (err.error().code() != 0) {
      return err;
    }
  }

  // register task
  tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID) {
    return CommandFailure(ENOMEM, "task creation failed");
  }

  err = AllocateState();
  if (err.error().code() != 0) {
    return err;
  }

  // Applied live: both are relaxed atomics the task reads per flow round.
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::DRRSettings &v) -> std::expected<Settings, std::string> {
        if (const char *err = QuantumError(v.quantum())) {
          return std::unexpected(err);
        }
        if (const char *err = QueueSizeError(v.max_flow_queue_size())) {
          return std::unexpected(err);
        }
        return Settings{v.quantum(), v.max_flow_queue_size()};
      },
      [this](Settings &s) {
        quantum_.store(s.quantum, std::memory_order_relaxed);
        max_queue_size_.store(s.max_queue_size, std::memory_order_relaxed);
      },
      [this] {
        bess::pb::DRRSettings v;
        v.set_quantum(quantum_.load(std::memory_order_relaxed));
        v.set_max_flow_queue_size(max_queue_size_.load(std::memory_order_relaxed));
        return v;
      },
      nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  // The task's runs by worker (drr.h): ListMetrics reads it, and the live test
  // that moves the task between workers asserts the old worker's count stops.
  task_runs_metric_ = init_context().metrics().Register(
      [this](bess::stats::MetricWriter &writer) {
        const bess::stats::CounterSnapshot snap = task_runs_.Snapshot(true);
        for (size_t wid = 0; wid < bess::dataplane::kMaxWorkers; wid++) {
          const uint64_t runs = snap.PerWorker(
              bess::dataplane::WorkerId(static_cast<uint16_t>(wid)), kTaskRuns);
          if (runs == 0) {
            continue;  // a worker that never ran the task is not a series
          }
          WriteObservation(
              writer, "task_runs", runs,
              bess::dataplane::WorkerId(static_cast<uint16_t>(wid)));
        }
      });
  return CommandSuccess();
}

void DRR::DeInit() {
  task_runs_metric_.Reset();
  settings_.Release();
}

CommandResponse DRR::AllocateState() {
  // Both rings admitted before either is allocated (D-198).
  const size_t bytes = bess::utils::RingBytes(max_number_flows_) +
                       bess::utils::RingBytes(kIngressSize);
  rings_lease_ = init_context().memory().Lease("rings", bytes, bytes);
  int err_num = 0;
  flow_ring_ = AddQueue(max_number_flows_, &err_num);
  if (err_num != 0) {
    return CommandFailure(-err_num);
  }
  ingress_ = AddQueue(kIngressSize, &err_num, kIngressRingFlags);
  if (err_num != 0) {
    return CommandFailure(-err_num);
  }

  // The round-robin ring holds max_number_flows_ - 1 flows, each at most once:
  // the table holds as many, so a flow joining the ring always finds room.
  const size_t capacity = max_number_flows_ - 1;
  bess::framework::MemoryLease lease =
      init_context().memory().LeaseFor<FlowTable>("flows", capacity);
  auto table = FlowTable::Create(capacity);
  if (!table) {
    return CommandFailure(ENOMEM, "cannot create the flow table for %zu flows", capacity);
  }
  flows_ = bess::framework::MemoryScope::Adopt(std::move(lease), std::move(*table));

  queue_slots_ = static_cast<uint32_t>(std::min<uint64_t>(
      uint64_t{max_number_flows_} * kFlowQueueMax, kMaxQueuedPackets));
  const size_t queue_bytes = size_t{queue_slots_} * sizeof(Queued);
  queue_lease_ = init_context().memory().Lease("queue", queue_bytes, queue_bytes);
  queued_ = std::make_unique<Queued[]>(queue_slots_);
  for (uint32_t slot = 0; slot < queue_slots_; slot++) {
    queued_[slot].next = slot + 1;
  }
  free_ = 0;
  return CommandSuccess();
}

void DRR::ProcessBatch(Context *, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  // A batch larger than the framework's maximum, or a handle that is not a
  // live mbuf, is memory corruption upstream of here; PacketBatch::add() has
  // no bounds check, so this is the last place it can be caught (D-097's
  // debug-check convention: no cost in a release build).
  DCHECK_LE(cnt, static_cast<int>(bess::PacketBatch::kMaxBurst))
      << "DRR: a batch arrived with " << cnt << " packets";
  for (int i = 0; i < cnt; i++) {
    DCHECK(PlausiblePacket(batch->handles()[i]))
        << "DRR: packet " << i << " of the batch handed over (cnt " << cnt
        << ") is not a live mbuf";
  }
  const unsigned sent = rte_ring_mp_enqueue_burst(
      ingress_, reinterpret_cast<void *const *>(batch->handles()), cnt,
      nullptr);
  if (static_cast<int>(sent) < cnt) {
    bess::PacketFreeBulk(batch->handles() + sent, cnt - sent);
  }
}

void DRR::DrainIngress(uint32_t limit, uint64_t now) {
  bess::PacketHandle pkts[bess::PacketBatch::kMaxBurst];
  // D-019: one consumer at a time. Both the flag and the check are debug-only
  // (no cost in a release build): the task may move between workers while they
  // are paused, so what must never happen is two drains at once, not a second
  // worker.
#ifndef NDEBUG
  const bool already_draining =
      draining_.exchange(true, std::memory_order_acq_rel);
  DCHECK(!already_draining)
      << "DRR: two threads are draining the ingress ring (D-019: the task's "
         "worker is its only consumer)";
#endif
  while (limit > 0) {
    const unsigned n = rte_ring_sc_dequeue_burst(
        ingress_, reinterpret_cast<void **>(pkts),
        std::min<uint32_t>(limit, bess::PacketBatch::kMaxBurst), nullptr);
    for (unsigned i = 0; i < n; i++) {
      DCHECK(PlausiblePacket(pkts[i]))
          << "DRR: the ingress ring handed out packet " << i << " of " << n
             << " (value " << static_cast<const void *>(pkts[i])
             << ", ring count " << rte_ring_count(ingress_) << ")";
      Classify(pkts[i], now);
    }
    if (n < bess::PacketBatch::kMaxBurst) {
      break;
    }
    limit -= n;
  }
#ifndef NDEBUG
  draining_.store(false, std::memory_order_release);
#endif
}

void DRR::Classify(bess::PacketHandle handle, uint64_t now) {
  // TODO(joshua): Add support for fragmented packets.
  const FlowId id = GetId(bess::PacketRef(handle));
  Flow *f = flows_->Find(id);
  if (f == nullptr) {
    // A full table drops the packet. Nothing is allocated: the flow takes a
    // slot of the table made at Init (MA-B1).
    f = flows_->Emplace(id).state;
    if (f == nullptr) {
      bess::PacketFree(handle);
      return;
    }
    f->id = id;
  }
  Enqueue(f, handle, now);
}

struct task_result DRR::RunTask(Context *ctx, bess::PacketBatch *batch,
                                void *) {
  task_runs_.ForWorker(ctx->wid).Add(kTaskRuns, 1);
  if (children_overload_ > 0) {
    return {
        .block = true, .packets = 0, .bits = 0,
    };
  }

  const uint64_t now = ctx->current_ns;
  DrainIngress(kIngressPerRun, now);
  ExpireIdle(now);

  batch->clear();
  const uint32_t total_bytes = GetNextBatch(batch);
  if (total_bytes > 0) {
    RunNextModule(ctx, batch);
  }

  // the number of bits inserted into the packet batch
  uint32_t cnt = batch->cnt();
  uint64_t bits_retrieved = (total_bytes + cnt * kPacketOverhead) * 8;
  return {.block = (cnt == 0), .packets = cnt, .bits = bits_retrieved};
}

uint32_t DRR::GetNextBatch(bess::PacketBatch *batch) {
  uint32_t total_bytes = 0;
  uint32_t count = rte_ring_count(flow_ring_);
  if (current_flow_) {
    count++;
  }
  int batch_size = batch->cnt();

  // iterate through flows in round robin fashion until batch is full
  while (!batch->full()) {
    // checks to see if there has been no update after a full round
    // ensures that if every flow is empty or if there are no flows
    // that will terminate with a non-full batch.
    if (count == 0) {
      if (batch_size == batch->cnt()) {
        break;
      } else {
        count = rte_ring_count(flow_ring_);
        batch_size = batch->cnt();
      }
    }
    count--;

    Flow *f = GetNextFlow();
    if (f == nullptr) {
      break;  // no flow has a packet queued
    }
    total_bytes += GetNextPackets(batch, f);

    if (f->count == 0) {
      // Drained: the flow leaves the round robin until its next packet, so a
      // round visits the flows with packets only (MA-B2).
      f->deficit = 0;
      f->active = false;
    } else if (bess::PacketRef(queued_[f->head].pkt).total_len() > f->deficit) {
      // Its turn is over: to the back of the round robin. The ring holds every
      // flow at once, so this cannot fail.
      const int requeued = rte_ring_sp_enqueue(flow_ring_, f);
      DCHECK_EQ(0, requeued);
    } else {
      // knowing that the while statement will exit, keep the flow that still
      // has packets at the front
      current_flow_ = f;
    }
  }
  return total_bytes;
}

DRR::Flow *DRR::GetNextFlow() {
  Flow *f = current_flow_;
  if (f != nullptr) {
    current_flow_ = nullptr;
    return f;
  }
  if (rte_ring_sc_dequeue(flow_ring_, reinterpret_cast<void **>(&f)) != 0) {
    return nullptr;
  }
  f->deficit += quantum_.load(std::memory_order_relaxed);
  return f;
}

uint32_t DRR::GetNextPackets(bess::PacketBatch *batch, Flow *f) {
  uint32_t total_bytes = 0;
  while (!batch->full() && f->count > 0) {
    const uint32_t slot = f->head;
    Queued &q = queued_[slot];
    bess::PacketRef pkt(q.pkt);
    if (pkt.total_len() > f->deficit) {
      break;
    }
    f->deficit -= pkt.total_len();
    total_bytes += pkt.total_len();
    batch->add(pkt);
    // The slot goes back to the free list.
    f->head = q.next;
    f->count--;
    q.pkt = nullptr;
    q.next = free_;
    free_ = slot;
  }
  return total_bytes;
}

void DRR::ExpireIdle(uint64_t now) {
  constexpr uint64_t kTtlNs = uint64_t{kTtl} * 1'000'000'000;
  FlowId expired[kSweepSlots];
  size_t n = 0;
  sweep_cursor_ = flows_->VisitRange(
      sweep_cursor_, kSweepSlots,
      [now, &expired, &n](bess::flow::FlowHandle, const FlowId &id, const Flow &f) {
        // An idle flow is off the round robin and holds no packet.
        if (!f.active && now > f.last_ns + kTtlNs) {
          expired[n++] = id;
        }
      });
  for (size_t i = 0; i < n; i++) {
    (void)flows_->Erase(expired[i]);
  }
  if (sweep_cursor_ == flows_->capacity()) {
    sweep_cursor_ = 0;
  }
}

// The 5-tuple of an untagged IPv4 packet, read through a PacketCursor (bounds
// checked, chained packets included; contiguous headers take its fast path).
// Ports are read for TCP and UDP first fragments that carry them, and are 0
// otherwise. Anything else -- not IPv4, shorter than its headers, a header
// length below 5 -- maps to one fallback flow (all zeros), so DRR still
// schedules packets of any format, as documented, without reading past them.
DRR::FlowId DRR::GetId(bess::PacketRef pkt) {
  using bess::utils::Ipv4;
  using bess::utils::be16_t;

  FlowId id = {};
  bess::packet::PacketCursor cursor(pkt);
  const auto ether_type =
      cursor.Skip(12) ? cursor.Read<be16_t>() : std::nullopt;
  if (!ether_type || ether_type->value() != 0x0800) {
    return FlowId{};
  }
  const std::optional<Ipv4> ip = cursor.Read<Ipv4>();
  if (!ip || ip->version != 4 || ip->header_length < 5) {
    return FlowId{};
  }
  id.src_ip = ip->src.value();
  id.dst_ip = ip->dst.value();
  id.protocol = ip->protocol;
  const bool first_fragment = (ip->fragment_offset.value() & 0x1fff) == 0;
  if ((ip->protocol == 6 || ip->protocol == 17) && first_fragment &&
      cursor.Skip((ip->header_length - 5u) * 4u)) {
    const auto ports = cursor.Read<std::array<be16_t, 2>>();
    if (ports) {
      id.src_port = (*ports)[0].value();
      id.dst_port = (*ports)[1].value();
    }
  }
  return id;
}

rte_ring *DRR::AddQueue(uint32_t slots, int *err, unsigned flags) {
  ssize_t bytes = rte_ring_get_memsize(slots);
  if (bytes < 0) {
    *err = -EINVAL;
    return nullptr;
  }
  int ret;

  rte_ring *queue = static_cast<rte_ring *>(
      bess::utils::AllocRingMem(static_cast<size_t>(bytes)));
  if (!queue) {
    *err = -ENOMEM;
    return nullptr;
  }

  std::string name = bess::utils::NewRingName("drr");
  ret = rte_ring_init(queue, name.c_str(), slots, flags);
  if (ret) {
    std::free(queue);
    *err = -EINVAL;
    return nullptr;
  }
  return queue;
}

void DRR::Enqueue(Flow *f, bess::PacketHandle pkt, uint64_t now) {
  // A full flow queue, or a full slab, drops the packet.
  if (f->count >= max_queue_size_.load(std::memory_order_relaxed) ||
      free_ == queue_slots_) {
    bess::PacketFree(pkt);
    return;
  }
  const uint32_t slot = free_;
  free_ = queued_[slot].next;
  queued_[slot].pkt = pkt;
  if (f->count == 0) {
    f->head = slot;
  } else {
    queued_[f->tail].next = slot;
  }
  f->tail = slot;
  f->count++;
  f->last_ns = now;
  if (!f->active) {
    // Joins the round robin. The ring holds every flow at once, so this
    // cannot fail.
    f->active = true;
    const int joined = rte_ring_sp_enqueue(flow_ring_, f);
    DCHECK_EQ(0, joined);
  }
}

CommandResponse DRR::SetQuantumSize(uint32_t size) {
  if (const char *err = QuantumError(size)) {
    return CommandFailure(EINVAL, "%s", err);
  }

  quantum_.store(size, std::memory_order_relaxed);
  return CommandSuccess();
}

CommandResponse DRR::SetMaxFlowQueueSize(uint32_t queue_size) {
  if (const char *err = QueueSizeError(queue_size)) {
    return CommandFailure(EINVAL, "%s", err);
  }
  max_queue_size_.store(queue_size, std::memory_order_relaxed);
  return CommandSuccess();
}

ADD_MODULE(DRR, "DRR", "Deficit Round Robin")
