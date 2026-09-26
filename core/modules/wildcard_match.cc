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

#include "wildcard_match.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../utils/endian.h"
#include "../utils/format.h"

using bess::metadata::Attribute;

namespace {

// Decodes one protobuf field value into `out`, sized to `field_size`.
//
// Binary input shorter than the field is zero-padded, which is what the legacy
// stack-local decode did (locals started zeroed); longer input is rejected
// instead of copied past the field word, which the legacy code did without
// checking.
bool DecodeFieldData(const bess::pb::FieldData &data, int field_size,
                     std::vector<uint8_t> *out, const char *what, size_t idx,
                     Error *err) {
  const size_t want = static_cast<size_t>(field_size);
  out->assign(want, 0);
  if (data.encoding_case() == bess::pb::FieldData::kValueBin) {
    const std::string &bin = data.value_bin();
    if (bin.size() > want) {
      *err = std::make_pair(
          EINVAL,
          bess::utils::Format("idx %zu: %s is %zu bytes, field is %zu", idx,
                              what, bin.size(), want));
      return false;
    }
    std::memcpy(out->data(), bin.data(), bin.size());
    return true;
  }
  uint64_t value = data.value_int();
  if (!bess::utils::uint64_to_bin(out->data(), value, field_size,
                                  /*big_endian=*/true)) {
    *err = std::make_pair(EINVAL,
                          bess::utils::Format("idx %zu: not a correct %d-byte "
                                              "%s",
                                              idx, field_size, what));
    return false;
  }
  return true;
}

}  // namespace

const Commands WildcardMatch::cmds = {
    {"get_initial_arg", "EmptyArg",
     MODULE_CMD_FUNC(&WildcardMatch::GetInitialArg), Command::THREAD_SAFE},
    {"get_runtime_config", "EmptyArg",
     MODULE_CMD_FUNC(&WildcardMatch::GetRuntimeConfig), Command::THREAD_SAFE},
    {"set_runtime_config", "WildcardMatchConfig",
     MODULE_CMD_FUNC(&WildcardMatch::SetRuntimeConfig), Command::THREAD_SAFE},
    {"add", "WildcardMatchCommandAddArg",
     MODULE_CMD_FUNC(&WildcardMatch::CommandAdd), Command::THREAD_SAFE},
    {"delete", "WildcardMatchCommandDeleteArg",
     MODULE_CMD_FUNC(&WildcardMatch::CommandDelete), Command::THREAD_SAFE},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&WildcardMatch::CommandClear),
     Command::THREAD_SAFE},
    {"set_default_gate", "WildcardMatchCommandSetDefaultGateArg",
     MODULE_CMD_FUNC(&WildcardMatch::CommandSetDefaultGate),
     Command::THREAD_SAFE}};

CommandResponse WildcardMatch::AddFieldOne(const bess::pb::Field &field,
                                           int idx) {
  FieldSpec spec;
  spec.size = field.num_bytes();

  if (spec.size < 1 || static_cast<size_t>(spec.size) > kMaxFieldSize) {
    return CommandFailure(EINVAL, "idx %d: 'size' must be 1-%zu", idx,
                          kMaxFieldSize);
  }

  if (field.position_case() == bess::pb::Field::kOffset) {
    spec.by_offset = true;
    spec.offset = field.offset();
    if (spec.offset < 0 || spec.offset > 1024) {
      return CommandFailure(EINVAL, "idx %d: too small 'offset'", idx);
    }
  } else if (field.position_case() == bess::pb::Field::kAttrName) {
    spec.by_offset = false;
    spec.attr_name = field.attr_name();
    // Resolve (register) the attribute here, once per module -- not per
    // generation: a rebuild that re-registered it would fail with EEXIST.
    spec.attr_id = AddMetadataAttr(spec.attr_name,
                                   static_cast<size_t>(spec.size),
                                   Attribute::AccessMode::kRead);
    if (spec.attr_id < 0) {
      return CommandFailure(-spec.attr_id, "idx %d: add_metadata_attr() failed",
                            idx);
    }
  } else {
    return CommandFailure(EINVAL, "idx %d: specify 'offset' or 'attr'", idx);
  }

  field_specs_.push_back(std::move(spec));
  return CommandSuccess();
}

/* Takes a list of all fields that may be used by rules.
 * Each field needs 'offset' (or 'name') and 'size' in bytes,
 *
 * e.g.: WildcardMatch([{'offset': 26, 'size': 4}, ...]
 * (checks the source IP address)
 *
 * You can also specify metadata attributes
 * e.g.: WildcardMatch([{'name': 'nexthop', 'size': 4}, ...] */

CommandResponse WildcardMatch::Init(const bess::pb::WildcardMatchArg &arg) {
  // A module with no fields has no key: the legacy implementation computed a
  // zero total key size and then indexed with (total_key_size_ - 1) / 8.
  if (arg.fields_size() == 0) {
    return CommandFailure(EINVAL, "must specify at least one field");
  }
  if (static_cast<size_t>(arg.fields_size()) > kMaxFields) {
    return CommandFailure(EINVAL, "too many fields (max %zu)", kMaxFields);
  }

  field_specs_.clear();
  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse err = AddFieldOne(arg.fields(i), i);
    if (err.error().code() != 0) {
      return err;
    }
  }

  auto table = NewTable();
  if (!table) {
    return CommandFailure(table.error().first, "%s",
                          table.error().second.c_str());
  }
  Error err;
  GenerationPtr gen = Build(*table, /*default_gate=*/DROP_GATE, &err);
  if (gen == nullptr) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }
  table_ = std::move(*table);
  published_.Initialize(std::move(gen));

  return CommandSuccess();
}

bool WildcardMatch::ComputeLayout(bool tolerate_invalid_metadata,
                                  KeyLayout *layout, Error *err) {
  if (field_specs_.empty()) {
    *err = std::make_pair(EINVAL, "must specify at least one field");
    return false;
  }
  if (field_specs_.size() > kMaxFields) {
    *err = std::make_pair(
        EINVAL, bess::utils::Format("too many fields (max %zu)", kMaxFields));
    return false;
  }

  layout->key_size = 0;
  layout->key_fields.clear();
  layout->baked_offsets.clear();
  layout->metadata_valid = true;
  layout->key_fields.reserve(field_specs_.size());
  layout->baked_offsets.reserve(field_specs_.size());

  size_t pos = 0;
  for (size_t i = 0; i < field_specs_.size(); i++) {
    const FieldSpec &spec = field_specs_[i];
    const int idx = static_cast<int>(i);
    if (spec.size < 1 || static_cast<size_t>(spec.size) > kMaxFieldSize) {
      *err = std::make_pair(
          EINVAL, bess::utils::Format("idx %d: 'size' must be in [1,%zu]", idx,
                                      kMaxFieldSize));
      return false;
    }

    bess::classifier::RuntimeKeyField field;
    field.key_offset = pos;
    field.size = static_cast<size_t>(spec.size);
    // Wildcard masks belong to the rules, not to extraction: no normalization.
    size_t baked;
    if (spec.by_offset) {
      if (spec.offset < 0 || spec.offset > 1024) {
        *err = std::make_pair(
            EINVAL, bess::utils::Format("idx %d: invalid 'offset'", idx));
        return false;
      }
      field.source = bess::classifier::SourceKind::kPacket;
      field.source_offset = static_cast<size_t>(spec.offset);
      baked = static_cast<size_t>(spec.offset);
    } else {
      const bess::metadata::mt_offset_t offset =
          attr_offset(static_cast<size_t>(spec.attr_id));
      if (!bess::metadata::IsValidOffset(offset)) {
        layout->metadata_valid = false;
        if (!tolerate_invalid_metadata) {
          *err = std::make_pair(
              EINVAL,
              bess::utils::Format(
                  "idx %d: metadata attribute '%s' has no valid offset "
                  "(pipeline graph changed?)",
                  idx, spec.attr_name.c_str()));
          return false;
        }
        baked = kInvalidOffset;
        // Placeholder; the plan is never executed while invalid.
        field.source_offset = 0;
      } else {
        baked = static_cast<size_t>(offset);
        field.source_offset = baked;
      }
      field.source = bess::classifier::SourceKind::kMetadata;
    }

    layout->key_fields.push_back(std::move(field));
    layout->baked_offsets.push_back(baked);
    pos += static_cast<size_t>(spec.size);
  }
  layout->key_size = pos;
  return true;
}

Error WildcardMatch::PackRule(const Rule &rule, std::vector<std::byte> *value,
                              std::vector<std::byte> *mask) const {
  if (rule.values.size() != field_specs_.size() ||
      rule.masks.size() != field_specs_.size()) {
    return std::make_pair(
        EINVAL, bess::utils::Format("rule should have %zu fields (has %zu)",
                                    field_specs_.size(), rule.values.size()));
  }
  value->clear();
  mask->clear();
  for (size_t i = 0; i < rule.values.size(); i++) {
    const size_t want = static_cast<size_t>(field_specs_[i].size);
    if (rule.values[i].size() != want || rule.masks[i].size() != want) {
      return std::make_pair(
          EINVAL,
          bess::utils::Format("rule field %zu should have size %zu", i, want));
    }
    const auto *v = reinterpret_cast<const std::byte *>(rule.values[i].data());
    const auto *m = reinterpret_cast<const std::byte *>(rule.masks[i].data());
    value->insert(value->end(), v, v + want);
    mask->insert(mask->end(), m, m + want);
  }
  return std::make_pair(0, std::string());
}

std::expected<std::shared_ptr<bess::classifier::ConcurrentMaskedTable>, Error>
WildcardMatch::NewTable() const {
  size_t key_size = 0;
  for (const FieldSpec &spec : field_specs_) {
    key_size += static_cast<size_t>(spec.size);
  }
  auto table = bess::classifier::ConcurrentMaskedTable::Create(
      static_cast<uint32_t>(key_size), kMaxTuples,
      bess::control::runtime().rcu());
  if (!table) {
    return std::unexpected(std::make_pair(EINVAL, table.error()));
  }
  return std::shared_ptr<bess::classifier::ConcurrentMaskedTable>(
      std::move(*table));
}

Error WildcardMatch::InsertRule(bess::classifier::ConcurrentMaskedTable &table,
                                const Rule &rule) const {
  std::vector<std::byte> value, mask;
  Error err = PackRule(rule, &value, &mask);
  if (err.first) {
    return err;
  }
  using R = bess::classifier::ConcurrentMaskedTable::UpsertResult;
  switch (table.Upsert(
      bess::classifier::ConstBytes(mask.data(), mask.size()),
      bess::classifier::ConstBytes(value.data(), value.size()), rule.priority,
      rule.gate)) {
    case R::kInserted:
    case R::kUpdated:
      return std::make_pair(0, std::string());
    case R::kTooManyTuples:
      // The wire-compatible ceiling counts *active* masks: a mask whose last
      // rule was deleted, or a clear, frees its tuple.
      return std::make_pair(
          EINVAL, bess::utils::Format("too many distinct masks (%zu, max %zu)",
                                      table.tuple_count() + 1, kMaxTuples));
    case R::kNotCanonical:
      return std::make_pair(EINVAL, "invalid pair of value and mask");
    case R::kFull:
      break;
  }
  return std::make_pair(ENOSPC, "rule table is full");
}

WildcardMatch::GenerationPtr WildcardMatch::Build(
    std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table,
    gate_idx_t default_gate, Error *err) {
  KeyLayout layout;
  if (!ComputeLayout(/*tolerate_invalid_metadata=*/false, &layout, err)) {
    return nullptr;
  }

  bess::classifier::RuntimeClassifierSchema schema;
  schema.key_size = layout.key_size;
  schema.bounds = bess::classifier::BoundsPolicy::kCheck;
  schema.key_fields = layout.key_fields;
  auto plan = bess::classifier::ExtractPlan::Compile(schema);
  if (!plan) {
    *err = std::make_pair(EINVAL, "extraction plan: " + plan.error().message);
    return nullptr;
  }
  CHECK_EQ(layout.key_size, table->key_len());

  return std::make_unique<Generation>(
      default_gate, std::move(*plan), std::move(table), layout.key_size,
      /*extraction_valid=*/true, std::move(layout.baked_offsets));
}

WildcardMatch::GenerationPtr WildcardMatch::BuildDegraded(
    std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table,
    gate_idx_t default_gate) {
  // Same layout machinery, tolerating unreadable metadata: the resulting plan
  // is never executed (extraction_valid == false), so placeholder offsets are
  // safe.
  KeyLayout layout;
  Error err;
  if (!ComputeLayout(/*tolerate_invalid_metadata=*/true, &layout, &err)) {
    CHECK(false) << "degraded WildcardMatch build failed: " << err.second;
  }
  CHECK_GT(layout.key_size, 0u);

  bess::classifier::RuntimeClassifierSchema schema;
  schema.key_size = layout.key_size;
  schema.bounds = bess::classifier::BoundsPolicy::kCheck;
  schema.key_fields = layout.key_fields;
  auto plan = bess::classifier::ExtractPlan::Compile(schema);
  CHECK(plan.has_value()) << "degraded WildcardMatch plan failed to compile";

  return std::make_unique<Generation>(
      default_gate, std::move(*plan), std::move(table), layout.key_size,
      /*extraction_valid=*/false, std::move(layout.baked_offsets));
}

void WildcardMatch::RefreshForResume() {
  const Generation *current = published_.Read();
  if (current == nullptr) {
    return;  // not initialized (or already deinitialized)
  }

  bool changed = false;
  for (size_t i = 0; i < field_specs_.size(); i++) {
    const FieldSpec &spec = field_specs_[i];
    size_t now;
    if (spec.by_offset) {
      now = static_cast<size_t>(spec.offset);
    } else {
      const bess::metadata::mt_offset_t offset =
          attr_offset(static_cast<size_t>(spec.attr_id));
      now = bess::metadata::IsValidOffset(offset)
                ? static_cast<size_t>(offset)
                : kInvalidOffset;
    }
    if (i >= current->baked_source_offsets.size() ||
        current->baked_source_offsets[i] != now) {
      changed = true;
      break;
    }
  }
  if (!changed) {
    return;
  }

  Error err;
  GenerationPtr next = Build(current->table, current->default_gate, &err);
  if (next != nullptr) {
    published_.Publish(std::move(next));
    bess::control::runtime().rcu().ReclaimReady();
    return;
  }

  // The dispatcher ignores ordinary OnEvent errors and resume would proceed
  // with a stale plan reading a reassigned metadata region. Fail closed
  // instead: keep serving rules/default-gate introspection, but route every
  // packet to the default gate.
  LOG(ERROR) << "WildcardMatch '" << name() << "': metadata refresh failed ("
             << err.second << "); routing all packets to the default gate";
  published_.Publish(BuildDegraded(current->table, current->default_gate));
  bess::control::runtime().rcu().ReclaimReady();
}

int WildcardMatch::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  RefreshForResume();
  // Return 0 (not -ENOTSUP) to stay registered for future resumes.
  return 0;
}

bool WildcardMatch::Publish(
    const std::function<GenerationPtr(const Generation &)> &build,
    Error *err) {
  const Generation *current = published_.Read();
  if (current == nullptr) {
    *err = Error(EINVAL, "not initialized");
    return false;  // not initialized (or already deinitialized)
  }

  GenerationPtr next = build(*current);
  if (next == nullptr) {
    return false;  // the builder owns reporting why
  }

  // Publish, retire the replaced generation against a fresh grace period, and
  // reclaim whatever readers are already done with. This runs on the control
  // thread, so a retired table is destroyed here -- never on a worker.
  published_.Publish(std::move(next));
  bess::control::runtime().rcu().ReclaimReady();
  return true;
}

Error WildcardMatch::RuleFieldsFromPb(
    const RepeatedPtrField<bess::pb::FieldData> &fields, size_t field_size,
    std::vector<std::vector<uint8_t>> *out) {
  out->clear();
  out->reserve(field_size);
  for (size_t i = 0; i < field_size; i++) {
    out->emplace_back();
    Error err;
    if (!DecodeFieldData(fields.Get(static_cast<int>(i)),
                         field_specs_[i].size, &out->back(), "field", i, &err)) {
      return err;
    }
  }
  return std::make_pair(0, std::string());
}

Error WildcardMatch::RuleFromPb(
    const bess::pb::WildcardMatchCommandAddArg &arg, Rule *rule) {
  // Validate the 64-bit wire value before narrowing: gate_idx_t is 16-bit, so
  // a post-cast check would accept e.g. 65536 as gate 0.
  if (!bess::IsValidGateValue(arg.gate())) {
    return std::make_pair(
        EINVAL, bess::utils::Format("Invalid gate: %llu",
                                    static_cast<unsigned long long>(arg.gate())));
  }
  const gate_idx_t gate = static_cast<gate_idx_t>(arg.gate());
  if (static_cast<size_t>(arg.values_size()) != field_specs_.size()) {
    return std::make_pair(
        EINVAL, bess::utils::Format("must specify %zu values",
                                    field_specs_.size()));
  }
  if (static_cast<size_t>(arg.masks_size()) != field_specs_.size()) {
    return std::make_pair(
        EINVAL,
        bess::utils::Format("must specify %zu masks", field_specs_.size()));
  }

  Error ret = RuleFieldsFromPb(arg.values(), field_specs_.size(),
                               &rule->values);
  if (ret.first) {
    return ret;
  }
  ret = RuleFieldsFromPb(arg.masks(), field_specs_.size(), &rule->masks);
  if (ret.first) {
    return ret;
  }

  // The canonical-rule invariant, checked per field exactly as the legacy
  // per-field check did (and again on the dense key by the substrate).
  for (size_t i = 0; i < field_specs_.size(); i++) {
    for (size_t b = 0; b < rule->values[i].size(); b++) {
      const uint8_t value = rule->values[i][b];
      const uint8_t mask = rule->masks[i][b];
      if ((value & static_cast<uint8_t>(~mask)) != 0) {
        return std::make_pair(
            EINVAL,
            bess::utils::Format("idx %zu: invalid pair of value and mask", i));
      }
    }
  }

  rule->priority = arg.priority();
  rule->gate = gate;
  return std::make_pair(0, std::string());
}

// add/delete/clear change the live table in place (G1.2 mode C): one
// table operation each, with workers reading throughout. No generation is
// built.
CommandResponse WildcardMatch::CommandAdd(
    const bess::pb::WildcardMatchCommandAddArg &arg) {
  Rule rule;
  Error ret = RuleFromPb(arg, &rule);
  if (!ret.first) {
    ret = InsertRule(*table_, rule);
  }
  if (ret.first) {
    return CommandFailure(ret.first, "%s", ret.second.c_str());
  }
  return CommandSuccess();
}

CommandResponse WildcardMatch::CommandDelete(
    const bess::pb::WildcardMatchCommandDeleteArg &arg) {
  Rule rule;
  if (static_cast<size_t>(arg.values_size()) != field_specs_.size() ||
      static_cast<size_t>(arg.masks_size()) != field_specs_.size()) {
    return CommandFailure(EINVAL, "must specify %zu values and masks",
                          field_specs_.size());
  }
  Error ret =
      RuleFieldsFromPb(arg.values(), field_specs_.size(), &rule.values);
  if (!ret.first) {
    ret = RuleFieldsFromPb(arg.masks(), field_specs_.size(), &rule.masks);
  }
  std::vector<std::byte> value, mask;
  if (!ret.first) {
    ret = PackRule(rule, &value, &mask);
  }
  if (ret.first) {
    return CommandFailure(ret.first, "%s", ret.second.c_str());
  }
  if (!table_->Erase(bess::classifier::ConstBytes(mask.data(), mask.size()),
                     bess::classifier::ConstBytes(value.data(),
                                                  value.size()))) {
    return CommandFailure(ENOENT, "failed to delete a rule");
  }
  return CommandSuccess();
}

CommandResponse WildcardMatch::CommandClear(const bess::pb::EmptyArg &) {
  // Rules go, the default gate stays. Tuple masks go with them, so a clear
  // genuinely restores tuple capacity.
  table_->Clear();
  return CommandSuccess();
}

CommandResponse WildcardMatch::CommandSetDefaultGate(
    const bess::pb::WildcardMatchCommandSetDefaultGateArg &arg) {
  if (!bess::IsValidGateValue(arg.gate())) {
    return CommandFailure(EINVAL, "Invalid gate: %llu",
                          static_cast<unsigned long long>(arg.gate()));
  }
  const gate_idx_t gate = static_cast<gate_idx_t>(arg.gate());
  Error err;
  const bool published = Publish([&](const Generation &current) {
    return Build(current.table, gate, &err);
  }, &err);
  if (!published) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }

  return CommandSuccess();
}

// Retrieves a WildcardMatchArg that would reconstruct this module.
CommandResponse WildcardMatch::GetInitialArg(const bess::pb::EmptyArg &) {
  bess::pb::WildcardMatchArg resp;
  for (const FieldSpec &spec : field_specs_) {
    bess::pb::Field *f = resp.add_fields();
    if (spec.by_offset) {
      f->set_offset(spec.offset);
    } else {
      f->set_attr_name(spec.attr_name);
    }
    f->set_num_bytes(spec.size);
  }
  return CommandSuccess(resp);
}

// Retrieves a WildcardMatchConfig that would restore this module's runtime
// configuration.
CommandResponse WildcardMatch::GetRuntimeConfig(const bess::pb::EmptyArg &) {
  bess::pb::WildcardMatchConfig resp;
  using rule_t = bess::pb::WildcardMatchCommandAddArg;

  const Generation *gen = published_.Read();
  resp.set_default_gate(gen->default_gate);

  // The table is the source of truth: each rule's mask and (canonical, so
  // already masked) value, split back into fields.
  table_->ForEach([&](bess::classifier::ConstBytes mask,
                      bess::classifier::ConstBytes value,
                      const bess::classifier::ConcurrentMaskedTable::Rule &r) {
    rule_t *out = resp.add_rules();
    out->set_priority(r.priority);
    out->set_gate(r.result);
    size_t pos = 0;
    for (const FieldSpec &spec : field_specs_) {
      const size_t n = static_cast<size_t>(spec.size);
      out->add_values()->set_value_bin(
          reinterpret_cast<const char *>(value.data() + pos), n);
      out->add_masks()->set_value_bin(
          reinterpret_cast<const char *>(mask.data() + pos), n);
      pos += n;
    }
  });

  // Sort the results so that they're always predictable: by priority, then
  // gate, then masks, then values -- the legacy order.
  std::sort(resp.mutable_rules()->begin(), resp.mutable_rules()->end(),
            [](const rule_t &a, const rule_t &b) {
              if (a.priority() != b.priority()) {
                return a.priority() < b.priority();
              }
              if (a.gate() != b.gate()) {
                return a.gate() < b.gate();
              }
              for (int i = 0; i < a.masks_size(); i++) {
                if (a.masks(i).value_bin() != b.masks(i).value_bin()) {
                  return a.masks(i).value_bin() < b.masks(i).value_bin();
                }
              }
              for (int i = 0; i < a.values_size(); i++) {
                if (a.values(i).value_bin() != b.values(i).value_bin()) {
                  return a.values(i).value_bin() < b.values(i).value_bin();
                }
              }
              return false;
            });
  return CommandSuccess(resp);
}

// Uses a WildcardMatchConfig to restore this module's runtime config.
// The new configuration is built in full and swapped in, so an error leaves the
// currently installed one serving unchanged -- no partially restored state,
// which is what the old in-place version had to warn about.
CommandResponse WildcardMatch::SetRuntimeConfig(
    const bess::pb::WildcardMatchConfig &arg) {
  if (!bess::IsValidGateValue(arg.default_gate())) {
    return CommandFailure(
        EINVAL, "Invalid default gate: %llu",
        static_cast<unsigned long long>(arg.default_gate()));
  }
  const gate_idx_t default_gate = static_cast<gate_idx_t>(arg.default_gate());
  // A whole-configuration restore: one of the deliberate bulk builds (G1.2
  // mode G). Fill a fresh table, then publish it in one step; any error
  // discards it and leaves the running configuration alone.
  auto table = NewTable();
  if (!table) {
    return CommandFailure(table.error().first, "%s",
                          table.error().second.c_str());
  }
  for (int i = 0; i < arg.rules_size(); i++) {
    Rule rule;
    Error ret = RuleFromPb(arg.rules(i), &rule);
    // Duplicates in the argument collapse to the last occurrence, as `add`
    // does.
    if (!ret.first) {
      ret = InsertRule(**table, rule);
    }
    if (ret.first) {
      return CommandFailure(ret.first, "%s", ret.second.c_str());
    }
  }

  Error err;
  auto next_table = *table;
  const bool published = Publish([&](const Generation &) {
    return Build(next_table, default_gate, &err);
  }, &err);
  if (!published) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }
  table_ = std::move(next_table);
  return CommandSuccess();
}

void WildcardMatch::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  // One snapshot for the whole batch: a concurrent command can neither swap
  // the generation mid-batch nor free it under this lookup.
  const Generation *gen = published_.Read();
  const gate_idx_t default_gate = gen->default_gate;
  const int cnt = batch->cnt();

  if (!gen->extraction_valid) {
    // Fail-closed generation (metadata offsets unreadable): route everything
    // to the default gate without touching packet or metadata bytes.
    for (int i = 0; i < cnt; i++) {
      EmitPacket(ctx, batch->packet(i), default_gate);
    }
    return;
  }

  // Stack scratch only: no packet-path allocation.
  const size_t key_size = gen->key_size;
  std::array<bess::classifier::SourceView, bess::PacketBatch::kMaxBurst>
      sources;
  std::array<std::byte, bess::PacketBatch::kMaxBurst * kMaxKeyBytes> keys;
  std::array<uint16_t, bess::PacketBatch::kMaxBurst> gates;

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    // Packet span is the first segment only: fields reaching past data_len (or
    // into later segments) fail extraction under kCheck and take the default
    // gate instead of over-reading, as the legacy 8-byte loads could.
    sources[i].packet = bess::classifier::ConstBytes(
        pkt.head_data<const std::byte *>(),
        static_cast<size_t>(pkt.data_len()));
    sources[i].metadata = bess::classifier::ConstBytes(
        pkt.metadata<const std::byte *>(), SNBUF_METADATA);
  }

  const uint64_t valid = gen->extract.ExecuteBatch(
      std::span<const bess::classifier::SourceView>(sources).first(
          static_cast<size_t>(cnt)),
      bess::classifier::MutableBytes(keys).first(static_cast<size_t>(cnt) *
                                                 key_size),
      key_size);
  // The masked backend examines every row, so failed rows must hold
  // well-defined bytes; their results are discarded through `valid` below
  // regardless.
  const uint64_t all_valid = (uint64_t{1} << static_cast<size_t>(cnt)) - 1;
  if ((valid & all_valid) != all_valid) {
    for (int i = 0; i < cnt; i++) {
      if (!(valid & (uint64_t{1} << i))) {
        std::memset(keys.data() + static_cast<size_t>(i) * key_size, 0,
                    key_size);
      }
    }
  }

  uint64_t hits = gen->table->LookupBatch(
      bess::classifier::ConstBytes(keys.data(),
                                   static_cast<size_t>(cnt) * key_size),
      key_size, gates.data(), static_cast<size_t>(cnt));
  hits &= valid;

  for (int i = 0; i < cnt; i++) {
    const gate_idx_t gate =
        (hits & (uint64_t{1} << i)) ? gates[i] : default_gate;
    EmitPacket(ctx, batch->packet(i), gate);
  }
}

std::string WildcardMatch::GetDesc() const {
  return bess::utils::Format("%zu fields, %zu rules", field_specs_.size(),
                             table_->size());
}

ADD_MODULE(WildcardMatch, "wm",
           "Multi-field classifier with a wildcard match table")
