// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_L2FORWARD_H_
#define BESS_MODULES_L2FORWARD_H_

#include <atomic>
#include <bit>

#include "l2_table.h"
#include "module.h"
#include "pb/module_msg.pb.h"

static_assert(std::endian::native == std::endian::little,
              "L2Forward packs MAC addresses into integers assuming a "
              "little-endian target");

class L2Forward final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  L2Forward() : Module(), l2_table_(), default_gate_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::L2ForwardArg &arg);

  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandAdd(const bess::pb::L2ForwardCommandAddArg &arg);
  CommandResponse CommandDelete(const bess::pb::L2ForwardCommandDeleteArg &arg);
  CommandResponse CommandSetDefaultGate(
      const bess::pb::L2ForwardCommandSetDefaultGateArg &arg);
  CommandResponse CommandLookup(const bess::pb::L2ForwardCommandLookupArg &arg);
  CommandResponse CommandPopulate(
      const bess::pb::L2ForwardCommandPopulateArg &arg);

 private:
  struct l2_table l2_table_;
  // Set by commands while workers read it: an atomic field, relaxed on both
  // sides (the gate is independent of the table).
  std::atomic<gate_idx_t> default_gate_;
};

#endif  // BESS_MODULES_L2FORWARD_H_
