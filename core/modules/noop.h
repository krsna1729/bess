// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_NOOP_H_
#define BESS_MODULES_NOOP_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"

class NoOP final : public Module {
 public:
  NoOP() : Module() { is_task_ = true; };
  CommandResponse Init(const bess::pb::EmptyArg &arg);

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;

  static const gate_idx_t kNumIGates = 0;
  static const gate_idx_t kNumOGates = 0;
};

#endif  // BESS_MODULES_NOOP_H_
