// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_UPDATE_H_
#define BESS_MODULES_UPDATE_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"

#include "../utils/endian.h"

using bess::utils::be64_t;

class Update final : public Module {
 public:
  static const Commands cmds;

  Update() : Module(), num_fields_(), fields_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::UpdateArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandAdd(const bess::pb::UpdateArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

 private:
  static const size_t kMaxFields = 16;

  size_t num_fields_;

  struct {
    be64_t mask;  /* bits with 1 won't be updated */
    be64_t value; /* in network order */
    size_t offset;
  } fields_[kMaxFields];
};

#endif  // BESS_MODULES_UPDATE_H_
