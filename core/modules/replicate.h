// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_REPLICATE_H_
#define BESS_MODULES_REPLICATE_H_

#include <array>
#include <expected>
#include <string>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"

class Replicate final : public Module {
 public:
  static const gate_idx_t kMaxGates = 32;
  static const gate_idx_t kNumOGates = kMaxGates;

  static const Commands cmds;
  static const ResourceTables resource_tables;

  Replicate() : Module(), gates_(), ngates_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::ReplicateArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  struct Gates {
    std::array<gate_idx_t, kMaxGates> gates{};
    int ngates = 0;
  };

  // Validates every wire gate; the gates to install, or why not.
  template <typename Arg>
  static std::expected<Gates, std::string> StageGates(const Arg &arg);
  void ApplyGates(const Gates &gates);
  template <typename Arg>
  CommandResponse SetGates(const Arg &arg);

  // ID number for each egress gate.
  gate_idx_t gates_[kMaxGates];
  // The total number of output gates
  int ngates_;

  // "<name>/settings" (D-129): the gates, paused workers only.
  bess::framework::Singleton<bess::pb::ReplicateArg, Gates> settings_;
};

#endif  // BESS_MODULES_RELICATE_H_
