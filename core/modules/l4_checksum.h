// Copyright (c) 2017, The Regents of the University of California.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_L4_CHECKSUM_H_
#define BESS_MODULES_L4_CHECKSUM_H_

#include "module.h"

// Compute L4 checksum on packet
class L4Checksum final : public Module {
 public:
 L4Checksum() : Module(), verify_(false) { max_allowed_workers_ = Worker::kMaxWorkers; }

  /* Gates: (0) Default, (1) Drop */
  static const gate_idx_t kNumOGates = 2;
  static const gate_idx_t kNumIGates = MAX_GATES;
  CommandResponse Init(const bess::pb::L4ChecksumArg &arg);
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  bool verify_;
};

#endif  // BESS_MODULES_L4_CHECKSUM_H_
