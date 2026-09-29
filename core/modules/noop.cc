// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "noop.h"

CommandResponse NoOP::Init(const bess::pb::EmptyArg &) {
  task_id_t tid;

  tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID)
    return CommandFailure(ENOMEM, "Context creation failed");

  return CommandSuccess();
}

struct task_result NoOP::RunTask(Context *, bess::PacketBatch *, void *) {
  return {.block = false, .packets = 0, .bits = 0};
}

ADD_MODULE(NoOP, "noop", "creates a task that does nothing")
