// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "task.h"

#include <unordered_set>

#include "gate.h"
#include "module.h"
#include "traffic_class.h"

// Called when the leaf that owns this task is destroyed.
void Task::Detach() {
  c_ = nullptr;
}

// Called when the leaf that owns this task is created.
void Task::Attach(bess::LeafTrafficClass *c) {
  c_ = c;
}

struct task_result Task::operator()(Context *ctx) const {
  bess::PacketBatch init_batch;
  ClearPacketBatch();

  // Start from the first module (task module)
  struct task_result result = module_->RunTask(ctx, &init_batch, arg_);
  // next_gate_: Continuously run if modules are chained
  // igates_to_run_ : If next module connection is not chained (merged),
  // check priority to choose which module run next
  while (next_gate_ || !igates_to_run_.empty()) {
    bess::IGate *igate;
    bess::PacketBatch *batch;

    // choose igate and batch to run next
    if (next_gate_) {
      igate = next_gate_;
      batch = next_batch_;
      next_gate_ = nullptr;
      next_batch_ = nullptr;
    } else {
      auto item = igates_to_run_.top();
      igates_to_run_.pop();

      igate = item.first;
      batch = item.second;

      set_gate_batch(igate, nullptr);
    }

    ctx->current_igate = igate->gate_idx();

    for (auto &hook : igate->hooks()) {
      hook->ProcessBatch(batch);
    }

    Module *m = igate->module();
    m->ProcessBatch(ctx, batch);  // process module
    m->ProcessOGates(ctx);        // process ogates
  }

  deadend(ctx, &dead_batch_);

  return result;
}

// Compute constraints for the pipeline starting at this task.
placement_constraint Task::GetSocketConstraints() const {
  if (module_) {
    std::unordered_set<const Module *> visited;
    return module_->ComputePlacementConstraints(&visited);
  } else {
    return UNCONSTRAINED_SOCKET;
  }
}

// Add a worker to the set of workers that call this task.
void Task::AddActiveWorker(int wid) const {
  if (module_) {
    module_->AddActiveWorker(wid, c_->task());
  }
}
