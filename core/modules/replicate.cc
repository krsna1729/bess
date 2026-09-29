// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "replicate.h"

const Commands Replicate::cmds = {
    {"set_gates", "ReplicateCommandSetGatesArg",
     MODULE_CMD_FUNC(&Replicate::CommandSetGates), Command::THREAD_UNSAFE},
};

// Validates every wire gate (int64) before anything changes: each must name
// one of this module's output gates. They used to be narrowed unchecked.
template <typename Arg>
CommandResponse Replicate::SetGates(const Arg &arg) {
  if (arg.gates_size() > kMaxGates) {
    return CommandFailure(EINVAL, "no more than %d gates", kMaxGates);
  }
  for (int i = 0; i < arg.gates_size(); i++) {
    if (arg.gates(i) < 0 || arg.gates(i) >= kNumOGates) {
      return CommandFailure(EINVAL, "gate %lld is out of range (0..%d)",
                            static_cast<long long>(arg.gates(i)),
                            kNumOGates - 1);
    }
  }
  for (int i = 0; i < arg.gates_size(); i++) {
    gates_[i] = static_cast<gate_idx_t>(arg.gates(i));
  }
  ngates_ = arg.gates_size();
  return CommandSuccess();
}

CommandResponse Replicate::Init(const bess::pb::ReplicateArg &arg) {
  return SetGates(arg);
}

CommandResponse Replicate::CommandSetGates(
    const bess::pb::ReplicateCommandSetGatesArg &arg) {
  return SetGates(arg);
}

void Replicate::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef tocopy = batch->packet(i);
    for (int j = 1; j < ngates_; j++) {
      bess::PacketRef newpkt(bess::PacketCopy(tocopy.handle()));
      if (newpkt.handle()) {
        EmitPacket(ctx, newpkt, gates_[j]);
      }
    }
    EmitPacket(ctx, tocopy, 0);
  }
}

ADD_MODULE(Replicate, "repl",
           "makes a copy of a packet and sends it out over n gates")
