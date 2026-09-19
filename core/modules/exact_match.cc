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

#include "exact_match.h"

#include <algorithm>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../utils/endian.h"
#include "../utils/published_generation.h"
#include "../utils/format.h"

// XXX: this is repeated in many modules. get rid of them when converting .h to
// .hh, etc... it's in defined in some old header
static inline int is_valid_gate(gate_idx_t gate) {
  return (gate < MAX_GATES || gate == DROP_GATE);
}

const Commands ExactMatch::cmds = {
    {"get_initial_arg", "EmptyArg", MODULE_CMD_FUNC(&ExactMatch::GetInitialArg),
     Command::THREAD_SAFE},
    {"get_runtime_config", "EmptyArg",
     MODULE_CMD_FUNC(&ExactMatch::GetRuntimeConfig), Command::THREAD_SAFE},
    {"set_runtime_config", "ExactMatchConfig",
     MODULE_CMD_FUNC(&ExactMatch::SetRuntimeConfig), Command::THREAD_SAFE},
    {"add", "ExactMatchCommandAddArg", MODULE_CMD_FUNC(&ExactMatch::CommandAdd),
     Command::THREAD_SAFE},
    {"delete", "ExactMatchCommandDeleteArg",
     MODULE_CMD_FUNC(&ExactMatch::CommandDelete), Command::THREAD_SAFE},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&ExactMatch::CommandClear),
     Command::THREAD_SAFE},
    {"set_default_gate", "ExactMatchCommandSetDefaultGateArg",
     MODULE_CMD_FUNC(&ExactMatch::CommandSetDefaultGate),
     Command::THREAD_SAFE}};

CommandResponse ExactMatch::AddFieldOne(const bess::pb::Field &field,
                                        const bess::pb::FieldData &mask,
                                        int idx) {
  int size = field.num_bytes();
  uint64_t mask64 = 0;
  if (mask.encoding_case() == bess::pb::FieldData::kValueInt) {
    mask64 = mask.value_int();
  } else if (mask.encoding_case() == bess::pb::FieldData::kValueBin) {
    bess::utils::Copy(reinterpret_cast<uint8_t *>(&mask64),
                      mask.value_bin().c_str(), mask.value_bin().size());
  }

  FieldSpec spec;
  spec.size = size;
  spec.mask = mask64;
  spec.attr_id = -1;
  if (field.position_case() == bess::pb::Field::kAttrName) {
    spec.by_offset = false;
    spec.attr_name = field.attr_name();
    spec.offset = 0;
    // Resolve (register) the attribute here, once per module -- not per
    // generation: a rebuild that re-registered it would fail with EEXIST.
    spec.attr_id = AddMetadataAttr(
        spec.attr_name, size, bess::metadata::Attribute::AccessMode::kRead);
    if (spec.attr_id < 0) {
      return CommandFailure(-spec.attr_id,
                            "idx %d: add_metadata_attr() failed", idx);
    }
  } else if (field.position_case() == bess::pb::Field::kOffset) {
    spec.by_offset = true;
    spec.offset = field.offset();
  } else {
    return CommandFailure(EINVAL,
                          "idx %d: must specify 'offset' or 'attr_name'", idx);
  }

  field_specs_.push_back(std::move(spec));
  return CommandSuccess();
}

// Applies the module's configured fields to `table`. Called for every
// generation, so a rebuild reproduces the module's matching exactly. The
// second and later calls re-validate the same configuration -- cheap, and the
// error paths stay in one place.
Error ExactMatch::ApplyFields(ExactMatchTable<gate_idx_t> *table) {
  for (size_t i = 0; i < field_specs_.size(); i++) {
    const FieldSpec &spec = field_specs_[i];
    Error ret;
    if (spec.by_offset) {
      ret = table->AddField(spec.offset, spec.size, spec.mask, i);
    } else {
      ret = table->AddResolvedAttrField(spec.attr_id, spec.size, spec.mask, i);
    }
    if (ret.first) {
      return ret;
    }
  }
  return std::make_pair(0, std::string());
}

ExactMatch::GenerationPtr ExactMatch::Build(const std::vector<Rule> &rules,
                                            gate_idx_t default_gate,
                                            Error *err) {
  auto gen = std::make_shared<Generation>();
  gen->default_gate = default_gate;

  Error ret = ApplyFields(&gen->table);
  if (ret.first) {
    *err = ret;
    return nullptr;
  }

  for (const Rule &rule : rules) {
    // Validation failures and CuckooMap insertion failures (ENOSPC) both fail
    // the build: a rule that is not in the table must not enter the rule list
    // either.
    Error add_ret = gen->table.AddRule(rule.gate, rule.fields);
    if (add_ret.first) {
      *err = add_ret;
      return nullptr;
    }
  }

  // Canonicalized by the callers, so the source of truth and the table must
  // agree rule for rule. This is the regression invariant for that.
  CHECK_EQ(gen->table.Size(), rules.size());

  gen->rules = rules;
  return gen;
}

void ExactMatch::UpsertRule(std::vector<Rule> *rules, Rule rule) {
  for (Rule &r : *rules) {
    // Same match values: overwrite the gate, the way inserting the same key
    // into the live table did.
    if (r.fields == rule.fields) {
      r.gate = rule.gate;
      return;
    }
  }
  rules->push_back(std::move(rule));
}

CommandResponse ExactMatch::Init(const bess::pb::ExactMatchArg &arg) {
  empty_masks_ = arg.masks_size() == 0;
  if (arg.fields_size() != arg.masks_size() && !empty_masks_) {
    return CommandFailure(EINVAL,
                          "must provide masks for all fields (or no masks for "
                          "default match on all bits on all fields)");
  }

  // AddFieldOne only records the configuration; Build() below is what
  // validates it against the module (attr names) and applies it.
  field_specs_.clear();
  for (auto i = 0; i < arg.fields_size(); ++i) {
    CommandResponse err;

    if (empty_masks_) {
      bess::pb::FieldData emptymask;
      err = AddFieldOne(arg.fields(i), emptymask, i);
    } else {
      err = AddFieldOne(arg.fields(i), arg.masks(i), i);
    }

    if (err.error().code() != 0) {
      return err;
    }
  }

  Error err;
  GenerationPtr gen = Build(/*rules=*/{}, /*default_gate=*/DROP_GATE, &err);
  if (gen == nullptr) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }
  published_.Store(std::move(gen));

  return CommandSuccess();
}

// Retrieves an ExactMatchArg that would reconstruct this module.
CommandResponse ExactMatch::GetInitialArg(const bess::pb::EmptyArg &) {
  bess::pb::ExactMatchArg r;

  const GenerationPtr gen = published_.Snapshot();
  for (size_t i = 0; i < gen->table.num_fields(); i++) {
    const ExactMatchField &f = gen->table.get_field(i);
    bess::pb::Field *ret_field = r.add_fields();
    if (f.attr_id >= 0) {
      ret_field->set_attr_name(all_attrs().at(f.attr_id).name);
    } else {
      ret_field->set_offset(f.offset);
    }
    ret_field->set_num_bytes(f.size);
    if (!empty_masks_) {
      bess::pb::FieldData *ret_mask = r.add_masks();
      // The optimal type for the mask (value_bin vs value_int) depends
      // on the wire encoding.  For the moment, we'll just use value_bin
      // with the field size, though; it's much simpler.  Or, perhaps
      // we should save the form used during configuration, and use
      // the same form here.
      const char *ptr = reinterpret_cast<const char *>(&f.mask);
      ret_mask->set_value_bin(ptr, f.size);
    }
  }
  return CommandSuccess(r);
}

// Retrieves an ExactMatchConfig that would restore this module's
// runtime configuration.
CommandResponse ExactMatch::GetRuntimeConfig(const bess::pb::EmptyArg &) {
  bess::pb::ExactMatchConfig r;
  using rule_t = bess::pb::ExactMatchCommandAddArg;

  const GenerationPtr gen = published_.Snapshot();
  r.set_default_gate(gen->default_gate);
  for (const Rule &rule : gen->rules) {
    rule_t *out = r.add_rules();
    out->set_gate(rule.gate);
    for (size_t i = 0; i < rule.fields.size(); i++) {
      bess::pb::FieldData *field = out->add_fields();

      // See GetInitialArg above for why we only set_value_bin here.
      const char *ptr = reinterpret_cast<const char *>(rule.fields[i].data());
      field->set_value_bin(ptr, rule.fields[i].size());
    }
  }
  std::sort(r.mutable_rules()->begin(), r.mutable_rules()->end(),
            [](const rule_t &a, const rule_t &b) {
              // Primary sort key is gate number.
              if (a.gate() != b.gate()) {
                return a.gate() < b.gate();
              }
              // After that, sort by value-to-be-matched, in field order.
              for (int i = 0; i < a.fields_size(); i++) {
                if (a.fields(i).value_bin() != b.fields(i).value_bin()) {
                  return a.fields(i).value_bin() < b.fields(i).value_bin();
                }
              }
              // Huh: a, b exactly equal - should not happen unless
              // std::sort() is poor.
              return false;
            });
  return CommandSuccess(r);
}

Error ExactMatch::RuleFromPb(const bess::pb::ExactMatchCommandAddArg &arg,
                             Rule *rule) {
  gate_idx_t gate = arg.gate();

  if (!is_valid_gate(gate)) {
    return std::make_pair(EINVAL,
                          bess::utils::Format("Invalid gate: %hu", gate));
  }

  if (arg.fields_size() == 0) {
    return std::make_pair(EINVAL, "'fields' must be a list");
  }

  Error ret = RuleFieldsFromPb(arg.fields(), &rule->fields);
  if (ret.first) {
    return ret;
  }
  rule->gate = gate;

  return std::make_pair(0, std::string());
}

// Uses an ExactMatchConfig to restore this module's runtime config.
// The new configuration is built in full and swapped in, so an error leaves
// the currently installed one serving unchanged -- no partially restored
// state, which is what the old in-place version had to warn about.
CommandResponse ExactMatch::SetRuntimeConfig(
    const bess::pb::ExactMatchConfig &arg) {
  std::vector<Rule> rules;
  rules.reserve(arg.rules_size());
  for (auto i = 0; i < arg.rules_size(); i++) {
    Rule rule;
    Error ret = RuleFromPb(arg.rules(i), &rule);
    if (ret.first) {
      return CommandFailure(ret.first, "%s", ret.second.c_str());
    }
    // Duplicates in the argument collapse to one rule with the last gate,
    // which is what inserting them into the table did -- and keeps the rule
    // list from disagreeing with the table.
    UpsertRule(&rules, std::move(rule));
  }

  Error err;
  const bool published = published_.Update([&](const Generation &) {
    return Build(rules, arg.default_gate(), &err);
  });
  if (!published) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }

  return CommandSuccess();
}

void ExactMatch::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  ExactMatchKey keys[bess::PacketBatch::kMaxBurst] __ymm_aligned;

  // One snapshot for the whole batch: a concurrent command can neither swap
  // the table mid-batch nor free it under this lookup.
  const GenerationPtr gen = published_.Snapshot();
  const auto &table = gen->table;
  const gate_idx_t default_gate = gen->default_gate;

  const auto buffer_fn = [&](bess::Packet *pkt, const ExactMatchField &f) {
    int attr_id = f.attr_id;
    if (attr_id >= 0) {
      return ptr_attr<uint8_t>(this, attr_id, pkt);
    }
    return pkt->head_data<uint8_t *>() + f.offset;
  };
  table.MakeKeys(batch, buffer_fn, keys);

  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::Packet *pkt = batch->pkts()[i];
    EmitPacket(ctx, pkt, table.Find(keys[i], default_gate));
  }
}

std::string ExactMatch::GetDesc() const {
  const GenerationPtr gen = published_.Snapshot();
  return bess::utils::Format("%zu fields, %zu rules", gen->table.num_fields(),
                             gen->table.Size());
}

Error ExactMatch::RuleFieldsFromPb(
    const RepeatedPtrField<bess::pb::FieldData> &fields,
    bess::utils::ExactMatchRuleFields *rule) {
  if (static_cast<size_t>(fields.size()) != field_specs_.size()) {
    return std::make_pair(
        EINVAL, bess::utils::Format("rule should have %zu fields (has %d)",
                                    field_specs_.size(), fields.size()));
  }

  for (auto i = 0; i < fields.size(); i++) {
    int field_size = field_specs_[i].size;

    bess::pb::FieldData current = fields.Get(i);

    if (current.encoding_case() == bess::pb::FieldData::kValueBin) {
      const std::string &f_obj = fields.Get(i).value_bin();
      rule->push_back(std::vector<uint8_t>(f_obj.begin(), f_obj.end()));
    } else {
      rule->emplace_back();
      uint64_t rule64 = current.value_int();
      for (int j = 0; j < field_size; j++) {
        rule->back().push_back(rule64 & 0xFFULL);
        rule64 >>= 8;
      }
    }
  }

  return std::make_pair(0, std::string());
}

CommandResponse ExactMatch::CommandAdd(
    const bess::pb::ExactMatchCommandAddArg &arg) {
  Rule rule;
  Error ret = RuleFromPb(arg, &rule);
  if (ret.first) {
    return CommandFailure(ret.first, "%s", ret.second.c_str());
  }

  Error err;
  const bool published = published_.Update([&](const Generation &current) {
    std::vector<Rule> rules = current.rules;
    UpsertRule(&rules, rule);
    return Build(rules, current.default_gate, &err);
  });
  if (!published) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }

  return CommandSuccess();
}

CommandResponse ExactMatch::CommandDelete(
    const bess::pb::ExactMatchCommandDeleteArg &arg) {
  if (arg.fields_size() == 0) {
    return CommandFailure(EINVAL, "argument must be a list");
  }

  ExactMatchRuleFields fields;
  Error ret = RuleFieldsFromPb(arg.fields(), &fields);
  if (ret.first) {
    return CommandFailure(ret.first, "%s", ret.second.c_str());
  }

  Error err;
  bool found = false;
  const bool published =
      published_.Update([&](const Generation &current) -> GenerationPtr {
        std::vector<Rule> rules;
        rules.reserve(current.rules.size());
        for (const Rule &r : current.rules) {
          if (r.fields == fields) {
            found = true;
            continue;
          }
          rules.push_back(r);
        }
        if (!found) {
          return nullptr;
        }
        return Build(rules, current.default_gate, &err);
      });
  if (!published) {
    if (!found) {
      return CommandFailure(ENOENT, "rule doesn't exist");
    }
    return CommandFailure(err.first, "%s", err.second.c_str());
  }

  return CommandSuccess();
}

CommandResponse ExactMatch::CommandClear(const bess::pb::EmptyArg &) {
  // Rules go, the default gate stays -- what ClearRules() did to the live
  // table.
  Error err;
  const bool published = published_.Update([&](const Generation &current) {
    return Build(/*rules=*/{}, current.default_gate, &err);
  });
  if (!published) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }

  return CommandSuccess();
}

CommandResponse ExactMatch::CommandSetDefaultGate(
    const bess::pb::ExactMatchCommandSetDefaultGateArg &arg) {
  Error err;
  const bool published = published_.Update([&](const Generation &current) {
    return Build(current.rules, arg.gate(), &err);
  });
  if (!published) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }

  return CommandSuccess();
}

ADD_MODULE(ExactMatch, "em", "Multi-field classifier with an exact match table")
