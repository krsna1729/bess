// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "port_inc.h"

#include <expected>
#include <string>

#include "utils/format.h"

namespace {

// set_burst's refusal ("" when `burst` is valid).
std::string BurstError(uint64_t burst) {
  if (burst > bess::PacketBatch::kMaxBurst) {
    return bess::utils::Format("burst size must be [0,%zu]",
                               bess::PacketBatch::kMaxBurst);
  }
  return "";
}

}  // namespace

// "settings" (PortIncSettings) holds burst; the class has no actions.
const Commands PortInc::cmds = {};

const ResourceTables PortInc::resource_tables = {
    ResourceTable::Singleton<bess::pb::PortIncSettings>("settings")};

CommandResponse PortInc::Init(const bess::pb::PortIncArg &arg) {
  const char *port_name;
  queue_t num_inc_q;
  int ret;
  placement_constraint placement;

  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);

  if (!arg.port().length()) {
    return CommandFailure(EINVAL, "'port' must be given as a string");
  }
  port_name = arg.port().c_str();

  port_ = init_context().ports().Find(port_name);
  if (!port_) {
    return CommandFailure(ENODEV, "Port %s not found", port_name);
  }
  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);

  num_inc_q = port_->num_queues[PACKET_DIR_INC];
  if (num_inc_q == 0) {
    return CommandFailure(ENODEV, "Port %s has no incoming queue", port_name);
  }

  placement = port_->GetNodePlacementConstraint();
  node_constraints_ = placement;

  for (queue_t qid = 0; qid < num_inc_q; qid++) {
    task_id_t tid = RegisterTask((void *)(uintptr_t)qid);

    if (tid == INVALID_TASK_ID) {
      return CommandFailure(ENOMEM, "Context creation failed");
    }
  }

  if (arg.prefetch()) {
    prefetch_ = 1;
  }

  ret = port_->AcquireQueues(reinterpret_cast<const module *>(this),
                             PACKET_DIR_INC, nullptr, 0);
  if (ret < 0) {
    return CommandFailure(-ret);
  }

  // Applied live: the burst is a relaxed atomic each task run reads once.
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::PortIncSettings &v) -> std::expected<int, std::string> {
        if (std::string err = BurstError(v.burst()); !err.empty()) {
          return std::unexpected(std::move(err));
        }
        return static_cast<int>(v.burst());
      },
      [this](int &burst) { burst_.store(burst, std::memory_order_relaxed); },
      [this] {
        bess::pb::PortIncSettings v;
        v.set_burst(burst_.load(std::memory_order_relaxed));
        return v;
      },
      nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  return CommandSuccess();
}

void PortInc::DeInit() {
  settings_.Release();
  if (port_) {
    port_->ReleaseQueues(reinterpret_cast<const module *>(this), PACKET_DIR_INC,
                         nullptr, 0);
  }
}

std::string PortInc::GetDesc() const {
  return bess::utils::Format("%s/%s", port_->name().c_str(),
                             port_->port_builder()->class_name().c_str());
}

struct task_result PortInc::RunTask(Context *ctx, bess::PacketBatch *batch,
                                    void *arg) {
  if (children_overload_ > 0) {
    return {.block = true, .packets = 0, .bits = 0};
  }

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

ADD_MODULE(PortInc, "port_inc", "receives packets from a port")
