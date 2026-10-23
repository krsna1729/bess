// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "queue_inc.h"

#include <expected>
#include <string>

#include "modules/rx_mark.h"
#include "port.h"
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

// "settings" (QueueIncSettings) holds burst; the class has no actions.
const Commands QueueInc::cmds = {};

const ResourceTables QueueInc::resource_tables = {
    ResourceTable::Singleton<bess::pb::QueueIncSettings>("settings")};

CommandResponse QueueInc::Init(const bess::pb::QueueIncArg &arg) {
  const char *port_name;
  task_id_t tid;
  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);
  if (!arg.port().length()) {
    return CommandFailure(EINVAL, "Field 'port' must be specified");
  }
  port_name = arg.port().c_str();
  qid_ = arg.qid();

  port_ = init_context().ports().Find(port_name);
  if (!port_) {
    return CommandFailure(ENODEV, "Port %s not found", port_name);
  }
  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);

  if (arg.prefetch()) {
    prefetch_ = 1;
  }
  // D-185: the attribute exists only when configured; it has an offset only
  // when a module downstream reads it.
  mark_attr_name_ = arg.mark_attr();
  if (!mark_attr_name_.empty()) {
    mark_attr_ = AddMetadataAttr(mark_attr_name_, sizeof(bess::utils::be32_t),
                                 bess::metadata::Attribute::AccessMode::kWrite);
    if (mark_attr_ < 0) {
      return CommandFailure(-mark_attr_, "add_metadata_attr() failed");
    }
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

  // Applied live: the burst is a relaxed atomic each task run reads once.
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::QueueIncSettings &v) -> std::expected<int, std::string> {
        if (std::string err = BurstError(v.burst()); !err.empty()) {
          return std::unexpected(std::move(err));
        }
        return static_cast<int>(v.burst());
      },
      [this](int &burst) { burst_.store(burst, std::memory_order_relaxed); },
      [this] {
        bess::pb::QueueIncSettings v;
        v.set_burst(burst_.load(std::memory_order_relaxed));
        return v;
      },
      nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  return CommandSuccess();
}

void QueueInc::DeInit() {
  settings_.Release();
  if (port_) {
    port_->ReleaseQueues(reinterpret_cast<const module *>(this), PACKET_DIR_INC,
                         &qid_, 1);
  }
}

int QueueInc::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  // Offsets are assigned just before this: a configured MARK attribute
  // promises a reader (D-185).
  if (mark_attr_ >= 0 && !bess::metadata::IsValidOffset(MarkOffset())) {
    LOG(WARNING) << "QueueInc '" << name() << "': nothing reads '" << mark_attr_name_
                 << "' (a module downstream reading it would); packets carry no MARK";
  }
  return 0;
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

  // One test per batch: without a reader of mark_attr nothing is written.
  if (const bess::metadata::mt_offset_t mark = MarkOffset(); bess::metadata::IsValidOffset(mark)) {
    bess::modules::WriteRxMarks(mark, batch);
  }

  RunNextModule(ctx, batch);

  return {.block = false,
          .packets = cnt,
          .bits = (received_bytes + cnt * pkt_overhead) * 8};
}

ADD_MODULE(QueueInc, "queue_inc",
           "receives packets from a port via a specific queue")
