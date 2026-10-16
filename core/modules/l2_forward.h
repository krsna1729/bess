// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_L2FORWARD_H_
#define BESS_MODULES_L2FORWARD_H_

#include <atomic>
#include <bit>
#include <memory>

#include "dataplane/table_policy.h"
#include "framework/resource_bindings.h"
#include "framework/singleton_resource.h"
#include "l2/mac_table_resource.h"
#include "l2/packed_mac_table.h"
#include "module.h"
#include "pb/module_msg.pb.h"

static_assert(std::endian::native == std::endian::little,
              "L2Forward packs MAC addresses into integers assuming a "
              "little-endian target");

class L2Forward final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;
  static const ResourceTables resource_tables;

  L2Forward() : Module(), default_gate_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::L2ForwardArg &arg);

  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  // ProcessBatch's decision: gates[i], the output gate of packet i.
  void ClassifyBatch(const bess::PacketBatch *batch, gate_idx_t *gates) const;

  CommandResponse CommandLookup(const bess::pb::L2ForwardCommandLookupArg &arg);
  CommandResponse CommandPopulate(
      const bess::pb::L2ForwardCommandPopulateArg &arg);

 private:
  // One-word slots: commands write one at a time (the control thread runs
  // them in turn) while every worker reads without a lock (table policy,
  // user decision 1; D-017's guarantees). Value: gate + 1, so 0 is a miss.
  // No cold word (an empty type allocates none).
  struct NoCold {};
  using Table = bess::l2::PackedMacTable<NoCold, bess::dataplane::SingleWriter>;
  std::unique_ptr<Table> table_;
  // "<name>/macs" (D-114): the same table, transactionally.
  std::unique_ptr<bess::l2::MacTableResource<Table>> resource_;
  bess::framework::ResourceBinding binding_;  // after resource_ (D-044)
  // Set by commands and "<name>/settings" while workers read it: an atomic
  // field, relaxed on both sides (the gate is independent of the table).
  std::atomic<gate_idx_t> default_gate_;
  // "<name>/settings" (D-129): the default gate. After default_gate_.
  bess::framework::Singleton<bess::pb::L2ForwardSettings, gate_idx_t> settings_;
};

#endif  // BESS_MODULES_L2FORWARD_H_
