// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_NAT_H_
#define BESS_MODULES_NAT_H_

#include <memory>
#include <string>

#include "module.h"
#include "nat/nat.h"
#include "pb/module_msg.pb.h"

// NAT module (endpoint-independent NAPT, RFC 4787), a thin adapter over the
// nat library (M18, D-068). 2 igates and 2 ogates:
//   igate 0 / ogate 1: forward (internal -> external)
//   igate 1 / ogate 0: reverse (external -> internal)
// Packets are parsed with the checked conntrack parser (any VLAN tags, IPv4
// options); anything not translated is dropped.
class NAT final : public Module {
 public:
  static const gate_idx_t kNumIGates = 2;
  static const gate_idx_t kNumOGates = 2;

  static const Commands cmds;

  // One worker: the binding table is worker-owned.
  NAT() : Module() { max_allowed_workers_ = 1; }

  CommandResponse Init(const bess::pb::NATArg &arg);
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &arg);
  CommandResponse GetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetRuntimeConfig(const bess::pb::EmptyArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  // returns the number of active NAT entries (flows)
  std::string GetDesc() const override;

 private:
  std::unique_ptr<bess::nat::Nat> nat_;
};

#endif  // BESS_MODULES_NAT_H_
