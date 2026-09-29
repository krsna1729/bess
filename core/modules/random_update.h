// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_RANDOMUPDATE_H_
#define BESS_MODULES_RANDOMUPDATE_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"

#include "../utils/endian.h"
#include "../utils/random.h"

static const size_t kMaxVariable = 16;

class RandomUpdate final : public Module {
 public:
  static const Commands cmds;

  RandomUpdate() : Module(), num_vars_(), vars_(), rng_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::RandomUpdateArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandAdd(const bess::pb::RandomUpdateArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

 private:
  size_t num_vars_;

  struct {
    bess::utils::be32_t mask;  // bits with 1 won't be updated
    uint32_t min;
    uint32_t range;  // max - min + 1
    size_t offset;
    size_t bit_shift;
  } vars_[kMaxVariable];

  Random rng_;
};

#endif  // BESS_MODULES_RANDOMUPDATE_H_
