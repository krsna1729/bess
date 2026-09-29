// Copyright (c) 2017, Vivian Fang.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_RANDOM_SPLIT_H_
#define BESS_MODULES_RANDOM_SPLIT_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../utils/random.h"

// Maximum number of output gates to allow.
#define MAX_SPLIT_GATES 16384

// RandomSplit splits and drop packets.
class RandomSplit final : public Module {
 public:
  RandomSplit() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  static const gate_idx_t kNumOGates = MAX_GATES;
  static const Commands cmds;

  CommandResponse Init(const bess::pb::RandomSplitArg &arg);
  CommandResponse CommandSetDroprate(
      const bess::pb::RandomSplitCommandSetDroprateArg &arg);
  CommandResponse CommandSetGates(
      const bess::pb::RandomSplitCommandSetGatesArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  Random rng_;  // Random number generator
  double drop_rate_;
  gate_idx_t gates_[MAX_SPLIT_GATES];
  gate_idx_t ngates_;
};

#endif  // BESS_MODULES_RANDOM_SPLIT_H_
