// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "worker_split.h"

#include "utils/format.h"

const Commands WorkerSplit::cmds = {
    {"reset", "WorkerSplitArg", MODULE_CMD_FUNC(&WorkerSplit::CommandReset),
     Command::THREAD_UNSAFE, "", Command::LEGACY_ONLY}};

const ResourceTables WorkerSplit::resource_tables = {
    ResourceTable::Singleton<bess::pb::WorkerSplitArg>("settings", true),
};

CommandResponse WorkerSplit::Init(const bess::pb::WorkerSplitArg &arg) {
  CommandResponse err = CommandReset(arg);
  if (err.error().code() != 0) {
    return err;
  }
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::WorkerSplitArg &value) { return StageGates(value); },
      [this](Gates &gates) {
        for (int i = 0; i < Worker::kMaxWorkers; i++) {
          gates_[i] = gates[i];
        }
      },
      [this] { return ReadGates(); }, [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void WorkerSplit::DeInit() {
  settings_.Release();
}

// Every worker id must name a worker and every gate an output gate; nothing
// changes on a refusal.
std::expected<WorkerSplit::Gates, std::string> WorkerSplit::StageGates(
    const bess::pb::WorkerSplitArg &arg) {
  Gates gates;
  if (arg.worker_gates().empty()) {
    for (int i = 0; i < Worker::kMaxWorkers; i++) {
      gates[i] = i;
    }
    return gates;
  }

  gates.fill(-1);
  for (const auto &[wid, ogate] : arg.worker_gates()) {
    if (wid >= static_cast<uint32_t>(Worker::kMaxWorkers)) {
      return std::unexpected(bess::utils::Format(
          "worker id must be less than %d", Worker::kMaxWorkers));
    }
    if (ogate >= MAX_GATES) {
      return std::unexpected(bess::utils::Format(
          "output gate must be less than %" PRIu16, MAX_GATES));
    }
    gates[wid] = static_cast<int>(ogate);
  }
  return gates;
}

bess::pb::WorkerSplitArg WorkerSplit::ReadGates() const {
  bess::pb::WorkerSplitArg value;
  bool identity = true;
  for (int i = 0; i < Worker::kMaxWorkers; i++) {
    identity = identity && gates_[i] == i;
  }
  if (identity) {
    return value;
  }
  for (int i = 0; i < Worker::kMaxWorkers; i++) {
    if (gates_[i] >= 0) {
      (*value.mutable_worker_gates())[i] = static_cast<uint32_t>(gates_[i]);
    }
  }
  return value;
}

CommandResponse WorkerSplit::CommandReset(const bess::pb::WorkerSplitArg &arg) {
  auto gates = StageGates(arg);
  if (!gates) {
    return CommandFailure(EINVAL, "%s", gates.error().c_str());
  }
  for (int i = 0; i < Worker::kMaxWorkers; i++) {
    gates_[i] = (*gates)[i];
  }
  return CommandSuccess();
}

void WorkerSplit::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int gate = gates_[ctx->wid.value()];
  if (gate >= 0) {
    RunChooseModule(ctx, gate, batch);
  } else {
    bess::PacketFreeBatch(batch);
  }
}

void WorkerSplit::AddActiveWorker(bess::dataplane::WorkerId wid,
                                  const Task *t) {
  if (!HaveVisitedWorker(t)) {  // Have not already accounted for worker.
    active_workers_[wid.value()] = true;
    visited_tasks_.push_back(t);
    // Only propagate workers downstream on ogate mapped to `wid`
    int g = gates_[wid.value()];
    bess::OGate *ogate = (g < 0) ? nullptr : ogates()[g];
    if (ogate) {
      auto next = static_cast<Module *>(ogate->next());
      next->AddActiveWorker(wid, t);
    }
  }
}

ADD_MODULE(WorkerSplit, "ws",
           "send packets to output gate X, the id of current worker")
