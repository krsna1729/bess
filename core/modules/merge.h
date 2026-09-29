// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_MERGE_H_
#define BESS_MODULES_MERGE_H_

#include "../module.h"

class Merge final : public Module {
 public:
  Merge() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::MergeArg &) { return CommandSuccess(); }

  static const gate_idx_t kNumIGates = MAX_GATES;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
};

#endif  // BESS_MODULES_MERGE_H_
