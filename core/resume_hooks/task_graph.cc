// Copyright (c) 2017, The Regents of the University of California.
// SPDX-License-Identifier: BSD-3-Clause

#include "task_graph.h"
#include "module_graph.h"

const std::string SetupTaskGraph::kName = "setup_taskgraph";

SetupTaskGraph::SetupTaskGraph() : bess::ResumeHook(kName, kPriority, true) {}

CommandResponse SetupTaskGraph::Init(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

void SetupTaskGraph::Run() {
  ModuleGraph::UpdateTaskGraph();
}

ADD_RESUME_HOOK(SetupTaskGraph)

bool __enable_SetupTaskGraph = []() {
  bool ret = bess::global_resume_hooks.emplace(new SetupTaskGraph()).second;
  if (!ret) {
    LOG(ERROR) << "Failed to enable SetupTaskGraph hook by default";
  }
  return ret;
}();
