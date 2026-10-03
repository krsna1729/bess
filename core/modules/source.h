// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_FLOWGEN_H_
#define BESS_MODULES_FLOWGEN_H_

#include <atomic>

#include "module.h"
#include "pb/module_msg.pb.h"

class Source final : public Module {
 public:
  static const gate_idx_t kNumIGates = 0;

  static const Commands cmds;

  Source() : Module(), pkt_size_(), burst_() { is_task_ = true; }

  CommandResponse Init(const bess::pb::SourceArg &arg);

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;

  CommandResponse CommandSetBurst(
      const bess::pb::SourceCommandSetBurstArg &arg);
  CommandResponse CommandSetPktSize(
      const bess::pb::SourceCommandSetPktSizeArg &arg);

 private:
  // Set by THREAD_SAFE commands while workers read it.
  std::atomic<int> pkt_size_;
  // Set by THREAD_SAFE commands while workers read it.
  std::atomic<int> burst_;
};

#endif  // BESS_MODULES_FLOWGEN_H_
