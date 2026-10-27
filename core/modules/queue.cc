// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "queue.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "utils/format.h"
#include "utils/rte_ring_alloc.h"

#define DEFAULT_QUEUE_SIZE 1024

// "settings" (QueueSettings) holds size, burst, prefetch and backpressure;
// `get_status` is the class's one action.
const Commands Queue::cmds = {
    {"get_status", "QueueCommandGetStatusArg",
     MODULE_CMD_FUNC(&Queue::CommandGetStatus), Command::THREAD_SAFE,
     "QueueCommandGetStatusResponse"},
};

const ResourceTables Queue::resource_tables = {
    ResourceTable::Singleton<bess::pb::QueueSettings>("settings", true),
};

void Queue::RingFree::operator()(struct rte_ring *ring) const {
  bess::PacketHandle pkt;
  while (rte_ring_sc_dequeue(ring, reinterpret_cast<void **>(&pkt)) == 0) {
    bess::PacketFree(pkt);
  }
  std::free(ring);
}

std::expected<Queue::RingPtr, int> Queue::AllocRing(uint64_t slots) {
  ssize_t bytes = rte_ring_get_memsize(slots);
  if (bytes < 0) {
    return std::unexpected(EINVAL);
  }

  struct rte_ring *ring = static_cast<rte_ring *>(
      bess::utils::AllocRingMem(static_cast<size_t>(bytes)));
  if (!ring) {
    return std::unexpected(ENOMEM);
  }

  std::string name = bess::utils::NewRingName("queue");
  if (rte_ring_init(ring, name.c_str(), slots, RING_F_SC_DEQ)) {
    std::free(ring);
    return std::unexpected(EINVAL);
  }
  return RingPtr(ring);
}

void Queue::MigrateRing(struct rte_ring *from, struct rte_ring *to) {
  bess::PacketHandle pkt;
  while (rte_ring_sc_dequeue(from, reinterpret_cast<void **>(&pkt)) == 0) {
    if (rte_ring_sp_enqueue(to, reinterpret_cast<void *>(pkt)) == -ENOBUFS) {
      bess::PacketFree(pkt);
    }
  }
}

int Queue::Resize(int slots) {
  auto ring = AllocRing(slots);
  if (!ring) {
    return -ring.error();
  }

  /* migrate packets from the old queue */
  if (queue_) {
    MigrateRing(queue_, ring->get());
    std::free(queue_);
  }

  queue_ = ring->release();
  size_ = slots;

  if (backpressure_) {
    AdjustWaterLevels();
  }

  return 0;
}

std::expected<void, std::string> Queue::ValidSize(uint64_t size) {
  if (size < 4 || size > 16384) {
    return std::unexpected("must be in [4, 16384]");
  }
  if (size & (size - 1)) {
    return std::unexpected("must be a power of 2");
  }
  return {};
}

// As set_size and set_burst validate; the ring of a new size is allocated
// here, so apply only swaps.
std::expected<Queue::Staged, std::string> Queue::StageSettings(
    const bess::pb::QueueSettings &value) const {
  if (auto valid = ValidSize(value.size()); !valid) {
    return std::unexpected(valid.error());
  }
  if (value.burst() > bess::PacketBatch::kMaxBurst) {
    return std::unexpected(bess::utils::Format(
        "burst size must be [0,%zu]", bess::PacketBatch::kMaxBurst));
  }
  Staged staged;
  if (value.size() != size_) {
    auto ring = AllocRing(value.size());
    if (!ring) {
      return std::unexpected(
          bess::utils::Format("cannot allocate the queue: %s",
                              strerror(ring.error())));
    }
    staged.ring = std::move(*ring);
  }
  staged.size = value.size();
  staged.burst = static_cast<int>(value.burst());
  staged.prefetch = value.prefetch();
  staged.backpressure = value.backpressure();
  return staged;
}

// Workers are paused. The old ring goes back into `staged`, freed (empty)
// with it after the publish.
void Queue::ApplySettings(Staged &staged) {
  if (staged.ring != nullptr) {
    MigrateRing(queue_, staged.ring.get());
    struct rte_ring *old = queue_;
    queue_ = staged.ring.release();
    staged.ring.reset(old);
  }
  size_ = staged.size;
  burst_.store(staged.burst, std::memory_order_relaxed);
  prefetch_ = staged.prefetch;
  backpressure_ = staged.backpressure;
  AdjustWaterLevels();
}

bess::pb::QueueSettings Queue::ReadSettings() const {
  bess::pb::QueueSettings value;
  value.set_size(size_);
  value.set_burst(burst_.load(std::memory_order_relaxed));
  value.set_prefetch(prefetch_);
  value.set_backpressure(backpressure_);
  return value;
}

CommandResponse Queue::Init(const bess::pb::QueueArg &arg) {
  task_id_t tid;
  CommandResponse err;

  full_ = bess::stats::EventThrottle(init_context().events(), "bess.queue_full", name());

  tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID) {
    return CommandFailure(ENOMEM, "Task creation failed");
  }

  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);

  if (arg.backpressure()) {
    VLOG(1) << "Backpressure enabled for " << name() << "::Queue";
    backpressure_ = true;
  }

  if (arg.size() != 0) {
    err = SetSize(arg.size());
    if (err.error().code() != 0) {
      return err;
    }
  } else {
    int ret = Resize(DEFAULT_QUEUE_SIZE);
    if (ret) {
      return CommandFailure(-ret);
    }
  }

  if (arg.prefetch()) {
    prefetch_ = true;
  }

  init_arg_ = arg;

  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [this](const bess::pb::QueueSettings &value) {
        return StageSettings(value);
      },
      [this](Staged &staged) { ApplySettings(staged); },
      [this] { return ReadSettings(); }, [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  observations_ = init_context().metrics().Register(
      [this](bess::stats::MetricWriter &writer) {
        WriteObservation(writer, "enqueued_packets",
                         stats_.enqueued.load(std::memory_order_relaxed));
        WriteObservation(writer, "dequeued_packets",
                         stats_.dequeued.load(std::memory_order_relaxed));
        WriteObservation(writer, "queue_full_packets",
                         stats_.dropped.load(std::memory_order_relaxed));
      });
  return CommandSuccess();
}

void Queue::DeInit() {
  observations_.Reset();
  settings_.Release();

  bess::PacketHandle pkt;
  if (queue_) {
    while (rte_ring_sc_dequeue(queue_,
                               reinterpret_cast<void **>(&pkt)) == 0) {
      bess::PacketFree(pkt);
    }
    std::free(queue_);
  }
}
int Queue::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  const size_t workers = num_active_workers();
  if (workers <= 1) {
    enqueue_fn_ = &rte_ring_sp_enqueue_burst;
  } else {
    enqueue_fn_ = &rte_ring_mp_enqueue_burst;
  }
  return 0;
}


std::string Queue::GetDesc() const {
  const struct rte_ring *ring = queue_;

  return bess::utils::Format(
      "%u/%u (%s)", rte_ring_count(ring), rte_ring_get_size(ring),
      (enqueue_fn_ == &rte_ring_sp_enqueue_burst) ? "SP" : "MP");
}

/* from upstream */
void Queue::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int queued = static_cast<int>(enqueue_fn_(
      queue_, reinterpret_cast<void **>(batch->handles()), batch->cnt(),
      nullptr));
  if (backpressure_ && rte_ring_count(queue_) > high_water_) {
    SignalOverload();
  }

  stats_.enqueued.fetch_add(queued, std::memory_order_relaxed);

  if (queued < batch->cnt()) {
    int to_drop = batch->cnt() - queued;
    stats_.dropped.fetch_add(to_drop, std::memory_order_relaxed);
    bess::PacketFreeBulk(batch->handles() + queued, to_drop);
    full_.Note(ctx->wid, ctx->current_ns, static_cast<uint64_t>(to_drop));
  }
}

/* to downstream */
struct task_result Queue::RunTask(Context *ctx, bess::PacketBatch *batch,
                                  void *) {
  if (children_overload_ > 0) {
    return {
        .block = true,
        .packets = 0,
        .bits = 0,
    };
  }

  const int burst = burst_.load(std::memory_order_relaxed);
  const int pkt_overhead = 24;

  uint64_t total_bytes = 0;

  uint32_t cnt = static_cast<uint32_t>(rte_ring_sc_dequeue_burst(
      queue_, reinterpret_cast<void **>(batch->handles()), burst, nullptr));

  if (cnt == 0) {
    return {.block = true, .packets = 0, .bits = 0};
  }

  stats_.dequeued.fetch_add(cnt, std::memory_order_relaxed);
  batch->set_cnt(cnt);

  if (prefetch_) {
    for (uint32_t i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      total_bytes += pkt.total_len();
      rte_prefetch0(pkt.head_data());
    }
  } else {
    for (uint32_t i = 0; i < cnt; i++) {
      total_bytes += batch->packet(i).total_len();
    }
  }

  RunNextModule(ctx, batch);

  if (backpressure_ && rte_ring_count(queue_) < low_water_) {
    SignalUnderload();
  }

  return {.block = false,
          .packets = cnt,
          .bits = (total_bytes + cnt * pkt_overhead) * 8};
}

CommandResponse Queue::SetSize(uint64_t size) {
  if (auto valid = ValidSize(size); !valid) {
    return CommandFailure(EINVAL, "%s", valid.error().c_str());
  }

  int ret = Resize(size);
  if (ret) {
    return CommandFailure(-ret);
  }

  return CommandSuccess();
}

CommandResponse Queue::CommandGetStatus(
    const bess::pb::QueueCommandGetStatusArg &) {
  bess::pb::QueueCommandGetStatusResponse resp;
  resp.set_count(rte_ring_count(queue_));
  resp.set_size(size_);
  resp.set_enqueued(stats_.enqueued.load(std::memory_order_relaxed));
  resp.set_dequeued(stats_.dequeued.load(std::memory_order_relaxed));
  resp.set_dropped(stats_.dropped.load(std::memory_order_relaxed));
  return CommandSuccess(resp);
}

void Queue::AdjustWaterLevels() {
  high_water_ = static_cast<uint64_t>(size_ * kHighWaterRatio);
  low_water_ = static_cast<uint64_t>(size_ * kLowWaterRatio);
}

CheckConstraintResult Queue::CheckModuleConstraints() const {
  CheckConstraintResult status = CHECK_OK;
  const size_t workers = num_active_workers();
  if (workers <= 1) {
    const_cast<Queue *>(this)->enqueue_fn_ = &rte_ring_sp_enqueue_burst;
  } else {
    const_cast<Queue *>(this)->enqueue_fn_ = &rte_ring_mp_enqueue_burst;
  }

  if (num_active_tasks() - tasks().size() < 1) {
    LOG(ERROR) << "Queue has no producers";
    status = CHECK_NONFATAL_ERROR;
  }

  if (tasks().size() > 1) {  // Assume single consumer.
    LOG(ERROR) << "More than one consumer for the queue" << name();
    return CHECK_FATAL_ERROR;
  }

  return status;
}

ADD_MODULE(Queue, "queue",
           "terminates current task and enqueue packets for new task")
