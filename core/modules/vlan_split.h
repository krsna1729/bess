// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_VLANSPLIT_H_
#define BESS_MODULES_VLANSPLIT_H_

#include "../module.h"

class VLANSplit final : public Module {
 public:
  VLANSplit() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::VLANSplitArg &) { return CommandSuccess(); }

  static const gate_idx_t kNumOGates = 4096;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
};

#endif  // BESS_MODULES_VLANSPLIT_H_
