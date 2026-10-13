// Copyright (c) 2017, The Regents of the University of California.
// Copyright (c) 2017, Vivian Fang.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "random_split.h"
#include <string>
#include <time.h>

#include "utils/format.h"

static inline bool is_valid_gate(gate_idx_t gate) {
  return (gate < MAX_GATES || gate == DROP_GATE);
}

const Commands RandomSplit::cmds = {
    {"set_droprate", "RandomSplitCommandSetDroprateArg",
     MODULE_CMD_FUNC(&RandomSplit::CommandSetDroprate), Command::THREAD_UNSAFE,
     "", Command::LEGACY_ONLY},
    {"set_gates", "RandomSplitCommandSetGatesArg",
     MODULE_CMD_FUNC(&RandomSplit::CommandSetGates), Command::THREAD_UNSAFE,
     "", Command::LEGACY_ONLY}};

const ResourceTables RandomSplit::resource_tables = {
    ResourceTable::Singleton<bess::pb::RandomSplitArg>("settings", true),
};

CommandResponse RandomSplit::Init(const bess::pb::RandomSplitArg &arg) {
  double drop_rate = arg.drop_rate();
  if (drop_rate < 0 || drop_rate > 1) {
    return CommandFailure(EINVAL, "drop rate needs to be between [0, 1]");
  }
  drop_rate_ = drop_rate;

  if (arg.gates_size() > MAX_SPLIT_GATES) {
    return CommandFailure(EINVAL, "no more than %d gates", MAX_SPLIT_GATES);
  }

  for (int i = 0; i < arg.gates_size(); i++) {
    if (!is_valid_gate(arg.gates(i))) {
      return CommandFailure(EINVAL, "Invalid gate %d", gates_[i]);
    }
  }

  ngates_ = arg.gates_size();
  for (int i = 0; i < ngates_; i++) {
    gates_[i] = arg.gates(i);
  }

  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::RandomSplitArg &value) { return StageSettings(value); },
      [this](Settings &settings) { ApplySettings(settings); },
      [this] { return ReadSettings(); }, [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void RandomSplit::DeInit() {
  settings_.Release();
}

std::expected<RandomSplit::Settings, std::string> RandomSplit::StageSettings(
    const bess::pb::RandomSplitArg &arg) {
  double drop_rate = arg.drop_rate();
  if (drop_rate < 0 || drop_rate > 1) {
    return std::unexpected("drop rate needs to be between [0, 1]");
  }
  if (arg.gates_size() > MAX_SPLIT_GATES) {
    return std::unexpected(
        bess::utils::Format("no more than %d gates", MAX_SPLIT_GATES));
  }
  Settings staged;
  staged.drop_rate = drop_rate;
  staged.gates.reserve(arg.gates_size());
  for (int64_t gate : arg.gates()) {
    const gate_idx_t idx = static_cast<gate_idx_t>(gate);
    if (gate < 0 || static_cast<int64_t>(idx) != gate || !is_valid_gate(idx)) {
      return std::unexpected(bess::utils::Format(
          "Invalid gate %lld", static_cast<long long>(gate)));
    }
    staged.gates.push_back(idx);
  }
  return staged;
}

void RandomSplit::ApplySettings(const Settings &settings) {
  drop_rate_ = settings.drop_rate;
  ngates_ = static_cast<gate_idx_t>(settings.gates.size());
  for (size_t i = 0; i < settings.gates.size(); i++) {
    gates_[i] = settings.gates[i];
  }
}

bess::pb::RandomSplitArg RandomSplit::ReadSettings() const {
  bess::pb::RandomSplitArg value;
  value.set_drop_rate(drop_rate_);
  for (int i = 0; i < ngates_; i++) {
    value.add_gates(gates_[i]);
  }
  return value;
}

CommandResponse RandomSplit::CommandSetDroprate(
    const bess::pb::RandomSplitCommandSetDroprateArg &arg) {
  double drop_rate = arg.drop_rate();
  if (drop_rate < 0 || drop_rate > 1) {
    return CommandFailure(EINVAL, "drop rate needs to be between [0, 1]");
  }
  drop_rate_ = drop_rate;

  return CommandSuccess();
}

CommandResponse RandomSplit::CommandSetGates(
    const bess::pb::RandomSplitCommandSetGatesArg &arg) {
  if (arg.gates_size() > MAX_SPLIT_GATES) {
    return CommandFailure(EINVAL, "no more than %d gates", MAX_SPLIT_GATES);
  }

  for (int i = 0; i < arg.gates_size(); i++) {
    if (!is_valid_gate(arg.gates(i))) {
      return CommandFailure(EINVAL, "Invalid gate %d", gates_[i]);
    }
  }

  ngates_ = arg.gates_size();
  for (int i = 0; i < ngates_; i++) {
    gates_[i] = arg.gates(i);
  }

  return CommandSuccess();
}

void RandomSplit::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (ngates_ <= 0) {
    bess::PacketFreeBatch(batch);
    return;
  }

  for (bess::PacketRef pkt : batch->packets()) {
    if (rng_.GetReal() > drop_rate_) {
      EmitPacket(ctx, pkt, gates_[rng_.GetRange(ngates_)]);
    } else {
      DropPacket(ctx, pkt);
    }
  }
}

ADD_MODULE(RandomSplit, "random_split", "randomly splits/drops packets")
