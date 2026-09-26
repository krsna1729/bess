// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_MODULES_WILDCARDMATCH_H_
#define BESS_MODULES_WILDCARDMATCH_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../classifier/extract_plan.h"
#include "../classifier/concurrent_masked.h"
#include "../classifier/runtime_schema.h"
#include "../control/runtime_state.h"
#include "../event.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../rcu/rcu_ptr.h"

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

  // Module-level limits. The classifier library supports arbitrary widths and
  // tuple counts; these bounds are WildcardMatch's own wire compatibility.
  static constexpr size_t kMaxFields = 8;
  static constexpr size_t kMaxFieldSize = 8;
  static constexpr size_t kMaxKeyBytes = kMaxFields * kMaxFieldSize;
  static constexpr size_t kMaxTuples = 8;

  WildcardMatch() : Module(), published_(bess::control::runtime().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  // Recompiles the extraction plan when metadata offsets are reassigned by a
  // pipeline-graph change. Returning 0 (rather than -ENOTSUP) keeps this
  // module registered for PreResume.
  int OnEvent(bess::Event event) override;

  CommandResponse Init(const bess::pb::WildcardMatchArg &arg);
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &arg);
  CommandResponse GetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetRuntimeConfig(const bess::pb::WildcardMatchConfig &arg);
  CommandResponse CommandAdd(const bess::pb::WildcardMatchCommandAddArg &arg);
  CommandResponse CommandDelete(
      const bess::pb::WildcardMatchCommandDeleteArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);
  CommandResponse CommandSetDefaultGate(
      const bess::pb::WildcardMatchCommandSetDefaultGateArg &arg);

 private:
  // One configured rule: per-field value and mask bytes (exactly as decoded
  // from the protobuf, field-sized) plus its rank and gate. Field bytes are
  // kept per field so introspection reproduces the legacy byte layout
  // verbatim; the dense packed key is derived at build time.
  struct Rule {
    std::vector<std::vector<uint8_t>> values;
    std::vector<std::vector<uint8_t>> masks;
    int64_t priority = 0;
    gate_idx_t gate = DROP_GATE;
  };

  // The configuration a batch runs against. Immutable once published; the
  // rules themselves are in `table`, which is updated in place.
  struct Generation {
    Generation(gate_idx_t d, bess::classifier::ExtractPlan e,
               std::shared_ptr<bess::classifier::ConcurrentMaskedTable> t,
               size_t k, bool valid, std::vector<size_t> offsets)
        : default_gate(d),
          extract(std::move(e)),
          table(std::move(t)),
          key_size(k),
          extraction_valid(valid),
          baked_source_offsets(std::move(offsets)) {}

    gate_idx_t default_gate = DROP_GATE;
    bess::classifier::ExtractPlan extract;
    // Shared, updated in place; owned by every generation that maps it.
    std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table;
    // Dense packed key width: sum of field sizes. Also the extraction stride.
    size_t key_size = 0;
    // False when metadata offsets were invalid at (re)build time. The packet
    // path then routes everything to the default gate instead of reading an
    // incorrect metadata region (fail-closed).
    bool extraction_valid = true;
    // Per-field source offset baked into `extract`, compared at PreResume to
    // detect graph-driven offset reassignment.
    std::vector<size_t> baked_source_offsets;
  };

  using GenerationPtr = std::unique_ptr<const Generation>;

  // Sentinel for a metadata field whose attribute currently has no valid
  // physical offset (e.g. orphan reader, out of space).
  static constexpr size_t kInvalidOffset = static_cast<size_t>(-1);

  // Field configuration, fixed at Init() time; every generation's plan gets
  // it, so a rebuild reproduces the module's matching exactly.
  struct FieldSpec {
    bool by_offset = true;
    int offset = 0;         // valid when by_offset
    std::string attr_name;  // otherwise (for GetInitialArg)
    int attr_id = -1;       // resolved once at Init() when !by_offset
    int size = 0;
  };

  // Per-field key layout resolved from the module's FieldSpecs and the
  // current metadata offsets.
  struct KeyLayout {
    size_t key_size = 0;
    std::vector<bess::classifier::RuntimeKeyField> key_fields;
    // Source offset baked into the plan per field, or kInvalidOffset for
    // unreadable metadata.
    std::vector<size_t> baked_offsets;
    // False when a metadata field has no valid physical offset.
    bool metadata_valid = true;
  };

  CommandResponse AddFieldOne(const bess::pb::Field &field, int idx);
  // Resolves FieldSpecs into a dense packed layout using current metadata
  // offsets. With `tolerate_invalid_metadata`, unreadable metadata marks the
  // layout invalid (fail-closed path) instead of failing.
  bool ComputeLayout(bool tolerate_invalid_metadata, KeyLayout *layout,
                     Error *err);
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
  // Inserts or replaces `rule` in `table`; the error on failure.
  Error InsertRule(bess::classifier::ConcurrentMaskedTable &table,
                   const Rule &rule) const;
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
  // Replaces the published generation with `build(current)`, or leaves the
  // active one alone when the builder returns nullptr (with *err set). Runs
  // only on the control plane, where command and graph mutations are
  // serialized; never on a packet worker.
  bool Publish(const std::function<GenerationPtr(const Generation &)> &build,
               Error *err);
  // Rebuilds and republishes the generation when metadata offsets changed
  // under it (graph reconfiguration + resume). Runs on the control thread with
  // workers paused. Never leaves a stale plan reading a reassigned metadata
  // region: refresh failure publishes a fail-closed generation.
  void RefreshForResume();

  // Field configuration, fixed at Init() time.
  std::vector<FieldSpec> field_specs_;

  // The live rule table (also held by the published generation).
  std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table_;

  // Publication and reclamation (bess::rcu::RcuPtr + the runtime's RcuDomain):
  // one acquire load per batch on the data path, serialized rebuilds off it,
  // and the retired generation is destroyed by a control thread rather than on
  // a packet worker.
  // Never null between Init() and module destruction: there is no DeInit()
  // (the generation is released with the module, workers already paused), and
  // every command publishes a replacement rather than clearing it.
  bess::rcu::RcuPtr<Generation> published_;
};

#endif  // BESS_MODULES_WILDCARDMATCH_H_
