// Copyright (c) 2017, The Regents of the University of California.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RESUME_HOOKS_TASK_GRAPH_
#define BESS_RESUME_HOOKS_TASK_GRAPH_

#include "../message.h"
#include "../resume_hook.h"
#include "../worker.h"

// SetupTaskGraph computes read/write offsets for packet metadata attributes.
class SetupTaskGraph final : public bess::ResumeHook {
 public:
  SetupTaskGraph();

  CommandResponse Init(const bess::pb::EmptyArg &);

  void Run() override;

  static constexpr uint16_t kPriority = 0;
  static const std::string kName;
};

#endif  // BESS_RESUME_HOOKS_TASK_GRAPH_
