// Copyright (c) 2018, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_STATIC_NAT_H_
#define BESS_MODULES_STATIC_NAT_H_

#include "module.h"
#include "pb/module_msg.pb.h"

#include <string>
#include <vector>

#include "utils/endian.h"

using bess::utils::be16_t;
using bess::utils::be32_t;

class StaticNAT : public Module {
 public:
  enum Direction {
    kForward = 0,  // internal -> external
    kReverse = 1,  // external -> internal
  };

  static const gate_idx_t kNumIGates = 2;
  static const gate_idx_t kNumOGates = 2;

  static const Commands cmds;

  CommandResponse Init(const bess::pb::StaticNATArg &arg);
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &arg);
  CommandResponse GetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetRuntimeConfig(const bess::pb::EmptyArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  struct NatPair {
    uint32_t int_addr;  // start address of internal address
    uint32_t ext_addr;  // start address of external address
    uint32_t size;      // [start_addr, start_addr + size) will be used
  };

  template <Direction dir>
  void DoProcessBatch(Context *ctx, bess::PacketBatch *batch);

  std::vector<NatPair> pairs_;
};

#endif  // BESS_MODULES_STATIC_NAT_H_
