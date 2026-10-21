// Copyright (c) 2017, The Regents of the University of California.
// SPDX-License-Identifier: BSD-3-Clause

#include "task_graph.h"
#include "module_graph.h"

const std::string SetupTaskGraph::kName = "setup_taskgraph";

SetupTaskGraph::SetupTaskGraph() : bess::ResumeHook(kName, kPriority, true) {}

CommandResponse SetupTaskGraph::Init(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

std::optional<CommandResponse> SetupTaskGraph::Run() {
  ModuleGraph::UpdateTaskGraph();
  // Which workers run each module, from the graph they resume with: the
  // EBUSY check of THREAD_UNSAFE commands and PreResume events read it. Only
  // the legacy CheckSchedulingConstraints set it before, so after
  // ApplyPipeline + ResumeWorkers every module looked unattached.
  ModuleGraph::PropagateActiveWorker();
  return std::nullopt;
}

ADD_RESUME_HOOK(SetupTaskGraph)

bool __enable_SetupTaskGraph = []() {
  bool ret = bess::global_resume_hooks.emplace(new SetupTaskGraph()).second;
  if (!ret) {
    LOG(ERROR) << "Failed to enable SetupTaskGraph hook by default";
  }
  return ret;
}();
