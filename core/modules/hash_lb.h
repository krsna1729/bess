// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_HASHLB_H_
#define BESS_MODULES_HASHLB_H_

#include <memory>
#include <vector>

#include "framework/exact_match_table.h"
#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_ptr.h"

using bess::framework::ExactMatchField;
using bess::framework::ExactMatchKey;
using bess::framework::ExactMatchKeyHash;
using bess::framework::ExactMatchTable;

// Splits packets across output gates by a hash of L2/L3/L4 or chosen fields.
//
// The configuration (mode, gate list, field layout) is one immutable object
// published through an RcuPtr: the "<name>/settings" resource (HashLBSettings,
// D-129) builds a replacement and publishes it while workers keep processing
// (G1.2 mode G; the configuration is small and changes rarely). A batch reads
// it once.
// Decision D-017 (docs/decisions.md)
class HashLB final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;
  // "settings" (HashLBSettings): mode, fields and gates, applied live by an
  // RCU swap (D-129).
  static const ResourceTables resource_tables;

  HashLB() : Module(), config_(init_context().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::HashLBArg &arg);
  void DeInit() override;

  std::string GetDesc() const override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  enum class Mode { kL2, kL3, kL4, kOther };
  static constexpr Mode kDefaultMode = Mode::kL4;
  static constexpr size_t kMaxGates = 16384;

  struct Config {
    Mode mode = kDefaultMode;
    std::vector<gate_idx_t> gates;
    // Built once per set_mode and never changed, so configurations share it.
    // No rules are ever added; it is only used for MakeKeys().
    std::shared_ptr<const ExactMatchTable<int>> fields_table =
        std::make_shared<const ExactMatchTable<int>>();
    ExactMatchKeyHash hasher{0};
    // What set it, as set (mode and fields as set_mode, gates as set_gates
    // got them): the settings resource's read-back.
    bess::pb::HashLBSettings settings;
  };

  template <Mode mode>
  inline void DoProcessBatch(Context *ctx, bess::PacketBatch *batch,
                             const Config &config);

  // A copy of the current configuration (or a default one before Init).
  Config Current() const;
  // Validates and applies a mode/field change to `config`.
  CommandResponse ApplyMode(const bess::pb::HashLBCommandSetModeArg &arg,
                            Config *config) const;
  // Validates every wire gate before narrowing it, then replaces the list.
  CommandResponse ApplyGates(const bess::pb::HashLBCommandSetGatesArg &arg,
                             Config *config) const;
  void Install(Config config);

  bess::rcu::RcuPtr<Config> config_;

  // "<name>/settings": stages a whole Config, swaps it in; after config_.
  bess::framework::Singleton<bess::pb::HashLBSettings,
                             std::unique_ptr<const Config>>
      settings_;
};

#endif  // BESS_MODULES_HASHLB_H_
