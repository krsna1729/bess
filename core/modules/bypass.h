// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_BYPASS_H_
#define BESS_MODULES_BYPASS_H_

#include "../module.h"

/* This module simply passes packets from input gate X down to output gate X
 * (the same gate index) */

class Bypass final : public Module {
 public:
  static const gate_idx_t kNumIGates = MAX_GATES;
  static const gate_idx_t kNumOGates = MAX_GATES;

  Bypass() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::BypassArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  uint32_t cycles_per_batch_;
  uint32_t cycles_per_packet_;
  uint32_t cycles_per_byte_;
};

#endif  // BESS_MODULES_BYPASS_H_
