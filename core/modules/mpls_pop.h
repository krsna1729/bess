// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_MPLSPOP_H_
#define BESS_MODULES_MPLSPOP_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../utils/ether.h"

using bess::utils::be16_t;

class MPLSPop final : public Module {
 public:
  static const gate_idx_t kNumIGates = 1;
  static const gate_idx_t kNumOGates = 2;

  static const Commands cmds;

  MPLSPop();  // constructor

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandSet(const bess::pb::MplsPopArg &arg);

 private:
  be16_t next_ether_type_;
  bool remove_eth_header_;
};

#endif  // BESS_MODULES_MPLSPOP_H_
