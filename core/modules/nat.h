// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_NAT_H_
#define BESS_MODULES_NAT_H_

#include <atomic>
#include <memory>
#include <string>

#include "framework/module_requests.h"
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

  // One worker unless Init chooses the shared NAT (TP6): the default table is
  // worker-owned.
  NAT() : Module() { max_allowed_workers_ = 1; }
  ~NAT() override;

  CommandResponse Init(const bess::pb::NATArg &arg);
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &arg);
  CommandResponse GetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetRuntimeConfig(const bess::pb::EmptyArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  // returns the number of active NAT entries (flows)
  std::string GetDesc() const override;

 private:
  // Growth (TP5, D-078): the worker asks for a larger table, the control side
  // allocates it and hands it over here; the worker migrates and hands the old
  // table back to be freed. Neither allocation nor free happens on a worker.
  struct GrowRequest {
    uint64_t capacity;  // 0: free the retired table
  };
  void OnGrowRequest(const GrowRequest &request);
  template <typename N>
  void Translate(N &nat, Context *ctx, bess::PacketBatch *batch);

  // Exactly one is set: the owned NAT (one worker; fixed, or growable with
  // max_capacity) or the shared one.
  std::unique_ptr<bess::nat::Nat> nat_;
  std::unique_ptr<bess::nat::GrowableNat> growable_;
  std::unique_ptr<bess::nat::SharedNat> shared_;
  std::atomic<bess::nat::GrowableNat::Table *> handover_{nullptr};  // control -> worker
  std::atomic<bess::nat::GrowableNat::Table *> retired_{nullptr};   // worker -> control
  bess::framework::RequestEndpoint<GrowRequest> grow_;
  bess::framework::RequestEndpoint<GrowRequest> free_;
};

#endif  // BESS_MODULES_NAT_H_
