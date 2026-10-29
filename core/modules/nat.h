// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_NAT_H_
#define BESS_MODULES_NAT_H_

#include <atomic>
#include <memory>
#include <string>
#include <variant>

#include "framework/memory_scope.h"
#include "framework/module_requests.h"
#include "module.h"
#include "nat/nat.h"
#include "pb/module_msg.pb.h"
#include "stats/event_throttle.h"

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

  // Shared: every worker changes one synchronized binding table. Otherwise one
  // worker owns the bindings.
  static ModuleCapabilities Capabilities(const bess::pb::NATArg &arg);
  CommandResponse Init(const bess::pb::NATArg &arg);
  // Usage (TP7, D-083): ask for an interim record of every mapping; drain
  // the records (interim and final) the packet path has logged.
  CommandResponse CommandRequestUsageReport(const bess::pb::EmptyArg &arg);
  CommandResponse CommandDrainUsage(const bess::pb::EmptyArg &arg);

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
  void Run(N &nat, Context *ctx, bess::PacketBatch *batch);
  template <typename N>
  void Translate(N &nat, Context *ctx, bess::PacketBatch *batch);
  template <typename N>
  CommandResponse Make(typename N::Config &config);
  // Owned growth: frees a table in flight (nullptr: nothing) and gives its
  // admitted bytes back.
  void FreeTable(void *table) noexcept;

  // The engine the arguments chose: owned (fixed, or growable with
  // max_capacity) or shared, each with or without usage counters. Admitted as
  // "<name>/bindings" (D-198), which also carries every growth.
  using Engine = std::variant<std::monostate, bess::framework::Sized<bess::nat::Nat>,
                              bess::framework::Sized<bess::nat::GrowableNat>,
                              bess::framework::Sized<bess::nat::SharedNat>,
                              bess::framework::Sized<bess::nat::CountedNat>,
                              bess::framework::Sized<bess::nat::CountedGrowableNat>,
                              bess::framework::Sized<bess::nat::CountedSharedNat>>;
  Engine engine_;
  // Owned growth: tables in flight, of the engine's table type (freed with
  // free_table_, which knows it and gives its bytes back to table_growth_,
  // the engine's lease).
  std::atomic<void *> handover_{nullptr};  // control -> worker
  std::atomic<void *> retired_{nullptr};   // worker -> control
  void (*free_table_)(bess::utils::MemoryGrowth *, void *) = nullptr;
  bess::utils::MemoryGrowth *table_growth_ = nullptr;
  bess::framework::RequestEndpoint<GrowRequest> grow_;
  bess::framework::RequestEndpoint<GrowRequest> free_;
  // Bindings refused (M25, D-089): the table (or its wheel) full, or no free
  // external port.
  bess::stats::EventThrottle table_full_;
  bess::stats::EventThrottle ports_exhausted_;
};

#endif  // BESS_MODULES_NAT_H_
