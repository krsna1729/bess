// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "source.h"

#include <expected>
#include <string>

#include "packet_pool.h"
#include "utils/format.h"

namespace {

// set_pkt_size's refusal ("" when `pkt_size` is valid).
std::string PktSizeError(uint64_t pkt_size) {
  if (pkt_size == 0 || pkt_size > bess::PacketPool::DefaultDataRoomSize()) {
    return bess::utils::Format("Invalid packet size: maximum is %zu",
                               bess::PacketPool::DefaultDataRoomSize());
  }
  return "";
}

// set_burst's refusal ("" when `burst` is valid).
std::string BurstError(uint64_t burst) {
  if (burst > bess::PacketBatch::kMaxBurst) {
    return bess::utils::Format("burst size must be [0,%zu]",
                               bess::PacketBatch::kMaxBurst);
  }
  return "";
}

}  // namespace

// "settings" (SourceSettings) holds packet size and burst; the class has no
// actions.
const Commands Source::cmds = {};

const ResourceTables Source::resource_tables = {
    ResourceTable::Singleton<bess::pb::SourceSettings>("settings")};

CommandResponse Source::Init(const bess::pb::SourceArg &arg) {

  task_id_t tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID)
    return CommandFailure(ENOMEM, "Task creation failed");

  pkt_size_.store(60, std::memory_order_relaxed);
  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);

  if (arg.pkt_size() > 0) {
    if (arg.pkt_size() > bess::PacketPool::DefaultDataRoomSize()) {
      return CommandFailure(EINVAL, "Invalid packet size: maximum is %zu",
                            bess::PacketPool::DefaultDataRoomSize());
    }
    pkt_size_.store(arg.pkt_size(), std::memory_order_relaxed);
  }

  if (arg.has_burst()) {
    if (std::string err = BurstError(arg.burst()); !err.empty()) {
      return CommandFailure(EINVAL, "%s", err.c_str());
    }
    burst_.store(arg.burst(), std::memory_order_relaxed);
  }

  // Applied live: both fields are relaxed atomics the task reads once per
  // run (it may pair a new size with the old burst for one run).
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::SourceSettings &v)
          -> std::expected<Settings, std::string> {
        if (std::string err = PktSizeError(v.pkt_size()); !err.empty()) {
          return std::unexpected(std::move(err));
        }
        if (std::string err = BurstError(v.burst()); !err.empty()) {
          return std::unexpected(std::move(err));
        }
        return Settings{static_cast<int>(v.pkt_size()),
                        static_cast<int>(v.burst())};
      },
      [this](Settings &s) {
        pkt_size_.store(s.pkt_size, std::memory_order_relaxed);
        burst_.store(s.burst, std::memory_order_relaxed);
      },
      [this] {
        bess::pb::SourceSettings v;
        v.set_pkt_size(pkt_size_.load(std::memory_order_relaxed));
        v.set_burst(burst_.load(std::memory_order_relaxed));
        return v;
      },
      nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Source::DeInit() {
  settings_.Release();
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
