// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "worker_split.h"

const Commands WorkerSplit::cmds = {
    {"reset", "WorkerSplitArg", MODULE_CMD_FUNC(&WorkerSplit::CommandReset),
     Command::THREAD_UNSAFE}};

CommandResponse WorkerSplit::Init(const bess::pb::WorkerSplitArg &arg) {
  return CommandReset(arg);
}

CommandResponse WorkerSplit::CommandReset(const bess::pb::WorkerSplitArg &arg) {
  if (arg.worker_gates().empty()) {
    for (int i = 0; i < Worker::kMaxWorkers; i++) {
      gates_[i] = i;
    }
    return CommandSuccess();
  }

  for (size_t i = 0; i < Worker::kMaxWorkers; i++) {
    gates_[i] = -1;
  }

  for (auto it : arg.worker_gates()) {
    gate_idx_t ogate = it.second;
    if (ogate >= MAX_GATES) {
      return CommandFailure(EINVAL, "output gate must be less than %" PRIu16,
                            MAX_GATES);
    }
    gates_[it.first] = ogate;
  }

  return CommandSuccess();
}

void WorkerSplit::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int gate = gates_[ctx->wid];
  if (gate >= 0) {
    RunChooseModule(ctx, gate, batch);
  } else {
    bess::PacketFreeBatch(batch);
  }
}

void WorkerSplit::AddActiveWorker(int wid, const Task *t) {
  if (!HaveVisitedWorker(t)) {  // Have not already accounted for worker.
    active_workers_[wid] = true;
    visited_tasks_.push_back(t);
    // Only propagate workers downstream on ogate mapped to `wid`
    int g = gates_[wid];
    bess::OGate *ogate = (g < 0) ? nullptr : ogates()[g];
    if (ogate) {
      auto next = static_cast<Module *>(ogate->next());
      next->AddActiveWorker(wid, t);
    }
  }
}

ADD_MODULE(WorkerSplit, "ws",
           "send packets to output gate X, the id of current worker")
