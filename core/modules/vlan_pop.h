// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_VLANPOP_H_
#define BESS_MODULES_VLANPOP_H_

#include "../module.h"

class VLANPop final : public Module {
 public:
  VLANPop() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::VLANPopArg &) { return CommandSuccess(); }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
};

#endif  // BESS_MODULES_VLANPOP_H_
