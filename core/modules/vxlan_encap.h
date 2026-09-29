// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_VXLANENCAP_H_
#define BESS_MODULES_VXLANENCAP_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"

#include "../utils/endian.h"

class VXLANEncap final : public Module {
 public:
  static const uint16_t kDefaultDstPort;

  VXLANEncap() : Module(), dstport_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::VXLANEncapArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  bess::utils::be16_t dstport_;
};

#endif  // BESS_MODULES_VXLANENCAP_H_
