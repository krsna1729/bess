// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "queue_inc.h"

#include "runtime/runtime_state.h"
#include "../port.h"
#include "../utils/format.h"

const Commands QueueInc::cmds = {{"set_burst", "QueueIncCommandSetBurstArg",
                                  MODULE_CMD_FUNC(&QueueInc::CommandSetBurst),
                                  Command::THREAD_SAFE}};

CommandResponse QueueInc::Init(const bess::pb::QueueIncArg &arg) {
  const char *port_name;
  task_id_t tid;
  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);
  if (!arg.port().length()) {
    return CommandFailure(EINVAL, "Field 'port' must be specified");
  }
  port_name = arg.port().c_str();
  qid_ = arg.qid();

  port_ = bess::runtime::runtime().ports().Find(port_name);
  if (!port_) {
    return CommandFailure(ENODEV, "Port %s not found", port_name);
  }
  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);

  if (arg.prefetch()) {
    prefetch_ = 1;
  }
  node_constraints_ = port_->GetNodePlacementConstraint();
  tid = RegisterTask((void *)(uintptr_t)qid_);
  if (tid == INVALID_TASK_ID)
    return CommandFailure(ENOMEM, "Context creation failed");

  int ret = port_->AcquireQueues(reinterpret_cast<const module *>(this),
                                 PACKET_DIR_INC, &qid_, 1);
  if (ret < 0) {
    return CommandFailure(-ret);
  }

  return CommandSuccess();
}

void QueueInc::DeInit() {
  if (port_) {
    port_->ReleaseQueues(reinterpret_cast<const module *>(this), PACKET_DIR_INC,
                         &qid_, 1);
  }
}

std::string QueueInc::GetDesc() const {
  return bess::utils::Format("%s:%hhu/%s", port_->name().c_str(), qid_,
                             port_->port_builder()->class_name().c_str());
}

struct task_result QueueInc::RunTask(Context *ctx, bess::PacketBatch *batch,
                                     void *arg) {
  Port *p = port_;

  if (!p->conf().admin_up) {
    return {.block = true, .packets = 0, .bits = 0};
  }

  const queue_t qid = (queue_t)(uintptr_t)arg;

  uint64_t received_bytes = 0;

  const int burst = burst_.load(std::memory_order_relaxed);
  const int pkt_overhead = 24;

  batch->set_cnt(p->RecvPackets(qid, batch->handles(), burst));
  uint32_t cnt = batch->cnt();
  p->queue_stats[PACKET_DIR_INC][qid].requested_hist[burst]++;
  p->queue_stats[PACKET_DIR_INC][qid].actual_hist[cnt]++;
  p->queue_stats[PACKET_DIR_INC][qid].diff_hist[burst - cnt]++;
  if (cnt == 0) {
    return {.block = true, .packets = 0, .bits = 0};
  }

  // NOTE: we cannot skip this step since it might be used by scheduler.
  if (prefetch_) {
    for (uint32_t i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      received_bytes += pkt.total_len();
      rte_prefetch0(pkt.head_data());
    }
  } else {
    for (uint32_t i = 0; i < cnt; i++) {
      received_bytes += batch->packet(i).total_len();
    }
  }

  if (!(p->GetFlags() & DRIVER_FLAG_SELF_INC_STATS)) {
    p->queue_stats[PACKET_DIR_INC][qid].packets += cnt;
    p->queue_stats[PACKET_DIR_INC][qid].bytes += received_bytes;
  }

  RunNextModule(ctx, batch);

  return {.block = false,
          .packets = cnt,
          .bits = (received_bytes + cnt * pkt_overhead) * 8};
}

CommandResponse QueueInc::CommandSetBurst(
    const bess::pb::QueueIncCommandSetBurstArg &arg) {
  if (arg.burst() > bess::PacketBatch::kMaxBurst) {
    return CommandFailure(EINVAL, "burst size must be [0,%zu]",
                          bess::PacketBatch::kMaxBurst);
  } else {
    burst_.store(arg.burst(), std::memory_order_relaxed);
    return CommandSuccess();
  }
}

ADD_MODULE(QueueInc, "queue_inc",
           "receives packets from a port via a specific queue")
