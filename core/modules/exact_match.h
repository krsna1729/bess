// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_EXACTMATCH_H_
#define BESS_MODULES_EXACTMATCH_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../classifier/backend.h"
#include "../classifier/concurrent_exact.h"
#include "../classifier/exact_rule_resource.h"
#include "../classifier/extract_plan.h"
#include "../classifier/runtime_schema.h"
#include "../event.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../rcu/rcu_ptr.h"

using google::protobuf::RepeatedPtrField;
// (bess::framework::Error is the same alias; defined locally so this module
// no longer depends on framework/exact_match_table.h, which HashLB still uses.)
using Error = std::pair<int, std::string>;

// Two modes, fixed at Init():
//
//  - gate mode (the default): a rule's value is the output gate a matching
//    packet leaves on.
//  - action mode (`action_resource` set): a rule's value is the ActionId of a
//    per-session action, which the packet path writes into the `action_id`
//    metadata attribute and forwards on gate 0, so the graph continues into
//    an ActionTable. The rules are then a resource that references the
//    actions, and one transaction can create the session's meter, next hop,
//    route, action and rule together (G1.2b). A miss still takes the default
//    gate.
class ExactMatch final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  // Where a match goes in action mode: the module's first output gate, which
  // the graph connects to the ActionTable.
  static constexpr gate_idx_t kActionGate = 0;

  static const Commands cmds;

  // Whether rules name actions rather than gates (see the class comment).
  bool action_mode() const noexcept { return action_mode_; }

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

  // Gate-mode decision without emitting, for tests and benchmarks driven
  // from a registered reader thread. Action ids are not gate_idx_t values.
  void ClassifyBatch(bess::PacketBatch *batch, gate_idx_t *gates) const;
  // Action-mode matched ids; zero for a miss (no action to forward).
  void ClassifyActionsBatch(bess::PacketBatch *batch,
                            uint32_t *action_ids) const;

  std::string GetDesc() const override;

  // Recompiles the extraction plan when metadata offsets are reassigned by a
  // pipeline-graph change. Returning 0 (rather than -ENOTSUP) keeps this
  // module registered for PreResume.
  int OnEvent(bess::Event event) override;

  CommandResponse Init(const bess::pb::ExactMatchArg &arg);
  void DeInit() override;
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &arg);
  CommandResponse GetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetRuntimeConfig(const bess::pb::ExactMatchConfig &arg);
  CommandResponse CommandAdd(const bess::pb::ExactMatchCommandAddArg &arg);
  CommandResponse CommandDelete(
      const bess::pb::ExactMatchCommandDeleteArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);
  CommandResponse CommandSetDefaultGate(
      const bess::pb::ExactMatchCommandSetDefaultGateArg &arg);

 private:
  // One configured rule: the field values to match (in field order) and the
  // gate that matching packets go to.
  struct Rule {
    // Per-field raw bytes, exactly as decoded from the protobuf (value_bin
    // verbatim, value_int little-endian). Mirrors the legacy
    // ExactMatchRuleFields: rule bytes are stored WITHOUT applying the
    // configured mask, while packet/metadata bytes ARE masked on extraction.
    std::vector<std::vector<uint8_t>> fields;
    // The gate the rule forwards to, or the action id it names (action mode).
    uint64_t value;
  };

  // The configuration a batch runs against: the compiled extraction plan, the
  // default gate, and the rule table. Immutable once published; it changes
  // only when configuration does (default gate, metadata offsets, a restore,
  // or table growth).
  //
  // Rules are NOT part of a generation (G1.2 mode C). They live in a
  // ConcurrentExactTable -- DPDK's lock-free rte_hash under the runtime
  // QSBR -- that add/delete update in place, O(1), while workers keep
  // reading. The earlier design rebuilt the whole generation per rule
  // (7.9 ms per add at 100K rules). Generations share the table.
  struct Generation {
    Generation(gate_idx_t d, bess::classifier::ExtractPlan e,
               std::shared_ptr<bess::classifier::ConcurrentExactTable> t,
               size_t k, bool valid,
               std::vector<std::vector<std::byte>> masks,
               std::vector<size_t> offsets)
        : default_gate(d),
          extract(std::move(e)),
          table(std::move(t)),
          key_size(k),
          extraction_valid(valid),
          converted_masks(std::move(masks)),
          baked_source_offsets(std::move(offsets)) {}

    gate_idx_t default_gate = DROP_GATE;
    bess::classifier::ExtractPlan extract;
    // Shared, updated in place; owned by every generation that maps it (a
    // retired generation keeps a replaced table alive for its grace period).
    std::shared_ptr<bess::classifier::ConcurrentExactTable> table;
    // Dense packed key width: sum of field sizes. Also the extraction stride.
    size_t key_size = 0;
    // False when metadata offsets were invalid at (re)build time. The packet
    // path then routes everything to the default gate instead of reading an
    // incorrect metadata region (fail-closed).
    bool extraction_valid = true;
    // Per-field converted mask bytes (legacy ExactMatchField::mask byte
    // order), for GetInitialArg serialization.
    std::vector<std::vector<std::byte>> converted_masks;
    // Per-field source offset baked into `extract` (packet byte offset, or
    // metadata attr offset, or kInvalidOffset for unreadable metadata).
    // Compared at PreResume to detect graph-driven offset reassignment.
    std::vector<size_t> baked_source_offsets;
  };

  using GenerationPtr = std::unique_ptr<const Generation>;

  // Sentinel for a metadata field whose attribute currently has no valid
  // physical offset (e.g. orphan reader, out of space).
  static constexpr size_t kInvalidOffset = static_cast<size_t>(-1);

  // The packet path's per-batch decision, shared by ProcessBatch and
  // ClassifyBatch: calls emit(i, value, hit) once per packet, in order, where
  // `value` is the matched rule's value (a gate in gate mode, an action id in
  // action mode) and `hit` says whether it matched. `emit` is inlined into
  // each caller.
  template <typename Emit>
  void Classify(bess::PacketBatch *batch, const Generation &gen,
                Emit &&emit) const;

  CommandResponse AddFieldOne(const bess::pb::Field &field,
                              const bess::pb::FieldData &mask, int idx);
  // Turns a rule's protobuf fields into `rule`'s field list, validating the
  // count against the module's configured fields.
  Error RuleFieldsFromPb(const RepeatedPtrField<bess::pb::FieldData> &fields,
                         std::vector<std::vector<uint8_t>> *rule);
  // The value a rule command carries: the gate (gate mode) or the action id
  // (action mode), refusing the field the mode does not use.
  Error RuleValueFromPb(const bess::pb::ExactMatchCommandAddArg &arg,
                        uint64_t *value) const;
  // Turns a command argument into a `Rule`, validating value and fields.
  Error RuleFromPb(const bess::pb::ExactMatchCommandAddArg &arg, Rule *rule);
  // Replaces the published generation with `build(current)`, or leaves the
  // active one alone when the builder returns nullptr (with *err set). Runs
  // only on the control plane, where command and graph mutations are
  // serialized; never on a packet worker.
  bool Publish(const std::function<GenerationPtr(const Generation &)> &build,
               Error *err);

  // Builds a generation over `table`; nullptr with *err set on failure. Runs
  // on the control plane, off the data path. Cost is the plan, not the rules.
  GenerationPtr Build(
      std::shared_ptr<bess::classifier::ConcurrentExactTable> table,
      gate_idx_t default_gate, Error *err);
  // Packs a rule's fields into the dense table key, validating each field's
  // size against the configuration.
  Error PackKey(const std::vector<std::vector<uint8_t>> &fields,
                std::vector<std::byte> *key) const;
  // A new, empty table sized for `rules` entries plus headroom (D-010).
  std::expected<std::shared_ptr<bess::classifier::ConcurrentExactTable>, Error>
  NewTable(size_t rules) const;
  // Grows the table (a larger copy, published as a new generation) when one
  // more insert would eat into the grace-period headroom, or unconditionally
  // with `force`. Amortized O(1) per insert.
  // A table for at least `rules` rules, filled by `fill` (false = an insert
  // hit kFull; the table is then rebuilt at twice the size).
  std::expected<std::shared_ptr<bess::classifier::ConcurrentExactTable>, Error>
  FillTable(size_t rules,
            const std::function<bool(bess::classifier::ConcurrentExactTable &)>
                &fill) const;
  bool EnsureCapacity(bool force, Error *err);
  // Per-field key layout resolved from the module's FieldSpecs and the
  // current metadata offsets.
  struct KeyLayout {
    size_t key_size = 0;
    std::vector<bess::classifier::RuntimeKeyField> key_fields;
    // Converted mask bytes per field, in legacy ExactMatchField::mask byte
    // order (for GetInitialArg serialization).
    std::vector<std::vector<std::byte>> converted_masks;
    // Source offset baked into the plan per field, or kInvalidOffset for
    // unreadable metadata.
    std::vector<size_t> baked_offsets;
    // False when a metadata field has no valid physical offset.
    bool metadata_valid = true;
  };
  // Resolves FieldSpecs into a dense packed layout using current metadata
  // offsets. With `tolerate_invalid_metadata`, unreadable metadata marks the
  // layout invalid (fail-closed path) instead of failing.
  bool ComputeLayout(bool tolerate_invalid_metadata, KeyLayout *layout,
                     Error *err);  // Fail-closed generation for unrecoverable (re)build failures on the resume
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
  struct FieldSpec {
    bool by_offset = true;
    int offset = 0;             // valid when by_offset
    std::string attr_name;      // otherwise (for GetInitialArg)
    int attr_id = -1;           // resolved once at Init() when !by_offset
    int size = 0;
    uint64_t mask = 0;          // raw configured mask (0 == default all-ones)
    size_t mask_bin_len = 0;    // value_bin length; 0 for value_int/empty
  };
  std::vector<FieldSpec> field_specs_;
  bool empty_masks_;  // mainly for GetInitialArg

  // Action mode (see the class comment): rules name actions, and the packet
  // path writes the matched action id into `action_id`.
  bool action_mode_ = false;
  std::string action_resource_;
  int action_id_attr_ = -1;

  // Publication and reclamation (bess::rcu::RcuPtr + the runtime's RcuDomain):
  // one acquire load per batch on the data path, serialized rebuilds off it,
  // and the retired generation is destroyed by a control thread rather than on
  // a packet worker.
  // Never null between Init() and module destruction (DeInit() leaves it: the
  // generation is released with the module, workers already paused), and
  // every command publishes a replacement rather than clearing it.
  bess::rcu::RcuPtr<Generation> published_;

  // The table the command path writes (always the current generation's).
  std::shared_ptr<bess::classifier::ConcurrentExactTable> table_;

  // table_ as the transactional resource "<module name>/rules" (D-022),
  // registered from Init() to DeInit().
  std::unique_ptr<bess::classifier::ExactRuleResource> resource_;
};

#endif  // BESS_MODULES_EXACTMATCH_H_
