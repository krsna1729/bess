// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_WORKERSPLIT_H_
#define BESS_MODULES_WORKERSPLIT_H_

#include <array>
#include <expected>
#include <string>

#include "framework/singleton_resource.h"
#include "module.h"

class WorkerSplit final : public Module {
 public:
  static const gate_idx_t kNumOGates = Worker::kMaxWorkers;

  WorkerSplit() : Module(), gates_() { max_allowed_workers_ = kNumOGates; }

  static const Commands cmds;
  static const ResourceTables resource_tables;
  // Reads the worker -> gate map; keeps no state.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  CommandResponse Init(const bess::pb::WorkerSplitArg &);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  void AddActiveWorker(bess::dataplane::WorkerId wid,
                       const Task *task) override;

 private:
  using Gates = std::array<int, Worker::kMaxWorkers>;

  // The gate of each worker (-1: dropped) `arg` asks for, or why not.
  static std::expected<Gates, std::string> StageGates(
      const bess::pb::WorkerSplitArg &arg);
  // Applies the init argument's gates.
  CommandResponse Reset(const bess::pb::WorkerSplitArg &arg);
  // Empty when every worker goes to its own gate (the default mode).
  bess::pb::WorkerSplitArg ReadGates() const;

  int gates_[Worker::kMaxWorkers];

  // "<name>/settings" (D-129): the worker -> gate map, paused workers only.
  bess::framework::Singleton<bess::pb::WorkerSplitArg, Gates> settings_;
};

#endif  // BESS_MODULES_WORKERSPLIT_H_
