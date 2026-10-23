// Copyright (c) 2017, Vivian Fang.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_RANDOM_SPLIT_H_
#define BESS_MODULES_RANDOM_SPLIT_H_

#include <expected>
#include <string>
#include <vector>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "stats/worker_local.h"
#include "utils/random.h"

// Maximum number of output gates to allow.
#define MAX_SPLIT_GATES 16384

// RandomSplit splits and drop packets.
class RandomSplit final : public Module {
 public:
  RandomSplit() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  static const gate_idx_t kNumOGates = MAX_GATES;
  static const Commands cmds;
  static const ResourceTables resource_tables;
  // Each worker advances its own RNG; packets need no flow affinity.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kPerWorker,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  CommandResponse Init(const bess::pb::RandomSplitArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  struct Settings {
    double drop_rate = 0;
    std::vector<gate_idx_t> gates;
  };

  // "<name>/settings" (D-129): validated as the setters did,
  // and a gate must fit a gate index.
  static std::expected<Settings, std::string> StageSettings(
      const bess::pb::RandomSplitArg &arg);
  void ApplySettings(const Settings &settings);
  bess::pb::RandomSplitArg ReadSettings() const;

  bess::stats::WorkerLocal<Random> rng_;
  double drop_rate_;
  gate_idx_t gates_[MAX_SPLIT_GATES];
  gate_idx_t ngates_;

  bess::framework::Singleton<bess::pb::RandomSplitArg, Settings> settings_;
};

#endif  // BESS_MODULES_RANDOM_SPLIT_H_
