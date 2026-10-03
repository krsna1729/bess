// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_REPLICATE_H_
#define BESS_MODULES_REPLICATE_H_

#include "module.h"
#include "pb/module_msg.pb.h"

class Replicate final : public Module {
 public:
  static const gate_idx_t kMaxGates = 32;
  static const gate_idx_t kNumOGates = kMaxGates;

  static const Commands cmds;

  Replicate() : Module(), gates_(), ngates_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::ReplicateArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  /*!
   * Sets the number of output gates.
   */
  CommandResponse CommandSetGates(
      const bess::pb::ReplicateCommandSetGatesArg &arg);

 private:
  // ID number for each egress gate.
  template <typename Arg>
  CommandResponse SetGates(const Arg &arg);

  gate_idx_t gates_[kMaxGates];
  // The total number of output gates
  int ngates_;
};

#endif  // BESS_MODULES_RELICATE_H_
