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
#include "../classifier/extract_plan.h"
#include "../classifier/runtime_schema.h"
#include "../control/runtime_state.h"
#include "../event.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../rcu/rcu_ptr.h"

using google::protobuf::RepeatedPtrField;
// (bess::utils::Error is the same alias; defined locally so this module no
// longer depends on utils/exact_match_table.h, which hash_lb still uses.)
using Error = std::pair<int, std::string>;

class ExactMatch final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  // Module-level limits, preserved from the legacy ExactMatchTable-based
  // implementation for protobuf API compatibility. The classifier library
  // itself supports arbitrary widths; these bounds are ExactMatch's own.
  static constexpr size_t kMaxFields = 8;
  static constexpr size_t kMaxFieldSize = 8;
  static constexpr size_t kMaxKeyBytes = kMaxFields * kMaxFieldSize;

  ExactMatch()
      : Module(), published_(bess::control::runtime().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  // Recompiles the extraction plan when metadata offsets are reassigned by a
  // pipeline-graph change. Returning 0 (rather than -ENOTSUP) keeps this
  // module registered for PreResume.
  int OnEvent(bess::Event event) override;

  CommandResponse Init(const bess::pb::ExactMatchArg &arg);
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
    gate_idx_t gate;
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

  CommandResponse AddFieldOne(const bess::pb::Field &field,
                              const bess::pb::FieldData &mask, int idx);
  // Turns a rule's protobuf fields into `rule`'s field list, validating the
  // count against the module's configured fields.
  Error RuleFieldsFromPb(const RepeatedPtrField<bess::pb::FieldData> &fields,
                         std::vector<std::vector<uint8_t>> *rule);
  // Turns a command argument into a `Rule`, validating gate and fields.
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
  // A new, empty table sized for `rules` entries with growth headroom.
  std::expected<std::shared_ptr<bess::classifier::ConcurrentExactTable>, Error>
  NewTable(size_t rules) const;
  // Grows the table (a larger copy, published as a new generation) when one
  // more insert would pass the load limit. Amortized O(1) per insert.
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

  // Publication and reclamation (bess::rcu::RcuPtr + the runtime's RcuDomain):
  // one acquire load per batch on the data path, serialized rebuilds off it,
  // and the retired generation is destroyed by a control thread rather than on
  // a packet worker.
  // Never null between Init() and module destruction: there is no DeInit()
  // (the generation is released with the module, workers already paused), and
  // every command publishes a replacement rather than clearing it.
  bess::rcu::RcuPtr<Generation> published_;

  // The table the command path writes (always the current generation's).
  std::shared_ptr<bess::classifier::ConcurrentExactTable> table_;
};

#endif  // BESS_MODULES_EXACTMATCH_H_
