// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_WILDCARDMATCH_H_
#define BESS_MODULES_WILDCARDMATCH_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "classifier/concurrent_masked.h"
#include "classifier/masked_rule_resource.h"
#include "classifier/table_schema.h"
#include "event.h"
#include "framework/resource_bindings.h"
#include "framework/singleton_resource.h"
#include "framework/table_fields.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_ptr.h"

using google::protobuf::RepeatedPtrField;
using Error = std::pair<int, std::string>;

// Multi-field classifier with wildcard (ternary) matching.
//
// Matching is tuple-space search: one tuple per distinct rule mask, each an
// exact table over the masked field values; the highest priority wins, and
// among equal priorities the later command. Rules live in a
// classifier::ConcurrentMaskedTable (G1.2 mode C): add/delete/clear change it
// in place while workers keep reading, with no rebuild (Decision D-014). Extraction is a dense `field0 || field1 || ...` key produced by a
// compiled ExtractPlan -- no 8-byte-word padding, which was an artifact of the
// legacy `wm_hkey_t` storage and never affected matching (the legacy mask
// zeroed those bytes).
//
// A generation holds the configuration around the table -- the compiled
// extraction plan and the default gate -- plus a shared pointer to the table.
// It is republished only when that configuration changes (default gate,
// metadata offsets, a restore).
class WildcardMatch final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;
  // "<module>/rules" (WildcardMatchRuleKey -> WildcardMatchRuleValue) and
  // "<module>/settings" (WildcardMatchSettings), D-129. Neither requires
  // paused workers: rules change in place, the default gate is published.
  static const ResourceTables resource_tables;
  // The packet path reads the published generation and the rule table only.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kNone,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  // Module-level limits. The classifier library supports arbitrary widths and
  // tuple counts; these bounds are WildcardMatch's own wire compatibility.
  static constexpr size_t kMaxFields = 8;
  static constexpr size_t kMaxFieldSize = 8;
  static constexpr size_t kMaxKeyBytes = kMaxFields * kMaxFieldSize;
  static constexpr size_t kDefaultMaxTuples = 8;  // master's MAX_TUPLES
  static constexpr size_t kMaxTuplesLimit = 1024;  // max_tuples' upper bound

  WildcardMatch() : Module(), published_(init_context().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  void DeInit() override;

  // What ProcessBatch decides, without emitting: gates[i] is the output gate
  // of packet i and result_ids[i] is the matched rule's record id (0 on a
  // miss, and when the module has no result fields). The same code as the
  // packet path, for tests and benchmarks that drive the module from a
  // registered reader thread.
  void ClassifyBatch(bess::PacketBatch *batch, gate_idx_t *gates,
                     uint32_t *result_ids) const;

  std::string GetDesc() const override;

  // Recompiles the extraction plan when metadata offsets are reassigned by a
  // pipeline-graph change. Returning 0 (rather than -ENOTSUP) keeps this
  // module registered for PreResume.
  int OnEvent(bess::Event event) override;

  CommandResponse Init(const bess::pb::WildcardMatchArg &arg);

 private:
  // One configured rule: per-field value and mask bytes (exactly as decoded
  // from the protobuf, field-sized) plus its ranges, rank and gate. Field
  // bytes are kept per field so introspection reproduces the legacy byte
  // layout verbatim; the dense packed key is derived at build time.
  struct Rule {
    std::vector<std::vector<uint8_t>> values;
    std::vector<std::vector<uint8_t>> masks;
    // Canonical (D-186): in field order, at most one per field, none over a
    // field's whole domain; offsets and widths in the dense key.
    std::vector<bess::classifier::ConcurrentMaskedTable::KeyRange> ranges;
    int64_t priority = 0;
    gate_idx_t gate = DROP_GATE;
  };

  // The configuration a batch runs against. Immutable once published; the
  // rules themselves are in `table`, which is updated in place.
  struct Generation {
    Generation(gate_idx_t d,
               std::shared_ptr<bess::classifier::ConcurrentMaskedTable> t,
               bess::classifier::CompiledKey k,
               bess::classifier::CompiledResult r)
        : default_gate(d),
          table(std::move(t)),
          key(std::move(k)),
          result(std::move(r)) {}

    gate_idx_t default_gate = DROP_GATE;
    // Shared, updated in place; owned by every generation that maps it.
    std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table;
    // The extraction (invalid in a fail-closed generation: the packet path
    // then routes everything to the default gate instead of reading an
    // incorrect metadata region) and the result placement, with the offsets
    // each baked, compared at PreResume.
    bess::classifier::CompiledKey key;
    bess::classifier::CompiledResult result;
  };

  using GenerationPtr = std::unique_ptr<const Generation>;

  // Calls emit(i, gate, result_id) once per packet, in order. With
  // `with_result` (and result fields configured) a hit's record id is read
  // from its rule; otherwise a hit's result_id is 0 and no rule record is
  // touched. A miss's result_id is `miss_result_id`.
  template <typename Emit>
  void Classify(bess::PacketBatch *batch, bool with_result, Emit &&emit,
                uint32_t miss_result_id = 0) const;

  CommandResponse AddFieldOne(const bess::pb::Field &field, int idx);
  // Turns a rule's protobuf fields into `rule`'s per-field byte vectors.
  Error RuleFieldsFromPb(const RepeatedPtrField<bess::pb::FieldData> &fields,
                         size_t field_size, std::vector<std::vector<uint8_t>> *out);
  // Turns a command argument into a `Rule`, validating gate, field count, and
  // value/mask lengths.
  Error RuleFromPb(const bess::pb::WildcardMatchCommandAddArg &arg, Rule *rule);
  // Packs a rule's per-field values and masks densely in field order (the
  // legacy table stored rule bytes verbatim; so does this).
  Error PackRule(const Rule &rule, std::vector<std::byte> *value,
                 std::vector<std::byte> *mask) const;
  // A new, empty rule table.
  std::expected<std::shared_ptr<bess::classifier::ConcurrentMaskedTable>,
                Error>
  NewTable() const;
  // Builds a generation around `table`; nullptr with *err set on failure.
  // Runs on the control plane, off the data path.
  GenerationPtr Build(
      std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table,
      gate_idx_t default_gate, Error *err);
  // Fail-closed generation for unrecoverable (re)build failures on the resume
  // path: same rules/default for introspection, but extraction disabled so the
  // packet path routes everything to the default gate.
  GenerationPtr BuildDegraded(
      std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table,
      gate_idx_t default_gate);
  // Rebuilds and republishes the generation when metadata offsets changed
  // under it (graph reconfiguration + resume). Runs on the control thread with
  // workers paused. Never leaves a stale plan reading a reassigned metadata
  // region: refresh failure publishes a fail-closed generation.
  void RefreshForResume();

  // Field configuration, fixed at Init() time; every generation's plan gets
  // it, so a rebuild reproduces the module's matching exactly.
  bess::framework::KeyFields keys_{kMaxFields, kMaxFieldSize};
  // Distinct masks the table admits (Init's max_tuples); the writer checks
  // it, the packet path never reads it.
  size_t max_tuples_ = kDefaultMaxTuples;
  std::optional<uint64_t> initial_default_gate_;  // as given at creation

  // Result fields (fixed at Init): each maps a slice of a rule's record to a
  // metadata attribute or a packet offset, plus the records and the default
  // result. Empty: gate-only.
  bess::framework::ResultFields results_;

  // The live rule table (also held by the published generation).
  std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table_;

  // table_ as the transactional resource "<module name>/rules" (D-024),
  // registered from Init() to DeInit().
  std::unique_ptr<bess::classifier::MaskedRuleResource> resource_;
  bess::framework::ResourceBinding binding_;  // after resource_ (D-044)

  // Publication and reclamation (bess::rcu::RcuPtr + the runtime's RcuDomain):
  // one acquire load per batch on the data path, serialized rebuilds off it,
  // and the retired generation is destroyed by a control thread rather than on
  // a packet worker.
  // Never null between Init() and module destruction: there is no DeInit()
  // (the generation is released with the module, workers already paused), and
  // every writer publishes a replacement rather than clearing it.
  bess::rcu::RcuPtr<Generation> published_;

  // "<module>/settings" (D-129): the default gate. Stage builds the
  // replacement generation; apply publishes it. After
  // published_, which it touches.
  bess::framework::Singleton<bess::pb::WildcardMatchSettings, GenerationPtr>
      settings_;
};

#endif  // BESS_MODULES_WILDCARDMATCH_H_
