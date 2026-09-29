// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_GENERICDECAP_H_
#define BESS_MODULES_GENERICDECAP_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"

class GenericDecap final : public Module {
 public:
  GenericDecap() : Module(), decap_size_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::GenericDecapArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  int decap_size_;
};

#endif  // BESS_MODULES_GENERICDECAP_H_
