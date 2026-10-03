// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_WORKERSPLIT_H_
#define BESS_MODULES_WORKERSPLIT_H_

#include "module.h"

class WorkerSplit final : public Module {
 public:
  static const gate_idx_t kNumOGates = Worker::kMaxWorkers;

  WorkerSplit() : Module(), gates_() { max_allowed_workers_ = kNumOGates; }

  static const Commands cmds;

  CommandResponse Init(const bess::pb::WorkerSplitArg &);

  CommandResponse CommandReset(const bess::pb::WorkerSplitArg &);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  void AddActiveWorker(int wid, const Task *task) override;

 private:
  int gates_[Worker::kMaxWorkers];
};

#endif  // BESS_MODULES_WORKERSPLIT_H_
