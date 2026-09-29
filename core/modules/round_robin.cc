// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "round_robin.h"

const Commands RoundRobin::cmds = {
    {"set_mode", "RoundRobinCommandSetModeArg",
     MODULE_CMD_FUNC(&RoundRobin::CommandSetMode), Command::THREAD_UNSAFE},
    {"set_gates", "RoundRobinCommandSetGatesArg",
     MODULE_CMD_FUNC(&RoundRobin::CommandSetGates), Command::THREAD_UNSAFE},
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

  return CommandSuccess();
}

CommandResponse RoundRobin::CommandSetMode(
    const bess::pb::RoundRobinCommandSetModeArg &arg) {
  if (arg.mode() == "packet") {
    per_packet_ = 1;
  } else if (arg.mode() == "batch") {
    per_packet_ = 0;
  } else {
    return CommandFailure(EINVAL,
                          "argument must be either 'packet' or 'batch'");
  }
  return CommandSuccess();
}

CommandResponse RoundRobin::CommandSetGates(
    const bess::pb::RoundRobinCommandSetGatesArg &arg) {
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
  return CommandSuccess();
}

void RoundRobin::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (ngates_ <= 0) {
    bess::PacketFreeBatch(batch);
    return;
  }

  if (per_packet_) {
    int cnt = batch->cnt();
    for (int i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
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
