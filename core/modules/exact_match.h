// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_EXACTMATCH_H_
#define BESS_MODULES_EXACTMATCH_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "classifier/concurrent_exact.h"
#include "classifier/exact_rule_resource.h"
#include "classifier/table_schema.h"
#include "event.h"
#include "framework/resource_bindings.h"
#include "framework/singleton_resource.h"
#include "framework/table_fields.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_ptr.h"

using google::protobuf::RepeatedPtrField;
// (bess::framework::Error is the same alias; defined locally so this module
// does not depend on framework/exact_match_table.h.)
using Error = std::pair<int, std::string>;

// A rule's value is its output gate plus a result record. `result_fields`
// maps slices of that record to metadata attributes or packet offsets; the
// compiled ResultPlan places them on a hit. With no result fields the module
// is gate-only; a metadata field nothing downstream reads is compiled out of
// the plan (D-181). A miss takes the default gate; it also places
// `default_result` when the application configured one, and otherwise writes
// nothing.
class ExactMatch final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;
  // "rules" (ExactMatchRuleKey -> ExactMatchRuleValue) and "settings"
  // (ExactMatchSettings), both changed while workers run (D-129).
  static const ResourceTables resource_tables;
  // The packet path only reads the published generation and its rule table.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  // Module-level limits, preserved from the legacy ExactMatchTable-based
  // implementation for protobuf API compatibility. The classifier library
  // itself supports arbitrary widths; these bounds are ExactMatch's own.
  static constexpr size_t kMaxFields = 8;
  static constexpr size_t kMaxFieldSize = 8;
  static constexpr size_t kMaxKeyBytes = kMaxFields * kMaxFieldSize;

  ExactMatch()
      : Module(), published_(init_context().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  // What ProcessBatch decides, without placing: gates[i] is the output gate
  // of packet i and result_ids[i] the matched rule's record id (0 on a miss).
  // The same classification as the packet path, for tests and benchmarks
  // driven from a registered reader thread.
  void ClassifyBatch(bess::PacketBatch *batch, gate_idx_t *gates,
                     uint32_t *result_ids) const;

  // Tests (D-121): the published generation's fused-kernel key width (0 when
  // the generic path classifies), and a switch that republishes with the
  // fused kernel off or back on, so both paths run on the same rules.
  size_t fused_key_width() const;
  bool SetFusedKernelEnabled(bool enabled, Error *err);

  std::string GetDesc() const override;

  // Recompiles the extraction plan when metadata offsets are reassigned by a
  // pipeline-graph change. Returning 0 (rather than -ENOTSUP) keeps this
  // module registered for PreResume.
  int OnEvent(bess::Event event) override;

  CommandResponse Init(const bess::pb::ExactMatchArg &arg);
  void DeInit() override;

 private:
  // One configured rule: the field values to match (in field order) and the
  // gate that matching packets go to.
  struct Rule {
    // Per-field raw bytes, exactly as decoded from the protobuf (value_bin
    // verbatim, value_int little-endian). Mirrors the legacy
    // ExactMatchRuleFields: rule bytes are stored WITHOUT applying the
    // configured mask, while packet/metadata bytes ARE masked on extraction.
    std::vector<std::vector<uint8_t>> fields;
    // The gate the rule forwards to and its record id (packed; gate.h).
    uint64_t value;
  };

  // The configuration a batch runs against: the compiled extraction plan, the
  // default gate, and the rule table. Immutable once published, but for the
  // default gate, which the "settings" resource stores in place; the rest
  // changes only when configuration does (metadata offsets, a rule-table
  // rebuild, or table growth).
  //
  // Rules are NOT part of a generation (G1.2 mode C). They live in a
  // ConcurrentExactTable -- DPDK's lock-free rte_hash under the runtime
  // QSBR -- that add/delete update in place, O(1), while workers keep
  // reading. The earlier design rebuilt the whole generation per rule
  // (7.9 ms per add at 100K rules). Generations share the table.
  struct Generation {
    Generation(gate_idx_t d,
               std::shared_ptr<bess::classifier::ConcurrentExactTable> t,
               bess::classifier::CompiledKey k,
               bess::classifier::CompiledResult r)
        : default_gate(d),
          table(std::move(t)),
          key(std::move(k)),
          result(std::move(r)) {}

    // Read once per batch (relaxed): the "settings" resource stores it in the
    // published generation while workers run, with nothing to allocate or
    // retire (D-129). A rebuild carries the current value over (D-110).
    mutable std::atomic<gate_idx_t> default_gate{DROP_GATE};
    // Shared, updated in place; owned by every generation that maps it (a
    // retired generation keeps a replaced table alive for its grace period).
    std::shared_ptr<bess::classifier::ConcurrentExactTable> table;
    // The extraction (invalid in a fail-closed generation: the packet path
    // then routes everything to the default gate instead of reading an
    // incorrect metadata region) and the result placement, with the offsets
    // each baked, compared at PreResume.
    bess::classifier::CompiledKey key;
    bess::classifier::CompiledResult result;
    // The fused fixed-layout kernel (D-121): the key is one packet field of
    // 1, 2, 4 or 8 bytes, read, masked, hashed and looked up in one pass.
    // fused_width is that width, 0 for the generic extract-then-lookup path;
    // the field starts at packet byte fused_offset, and fused_mask holds its
    // mask bytes (all ones when unmasked).
    size_t fused_width = 0;
    size_t fused_offset = 0;
    uint64_t fused_mask = 0;
  };

  using GenerationPtr = std::unique_ptr<const Generation>;

  // The packet path's per-batch decision, shared by ProcessBatch and
  // ClassifyBatch: calls emit(i, value, hit) once per packet, in order, where
  // `value` is the matched rule's packed {gate, record id} and `hit` says
  // whether it matched. `emit` is inlined into each caller.
  template <typename Emit>
  void Classify(bess::PacketBatch *batch, const Generation &gen,
                Emit &&emit) const;

  CommandResponse AddFieldOne(const bess::pb::Field &field,
                              const bess::pb::FieldData &mask, int idx);
  // Turns a rule's protobuf fields into `rule`'s field list, validating the
  // count against the module's configured fields.
  Error RuleFieldsFromPb(const RepeatedPtrField<bess::pb::FieldData> &fields,
                         std::vector<std::vector<uint8_t>> *rule);

  // Builds a generation over `table`; nullptr with *err set on failure. Runs
  // on the control plane, off the data path. Cost is the plan, not the rules.
  GenerationPtr Build(
      std::shared_ptr<bess::classifier::ConcurrentExactTable> table,
      gate_idx_t default_gate, Error *err);
  // Packs a rule's fields into the dense table key, validating each field's
  // size against the configuration.
  Error PackKey(const std::vector<std::vector<uint8_t>> &fields,
                std::vector<std::byte> *key) const;
  // A new, empty table sized for `rules` entries plus headroom (D-010),
  // admitted as "rules" before it is built (D-198; refusal throws
  // MemoryRefused). The returned pointer owns the admission too: it is
  // released when the last generation mapping the table is reclaimed.
  std::expected<std::shared_ptr<bess::classifier::ConcurrentExactTable>, Error>
  NewTable(size_t rules) const;
  // A table for at least `rules` rules, filled by `fill` (false = an insert
  // hit kFull; the table is then rebuilt at twice the size).
  std::expected<std::shared_ptr<bess::classifier::ConcurrentExactTable>, Error>
  FillTable(size_t rules,
            const std::function<bool(bess::classifier::ConcurrentExactTable &)>
                &fill) const;
  // Grows the table (a larger copy, published as a new generation) when one
  // more insert would eat into the grace-period headroom, or unconditionally
  // with `force`. Amortized O(1) per insert.
  bool EnsureCapacity(bool force, Error *err);
  // Field i's extraction mask: the configured mask in the field's byte order,
  // none when it is the default all-ones (D-121 reads the mask back).
  bool FieldMask(size_t i, bess::classifier::Normalization *out,
                 Error *err) const;
  // Fail-closed generation for unrecoverable (re)build failures on the resume
  // path: same rules/default for introspection, but extraction disabled so
  // the packet path routes everything to the default gate.
  GenerationPtr BuildDegraded(
      std::shared_ptr<bess::classifier::ConcurrentExactTable> table,
      gate_idx_t default_gate);
  // Rebuilds and republishes the generation when metadata offsets changed
  // under it (graph reconfiguration + resume). Runs on the control thread
  // with workers paused. Never leaves a stale plan reading a reassigned
  // metadata region: refresh failure publishes a fail-closed generation.
  void RefreshForResume();
  // Field configuration, fixed at Init() time; every generation's plan gets
  // it, so a rebuild reproduces the module's matching exactly.
  bess::framework::KeyFields keys_{kMaxFields, kMaxFieldSize};
  // Per field, the raw configured mask (0 == the default all-ones).
  std::vector<uint64_t> field_masks_;

  // Result fields (fixed at Init): each maps a slice of a rule's record to a
  // metadata attribute or a packet offset, plus the records and the default
  // result placed on a miss. Empty: gate-only.
  bess::framework::ResultFields results_;
  std::optional<uint64_t> initial_default_gate_;  // as given at creation
  // Whether Build() may select the fused kernel (tests turn it off).
  bool fused_enabled_ = true;

  // Publication and reclamation (bess::rcu::RcuPtr + the runtime's RcuDomain):
  // one acquire load per batch on the data path, serialized rebuilds off it,
  // and the retired generation is destroyed by a control thread rather than on
  // a packet worker.
  // Never null between Init() and module destruction (DeInit() leaves it: the
  // generation is released with the module, workers already paused), and
  // every writer publishes a replacement rather than clearing it.
  bess::rcu::RcuPtr<Generation> published_;

  // The table the command path writes (always the current generation's).
  std::shared_ptr<bess::classifier::ConcurrentExactTable> table_;

  // table_ as the transactional resource "<module name>/rules" (D-022),
  // registered from Init() to DeInit().
  std::unique_ptr<bess::classifier::ExactRuleResource> resource_;
  bess::framework::ResourceBinding binding_;  // after resource_ (D-044)
  // ExactMatchSettings as the singleton resource "<module name>/settings"
  // (D-129), registered from Init() to DeInit(); after published_, whose
  // generation's default gate it sets.
  bess::framework::Singleton<bess::pb::ExactMatchSettings, gate_idx_t>
      settings_;
};

#endif  // BESS_MODULES_EXACTMATCH_H_
