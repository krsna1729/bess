// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "replicate.h"

#include "utils/format.h"

// "settings" (ReplicateArg) holds the gates; the class has no actions.
const Commands Replicate::cmds = {};

const ResourceTables Replicate::resource_tables = {
    ResourceTable::Singleton<bess::pb::ReplicateArg>("settings", true),
};

// Validates every wire gate (int64) before anything changes: each must name
// one of this module's output gates. They used to be narrowed unchecked.
template <typename Arg>
std::expected<Replicate::Gates, std::string> Replicate::StageGates(
    const Arg &arg) {
  if (arg.gates_size() > kMaxGates) {
    return std::unexpected(
        bess::utils::Format("no more than %d gates", kMaxGates));
  }
  Gates staged;
  for (int i = 0; i < arg.gates_size(); i++) {
    if (arg.gates(i) < 0 || arg.gates(i) >= kNumOGates) {
      return std::unexpected(bess::utils::Format(
          "gate %lld is out of range (0..%d)",
          static_cast<long long>(arg.gates(i)), kNumOGates - 1));
    }
    staged.gates[i] = static_cast<gate_idx_t>(arg.gates(i));
  }
  staged.ngates = arg.gates_size();
  return staged;
}

void Replicate::ApplyGates(const Gates &gates) {
  for (int i = 0; i < gates.ngates; i++) {
    gates_[i] = gates.gates[i];
  }
  ngates_ = gates.ngates;
}

template <typename Arg>
CommandResponse Replicate::SetGates(const Arg &arg) {
  auto staged = StageGates(arg);
  if (!staged) {
    return CommandFailure(EINVAL, "%s", staged.error().c_str());
  }
  ApplyGates(*staged);
  return CommandSuccess();
}

CommandResponse Replicate::Init(const bess::pb::ReplicateArg &arg) {
  CommandResponse err = SetGates(arg);
  if (err.error().code() != 0) {
    return err;
  }
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::ReplicateArg &value) { return StageGates(value); },
      [this](Gates &gates) { ApplyGates(gates); },
      [this] {
        bess::pb::ReplicateArg value;
        for (int i = 0; i < ngates_; i++) {
          value.add_gates(gates_[i]);
        }
        return value;
      },
      [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Replicate::DeInit() {
  settings_.Release();
}

void Replicate::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  for (bess::PacketRef tocopy : batch->packets()) {
    for (int j = 1; j < ngates_; j++) {
      bess::PacketRef newpkt(bess::PacketCopy(tocopy.handle()));
      if (newpkt.handle()) {
        EmitPacket(ctx, newpkt, gates_[j]);
      }
    }
    // The configured list is the module's whole contract: the original is
    // the first gate's packet, and the copies fill the rest. With no gate
    // configured there is no list to read, so the historical gate 0 stands.
    EmitPacket(ctx, tocopy, ngates_ > 0 ? gates_[0] : 0);
  }
}

ADD_MODULE(Replicate, "repl",
           "makes a copy of a packet and sends it out over n gates")
