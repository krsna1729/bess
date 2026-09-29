// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "source.h"
#include "../packet_pool.h"

namespace {

size_t ConfiguredPacketDataRoom() {
  const bess::PacketPool *pool = bess::PacketPool::GetDefaultPool(0);
  return pool == nullptr ? bess::kDefaultPacketDataSize
                          : pool->data_room_size();
}

}  // namespace

const Commands Source::cmds = {
    {"set_pkt_size", "SourceCommandSetPktSizeArg",
     MODULE_CMD_FUNC(&Source::CommandSetPktSize), Command::THREAD_SAFE},
    {"set_burst", "SourceCommandSetBurstArg",
     MODULE_CMD_FUNC(&Source::CommandSetBurst), Command::THREAD_SAFE},
};

CommandResponse Source::Init(const bess::pb::SourceArg &arg) {

  task_id_t tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID)
    return CommandFailure(ENOMEM, "Task creation failed");

  pkt_size_.store(60, std::memory_order_relaxed);
  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);

  if (arg.pkt_size() > 0) {
    if (arg.pkt_size() > ConfiguredPacketDataRoom()) {
      return CommandFailure(EINVAL, "Invalid packet size: maximum is %zu",
                            ConfiguredPacketDataRoom());
    }
    pkt_size_.store(arg.pkt_size(), std::memory_order_relaxed);
  }

  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);

  return CommandSuccess();
}

CommandResponse Source::CommandSetBurst(
    const bess::pb::SourceCommandSetBurstArg &arg) {
  if (arg.burst() > bess::PacketBatch::kMaxBurst) {
    return CommandFailure(EINVAL, "burst size must be [0,%zu]",
                          bess::PacketBatch::kMaxBurst);
  }
  burst_.store(arg.burst(), std::memory_order_relaxed);

  return CommandSuccess();
}

CommandResponse Source::CommandSetPktSize(
    const bess::pb::SourceCommandSetPktSizeArg &arg) {
  uint64_t val = arg.pkt_size();
  if (val == 0 || val > ConfiguredPacketDataRoom()) {
    return CommandFailure(EINVAL, "Invalid packet size: maximum is %zu",
                          ConfiguredPacketDataRoom());
  }
  pkt_size_.store(val, std::memory_order_relaxed);
  return CommandSuccess();
}

struct task_result Source::RunTask(Context *ctx, bess::PacketBatch *batch,
                                   void *) {
  if (children_overload_ > 0) {
    return {.block = true, .packets = 0, .bits = 0};
  }

  const int pkt_overhead = 24;
  const int pkt_size = pkt_size_.load(std::memory_order_relaxed);
  const uint32_t burst = burst_.load(std::memory_order_relaxed);

  if (current_worker.packet_pool()->AllocBulk(batch->handles(), burst, pkt_size)) {
    batch->set_cnt(burst);
    RunNextModule(ctx, batch);  // it's fine to call this function with cnt==0
    return {.block = false,
            .packets = burst,
            .bits = (pkt_size + pkt_overhead) * burst * 8};
  }

  return {.block = true, .packets = 0, .bits = 0};
}

ADD_MODULE(Source, "source",
           "infinitely generates packets with uninitialized data")
