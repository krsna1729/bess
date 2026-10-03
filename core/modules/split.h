// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_SPLIT_H_
#define BESS_MODULES_SPLIT_H_

#include "module.h"
#include "pb/module_msg.pb.h"

class Split final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  Split() : Module(), mask_(), attr_id_(), offset_(), size_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::SplitArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  uint64_t mask_;
  int shift_;
  int attr_id_;
  size_t offset_;
  size_t size_;
};

#endif  // BESS_MODULES_SPLIT_H_
