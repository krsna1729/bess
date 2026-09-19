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

#include <rte_config.h>
#include <rte_hash_crc.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../utils/exact_match_table.h"

using google::protobuf::RepeatedPtrField;
using bess::utils::ExactMatchField;
using bess::utils::ExactMatchKey;
using bess::utils::ExactMatchRuleFields;
using bess::utils::ExactMatchTable;
using bess::utils::Error;

class ExactMatch final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;

  ExactMatch() : Module() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

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
    ExactMatchRuleFields fields;
    gate_idx_t gate;
  };

  // A whole matching generation: the rule list it was built from, the default
  // gate to use when nothing matches, and the table built from that list.
  // Immutable once published -- commands build a replacement and swap it in,
  // so a batch sees either the old generation or the new one, never a
  // half-applied change.
  struct Generation {
    std::vector<Rule> rules;
    gate_idx_t default_gate = DROP_GATE;
    ExactMatchTable<gate_idx_t> table;
  };

  using GenerationPtr = std::shared_ptr<const Generation>;

  CommandResponse AddFieldOne(const bess::pb::Field &field,
                              const bess::pb::FieldData &mask, int idx);
  // Turns a rule's protobuf fields into `rule`'s field list, validating the
  // count against the module's configured fields.
  Error RuleFieldsFromPb(const RepeatedPtrField<bess::pb::FieldData> &fields,
                         bess::utils::ExactMatchRuleFields *rule);
  // Turns a command argument into a `Rule`, validating gate and fields.
  Error RuleFromPb(const bess::pb::ExactMatchCommandAddArg &arg, Rule *rule);
  // Builds a generation for `rules`; nullptr with *err set on failure. Runs on
  // the control plane, off the data path.
  GenerationPtr Build(const std::vector<Rule> &rules, gate_idx_t default_gate,
                      Error *err);
  // Applies the module's configured fields (fixed at Init() time; a table
  // starts out empty) to `table`. Metadata attributes were resolved once at
  // Init(); this only configures the table, never registers anything.
  Error ApplyFields(ExactMatchTable<gate_idx_t> *table);
  // Inserts `rule` into `rules` or, if a rule with the same match values is
  // already there, overwrites its gate -- the same operation inserting an
  // existing key into the live table performed. Shared by the add command and
  // SetRuntimeConfig so both canonicalize identically.
  static void UpsertRule(std::vector<Rule> *rules, Rule rule);
  // Publishes `next` and then waits for the readers of `current` to drain, so
  // the retired generation is freed on this (control-plane) thread rather than
  // on a packet worker. `current` must be the caller's only reference to the
  // generation (the wait is until its use count drops to that one); caller
  // holds mutation_lock_ too.
  void Publish(GenerationPtr next, const GenerationPtr &current);

  // Field configuration, fixed at Init() time; every generation's table gets
  // it, so a rebuild reproduces the module's matching exactly.
  struct FieldSpec {
    bool by_offset;
    int offset;             // valid when by_offset
    std::string attr_name;  // otherwise (for GetInitialArg)
    int attr_id;            // resolved once at Init() when !by_offset
    int size;
    uint64_t mask;
  };
  std::vector<FieldSpec> field_specs_;
  bool empty_masks_;  // mainly for GetInitialArg

  // Holds one published generation; `mutation_lock_` serializes rebuilds.
  // Never null between Init() and module destruction: there is no DeInit()
  // (the generation is released with the module, workers already paused), and
  // every command publishes a replacement rather than clearing it.
  std::atomic<GenerationPtr> generation_;
  std::mutex mutation_lock_;  // serializes rule-table rebuilds
};

#endif  // BESS_MODULES_EXACTMATCH_H_
