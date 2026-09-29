// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_VXLANDECAP_H_
#define BESS_MODULES_VXLANDECAP_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"

class VXLANDecap final : public Module {
 public:
  VXLANDecap() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }
  CommandResponse Init(const bess::pb::VXLANDecapArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
};

#endif  // BESS_MODULES_VXLANDECAP_H_
