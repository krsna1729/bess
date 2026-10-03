// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// Copyright (c) 2017, Cloudigo.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_IP_CHECKSUM_H_
#define BESS_MODULES_IP_CHECKSUM_H_

#include "module.h"

// Compute IP checksum on packet
class IPChecksum final : public Module {
 public:
  IPChecksum() : Module(), verify_(false) { max_allowed_workers_ = Worker::kMaxWorkers; }

  /* Gates: (0) Default, (1) Drop */
  static const gate_idx_t kNumOGates = 2;
  CommandResponse Init(const bess::pb::IPChecksumArg &arg);
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  /* enable checksum verification */
  bool verify_;
};

#endif  // BESS_MODULES_IP_CHECKSUM_H_
