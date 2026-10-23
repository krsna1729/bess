// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_MPLSPOP_H_
#define BESS_MODULES_MPLSPOP_H_

#include <expected>
#include <string>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "utils/ether.h"

using bess::utils::be16_t;

class MPLSPop final : public Module {
 public:
  static const gate_idx_t kNumIGates = 1;
  static const gate_idx_t kNumOGates = 2;

  static const Commands cmds;
  static const ResourceTables resource_tables;
  // The packet path only reads the control-written settings.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  MPLSPop();  // constructor

  // Hides Module::Init(EmptyArg) on purpose: ADD_MODULE binds the class's own.
  // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method)
  CommandResponse Init(const bess::pb::EmptyArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  be16_t next_ether_type_;
  bool remove_eth_header_;

  struct Settings {
    be16_t next_ether_type;
    bool remove_eth_header;
  };
  // "<name>/settings" (D-129): what `set` sets, paused workers only.
  bess::framework::Singleton<bess::pb::MplsPopArg, Settings> settings_;
};

#endif  // BESS_MODULES_MPLSPOP_H_
