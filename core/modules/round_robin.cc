// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "round_robin.h"

#include "utils/format.h"

// "settings" (RoundRobinArg) holds mode and gates; the class has no actions.
const Commands RoundRobin::cmds = {};

const ResourceTables RoundRobin::resource_tables = {
    ResourceTable::Singleton<bess::pb::RoundRobinArg>("settings", true),
};

CommandResponse RoundRobin::Init(const bess::pb::RoundRobinArg &arg) {

  if (arg.gates_size() > MAX_RR_GATES) {
    return CommandFailure(EINVAL, "no more than %d gates", MAX_RR_GATES);
  }

  for (int i = 0; i < arg.gates_size(); i++) {
    int elem = arg.gates(i);
    gates_[i] = elem;
    if (!is_valid_gate(gates_[i])) {
      return CommandFailure(EINVAL, "invalid gate %d", gates_[i]);
    }
  }
  ngates_ = arg.gates_size();

  if (arg.mode().length()) {
    if (arg.mode() == "packet") {
      per_packet_ = 1;
    } else if (arg.mode() == "batch") {
      per_packet_ = 0;
    } else {
      return CommandFailure(EINVAL,
                            "argument must be either 'packet' or 'batch'");
    }
  }

  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::RoundRobinArg &value) { return StageSettings(value); },
      [this](Settings &settings) { ApplySettings(settings); },
      [this] { return ReadSettings(); }, [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void RoundRobin::DeInit() {
  settings_.Release();
}

// As set_mode and set_gates validate, except that the mode is required (the
// read-back always names it) and a gate must fit a gate index (the commands
// narrow it unchecked).
std::expected<RoundRobin::Settings, std::string> RoundRobin::StageSettings(
    const bess::pb::RoundRobinArg &arg) {
  Settings staged;
  if (arg.mode() == "packet") {
    staged.per_packet = 1;
  } else if (arg.mode() == "batch") {
    staged.per_packet = 0;
  } else {
    return std::unexpected("argument must be either 'packet' or 'batch'");
  }
  if (arg.gates_size() > MAX_RR_GATES) {
    return std::unexpected(
        bess::utils::Format("no more than %d gates", MAX_RR_GATES));
  }
  staged.gates.reserve(arg.gates_size());
  for (int64_t gate : arg.gates()) {
    const gate_idx_t idx = static_cast<gate_idx_t>(gate);
    if (gate < 0 || static_cast<int64_t>(idx) != gate || !is_valid_gate(idx)) {
      return std::unexpected(bess::utils::Format(
          "invalid gate %lld", static_cast<long long>(gate)));
    }
    staged.gates.push_back(idx);
  }
  return staged;
}

void RoundRobin::ApplySettings(const Settings &settings) {
  for (size_t i = 0; i < settings.gates.size(); i++) {
    gates_[i] = settings.gates[i];
  }
  ngates_ = static_cast<int>(settings.gates.size());
  current_gate_ = 0;
  per_packet_ = settings.per_packet;
}

bess::pb::RoundRobinArg RoundRobin::ReadSettings() const {
  bess::pb::RoundRobinArg value;
  for (int i = 0; i < ngates_; i++) {
    value.add_gates(gates_[i]);
  }
  value.set_mode(per_packet_ ? "packet" : "batch");
  return value;
}

void RoundRobin::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (ngates_ <= 0) {
    bess::PacketFreeBatch(batch);
    return;
  }

  if (per_packet_) {
    for (bess::PacketRef pkt : batch->packets()) {
      EmitPacket(ctx, pkt, gates_[current_gate_]);
      if (++current_gate_ >= ngates_) {
        current_gate_ = 0;
      }
    }
  } else {
    gate_idx_t gate = gates_[current_gate_];
    if (++current_gate_ >= ngates_) {
      current_gate_ = 0;
    }
    RunChooseModule(ctx, gate, batch);
  }
}

ADD_MODULE(RoundRobin, "rr", "splits packets evenly with round robin")
