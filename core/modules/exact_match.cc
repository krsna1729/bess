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
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../event.h"
#include "../metadata.h"
#include "../snbuf_layout.h"
#include "../utils/bits.h"
#include "../utils/endian.h"
#include "../rcu/rcu_ptr.h"
#include "../utils/format.h"

namespace classifier = bess::classifier;

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

namespace {

// Converts a raw configured mask into the byte mask applied on extraction,
// mirroring ExactMatchTable::DoAddField (utils/exact_match_table.h):
//   - raw == 0 selects the default all-ones mask;
//   - otherwise the value must fit in `size` bytes, stored big-endian for
//     packet (offset) fields and little-endian for metadata fields;
//   - an all-zero result is rejected as an empty mask.
// `is_packet` selects the byte order (legacy `force_be`).
// Returns the all-ones flag through `is_default` so the caller can use an
// empty (unmasked) normalization, which is semantically identical.
bool ConvertMask(uint64_t raw, int size, bool is_packet,
                 std::vector<std::byte> *out, bool *is_default, Error *err,
                 int idx) {
  uint64_t converted;
  if (raw == 0) {
    // By default all bits are considered.
    converted = bess::utils::SetBitsHigh<uint64_t>(
        static_cast<size_t>(size) * 8);
    *is_default = true;
  } else {
    *is_default = false;
    converted = 0;
    const bool big_endian = bess::utils::is_be_system() || is_packet;
    if (!bess::utils::uint64_to_bin(&converted, raw, static_cast<size_t>(size),
                                    big_endian)) {
      *err = std::make_pair(
          EINVAL, bess::utils::Format("idx %d: not a valid %d-byte mask", idx,
                                      size));
      return false;
    }
    if (converted == 0) {
      *err = std::make_pair(EINVAL,
                            bess::utils::Format("idx %d: empty mask", idx));
      return false;
    }
  }
  out->resize(static_cast<size_t>(size));
  std::memcpy(out->data(), &converted, static_cast<size_t>(size));
  return true;
}

}  // namespace

CommandResponse ExactMatch::AddFieldOne(const bess::pb::Field &field,
                                        const bess::pb::FieldData &mask,
                                        int idx) {
  int size = field.num_bytes();
  uint64_t mask64 = 0;
  size_t mask_bin_len = 0;
  if (mask.encoding_case() == bess::pb::FieldData::kValueInt) {
    mask64 = mask.value_int();
  } else if (mask.encoding_case() == bess::pb::FieldData::kValueBin) {
    const std::string &bin = mask.value_bin();
    mask_bin_len = bin.size();
    if (mask_bin_len > sizeof(mask64)) {
      // The legacy parser copied the full bytestring over the stack-local
      // mask word (buffer over-read/over-write); reject it instead.
      return CommandFailure(EINVAL, "idx %d: not a valid %d-byte mask", idx,
                            size);
    }
    bess::utils::Copy(reinterpret_cast<uint8_t *>(&mask64), bin.data(),
                      bin.size());
  }

  FieldSpec spec;
  spec.size = size;
  spec.mask = mask64;
  spec.mask_bin_len = mask_bin_len;
  spec.attr_id = -1;
  if (field.position_case() == bess::pb::Field::kAttrName) {
    spec.by_offset = false;
    spec.attr_name = field.attr_name();
    spec.offset = 0;
    // Resolve (register) the attribute here, once per module -- not per
    // generation: a rebuild that re-registered it would fail with EEXIST.
    spec.attr_id = AddMetadataAttr(
        spec.attr_name, static_cast<size_t>(size),
        bess::metadata::Attribute::AccessMode::kRead);
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

// Resolves the module's FieldSpecs into a dense packed key layout using the
// currently assigned metadata offsets. Packet offsets are config-fixed;
// metadata offsets are reassigned by ComputeMetadataOffsets on graph changes.
// With `tolerate_invalid_metadata`, unreadable metadata marks the layout
// invalid (for the fail-closed path) instead of failing.
bool ExactMatch::ComputeLayout(bool tolerate_invalid_metadata,
                               KeyLayout *layout, Error *err) {
  if (field_specs_.size() > kMaxFields) {
    *err = std::make_pair(
        EINVAL, bess::utils::Format("too many fields (max %zu)", kMaxFields));
    return false;
  }

  layout->key_size = 0;
  layout->key_fields.clear();
  layout->converted_masks.clear();
  layout->baked_offsets.clear();
  layout->metadata_valid = true;
  layout->key_fields.reserve(field_specs_.size());
  layout->converted_masks.reserve(field_specs_.size());
  layout->baked_offsets.reserve(field_specs_.size());

  size_t pos = 0;
  for (size_t i = 0; i < field_specs_.size(); i++) {
    const FieldSpec &spec = field_specs_[i];
    const int idx = static_cast<int>(i);
    if (spec.size < 1 ||
        static_cast<size_t>(spec.size) > kMaxFieldSize) {
      *err = std::make_pair(
          EINVAL, bess::utils::Format("idx %d: 'size' must be in [1,%zu]", idx,
                                      kMaxFieldSize));
      return false;
    }
    if (spec.mask_bin_len > sizeof(uint64_t)) {
      *err = std::make_pair(
          EINVAL, bess::utils::Format("idx %d: not a valid %d-byte mask", idx,
                                      spec.size));
      return false;
    }

    classifier::RuntimeKeyField field;
    field.key_offset = pos;
    field.size = static_cast<size_t>(spec.size);
    size_t baked;
    if (spec.by_offset) {
      if (spec.offset < 0 || spec.offset > 1024) {
        *err = std::make_pair(
            EINVAL, bess::utils::Format("idx %d: invalid 'offset'", idx));
        return false;
      }
      field.source = classifier::SourceKind::kPacket;
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
      field.source = classifier::SourceKind::kMetadata;
    }

    std::vector<std::byte> converted;
    bool is_default = false;
    if (!ConvertMask(spec.mask, spec.size, spec.by_offset, &converted,
                     &is_default, err, idx)) {
      return false;
    }
    if (!is_default) {
      field.normalization.mask = converted;
    }
    layout->key_fields.push_back(std::move(field));
    layout->converted_masks.push_back(std::move(converted));
    layout->baked_offsets.push_back(baked);
    pos += static_cast<size_t>(spec.size);
  }
  layout->key_size = pos;
  return true;
}

Error ExactMatch::PackKey(const std::vector<std::vector<uint8_t>> &fields,
                         std::vector<std::byte> *key) const {
  if (fields.size() != field_specs_.size()) {
    return std::make_pair(
        EINVAL, bess::utils::Format("rule should have %zu fields (has %zu)",
                                    field_specs_.size(), fields.size()));
  }
  // Rule bytes are packed densely in field order WITHOUT applying the mask --
  // the legacy table copied rule bytes verbatim while masking only extracted
  // packet/metadata bytes. Preserve that distinction.
  key->clear();
  for (size_t i = 0; i < fields.size(); i++) {
    const size_t want = static_cast<size_t>(field_specs_[i].size);
    if (fields[i].size() != want) {
      return std::make_pair(
          EINVAL,
          bess::utils::Format("rule field %zu should have size %zu (has %zu)",
                              i, want, fields[i].size()));
    }
    const auto *bytes = reinterpret_cast<const std::byte *>(fields[i].data());
    key->insert(key->end(), bytes, bytes + want);
  }
  return std::make_pair(0, std::string());
}

std::expected<std::shared_ptr<classifier::ConcurrentExactTable>, Error>
ExactMatch::NewTable(size_t rules) const {
  size_t key_size = 0;
  for (const FieldSpec &spec : field_specs_) {
    key_size += static_cast<size_t>(spec.size);
  }
  // Decision D-010: room for `rules` plus the grace-period headroom.
  auto table = classifier::ConcurrentExactTable::Create(
      static_cast<uint32_t>(key_size),
      classifier::ConcurrentExactTable::CapacityFor(rules),
      bess::control::runtime().rcu());
  if (!table) {
    return std::unexpected(std::make_pair(ENOMEM, table.error()));
  }
  return std::shared_ptr<classifier::ConcurrentExactTable>(std::move(*table));
}

std::expected<std::shared_ptr<classifier::ConcurrentExactTable>, Error>
ExactMatch::FillTable(
    size_t rules,
    const std::function<bool(classifier::ConcurrentExactTable &)> &fill)
    const {
  // A cuckoo insert can fail before a table is full (an unlucky key set); a
  // rule is never dropped for it -- retry at twice the size.
  for (int attempt = 0; attempt < 4; attempt++) {
    auto table = NewTable(rules);
    if (!table) {
      return table;
    }
    if (fill(**table)) {
      return table;
    }
    rules = size_t{(*table)->capacity()};
  }
  return std::unexpected(
      std::make_pair(ENOSPC, std::string("rule table could not be built")));
}

bool ExactMatch::EnsureCapacity(bool force, Error *err) {
  if (!force && table_->HasRoomForOne()) {
    return true;
  }
  // Growth: copy into a table twice the size and publish it. Rare -- doubling
  // makes it amortized O(1) per insert -- and never on the packet path.
  // Growth is due when live keys plus deletes still in a grace period eat
  // into the headroom (D-010). `force`: an add failed anyway -- a burst of
  // deletes outran the headroom, or displacement ran out of room.
  auto bigger = FillTable(size_t{table_->capacity()},
                          [&](classifier::ConcurrentExactTable &next) {
                            bool ok = true;
                            table_->ForEach([&](classifier::ConstBytes key,
                                                uint64_t value) {
                              ok = ok && next.Upsert(key, value) !=
                                             classifier::ConcurrentExactTable::
                                                 UpsertResult::kFull;
                            });
                            return ok;
                          });
  if (!bigger) {
    *err = bigger.error();
    return false;
  }
  auto next_table = *bigger;
  const bool published = Publish([&](const Generation &current) {
    return Build(next_table, current.default_gate, err);
  }, err);
  if (published) {
    table_ = std::move(next_table);
  }
  return published;
}

ExactMatch::GenerationPtr ExactMatch::Build(
    std::shared_ptr<classifier::ConcurrentExactTable> table,
    gate_idx_t default_gate, Error *err) {
  KeyLayout layout;
  if (!ComputeLayout(/*tolerate_invalid_metadata=*/false, &layout, err)) {
    return nullptr;
  }

  classifier::RuntimeClassifierSchema schema;
  schema.key_size = layout.key_size;
  schema.bounds = classifier::BoundsPolicy::kCheck;
  schema.key_fields = layout.key_fields;
  auto plan = classifier::ExtractPlan::Compile(schema);
  if (!plan) {
    const auto &e = plan.error();
    *err = std::make_pair(EINVAL, "extraction plan: " + e.message);
    return nullptr;
  }
  CHECK_EQ(layout.key_size, table->key_len());

  return std::make_unique<Generation>(
      default_gate, std::move(*plan), std::move(table), layout.key_size,
      /*extraction_valid=*/true, std::move(layout.converted_masks),
      std::move(layout.baked_offsets));
}

ExactMatch::GenerationPtr ExactMatch::BuildDegraded(
    std::shared_ptr<classifier::ConcurrentExactTable> table,
    gate_idx_t default_gate) {
  // Same layout machinery, tolerating unreadable metadata: the resulting plan
  // is never executed (extraction_valid == false), so placeholder offsets are
  // safe. Only reachable for established generations, hence key_size > 0.
  KeyLayout layout;
  Error err;
  if (!ComputeLayout(/*tolerate_invalid_metadata=*/true, &layout, &err)) {
    // Field configuration itself is broken; callers only reach here after a
    // successful Build, so this is unreachable.
    CHECK(false) << "degraded ExactMatch build failed: " << err.second;
  }
  CHECK_GT(layout.key_size, 0u);

  classifier::RuntimeClassifierSchema schema;
  schema.key_size = layout.key_size;
  schema.bounds = classifier::BoundsPolicy::kCheck;
  schema.key_fields = layout.key_fields;
  auto plan = classifier::ExtractPlan::Compile(schema);
  CHECK(plan.has_value()) << "degraded ExactMatch plan failed to compile";

  return std::make_unique<Generation>(
      default_gate, std::move(*plan), std::move(table), layout.key_size,
      /*extraction_valid=*/false, std::move(layout.converted_masks),
      std::move(layout.baked_offsets));
}

void ExactMatch::RefreshForResume() {
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
  LOG(ERROR) << "ExactMatch '" << name()
             << "': metadata refresh failed (" << err.second
             << "); routing all packets to the default gate";
  published_.Publish(BuildDegraded(current->table, current->default_gate));
  bess::control::runtime().rcu().ReclaimReady();
}

int ExactMatch::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  RefreshForResume();
  // Return 0 (not -ENOTSUP) to stay registered for future resumes.
  return 0;
}

bool ExactMatch::Publish(
    const std::function<GenerationPtr(const Generation &)> &build, Error *err) {
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

  auto table = NewTable(0);
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

// Retrieves an ExactMatchArg that would reconstruct this module.
CommandResponse ExactMatch::GetInitialArg(const bess::pb::EmptyArg &) {
  bess::pb::ExactMatchArg r;

  const Generation *gen = published_.Read();
  for (size_t i = 0; i < field_specs_.size(); i++) {
    const FieldSpec &spec = field_specs_[i];
    bess::pb::Field *ret_field = r.add_fields();
    if (!spec.by_offset) {
      ret_field->set_attr_name(spec.attr_name);
    } else {
      ret_field->set_offset(spec.offset);
    }
    ret_field->set_num_bytes(spec.size);
    if (!empty_masks_) {
      bess::pb::FieldData *ret_mask = r.add_masks();
      // Masks are serialized in the converted (table-applied) byte order,
      // matching the legacy ExactMatchField::mask dump.
      const char *ptr =
          reinterpret_cast<const char *>(gen->converted_masks[i].data());
      ret_mask->set_value_bin(ptr, static_cast<size_t>(spec.size));
    }
  }
  return CommandSuccess(r);
}

// Retrieves an ExactMatchConfig that would restore this module's
// runtime configuration.
CommandResponse ExactMatch::GetRuntimeConfig(const bess::pb::EmptyArg &) {
  bess::pb::ExactMatchConfig r;
  using rule_t = bess::pb::ExactMatchCommandAddArg;

  const Generation *gen = published_.Read();
  r.set_default_gate(gen->default_gate);
  // The table is the source of truth: each key is the rule's fields packed
  // in field order.
  table_->ForEach([&](classifier::ConstBytes key, uint64_t value) {
    rule_t *out = r.add_rules();
    out->set_gate(static_cast<gate_idx_t>(value));
    size_t pos = 0;
    for (const FieldSpec &spec : field_specs_) {
      bess::pb::FieldData *field = out->add_fields();
      // See GetInitialArg above for why we only set_value_bin here.
      field->set_value_bin(reinterpret_cast<const char *>(key.data() + pos),
                           static_cast<size_t>(spec.size));
      pos += static_cast<size_t>(spec.size);
    }
  });
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
  // Validate the 64-bit wire value before narrowing: gate_idx_t is 16-bit, so
  // a post-cast check would accept e.g. 65536 as gate 0.
  if (!bess::IsValidGateValue(arg.gate())) {
    return std::make_pair(
        EINVAL,
        bess::utils::Format("Invalid gate: %llu",
                            static_cast<unsigned long long>(arg.gate())));
  }
  const gate_idx_t gate = static_cast<gate_idx_t>(arg.gate());

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
  if (!bess::IsValidGateValue(arg.default_gate())) {
    return CommandFailure(
        EINVAL, "Invalid default gate: %llu",
        static_cast<unsigned long long>(arg.default_gate()));
  }
  const gate_idx_t default_gate = static_cast<gate_idx_t>(arg.default_gate());

  // A whole-configuration restore: one of the few deliberate rebuilds (G1.2
  // mode G, bulk load). Build a fresh table, then publish it in one step.
  std::vector<std::pair<std::vector<std::byte>, gate_idx_t>> rules;
  rules.reserve(static_cast<size_t>(arg.rules_size()));
  for (auto i = 0; i < arg.rules_size(); i++) {
    Rule rule;
    std::vector<std::byte> key;
    Error ret = RuleFromPb(arg.rules(i), &rule);
    if (!ret.first) {
      ret = PackKey(rule.fields, &key);
    }
    if (ret.first) {
      return CommandFailure(ret.first, "%s", ret.second.c_str());
    }
    rules.emplace_back(std::move(key), rule.gate);
  }
  // Duplicates collapse to one rule with the last gate, as inserting them
  // into the table always did.
  auto table = FillTable(rules.size(), [&](classifier::ConcurrentExactTable &t) {
    for (const auto &[key, gate] : rules) {
      if (t.Upsert(classifier::ConstBytes(key.data(), key.size()), gate) ==
          classifier::ConcurrentExactTable::UpsertResult::kFull) {
        return false;
      }
    }
    return true;
  });
  if (!table) {
    return CommandFailure(table.error().first, "%s",
                          table.error().second.c_str());
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

void ExactMatch::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
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

  // Stack scratch only: no packet-path allocation. ExactMatch's key layout
  // is dense, so a fully-covering plan writes every byte of every valid row
  // and the used region needs no pre-zeroing; a failed extraction writes
  // nothing, and invalid rows are zeroed below before the backend (which
  // looks up all rows) sees them. Gapped generic schemas keep the
  // caller-initializes-gap contract and pre-zero instead.
  const size_t key_size = gen->key_size;
  std::array<classifier::SourceView, bess::PacketBatch::kMaxBurst> sources;
  std::array<std::byte, bess::PacketBatch::kMaxBurst * kMaxKeyBytes> keys;
  if (!gen->extract.fully_covers_key()) {
    std::memset(keys.data(), 0, static_cast<size_t>(cnt) * key_size);
  }
  std::array<uint64_t, bess::PacketBatch::kMaxBurst> gates;

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    // Packet span is the first segment only: fields reaching past data_len
    // (or into later segments) fail extraction under kCheck and take the
    // default gate instead of over-reading, as the legacy 8-byte loads could.
    sources[i].packet = classifier::ConstBytes(
        pkt.head_data<const std::byte *>(),
        static_cast<size_t>(pkt.data_len()));
    sources[i].metadata = classifier::ConstBytes(
        pkt.metadata<const std::byte *>(), SNBUF_METADATA);
  }

  // One extraction batch dispatch, one backend batch dispatch. Metadata
  // attribute IDs were resolved to physical offsets at (re)build time, so
  // nothing here resolves names or parses configuration.
  const uint64_t valid = gen->extract.ExecuteBatch(
      std::span<const classifier::SourceView>(sources).first(
          static_cast<size_t>(cnt)),
      classifier::MutableBytes(keys).first(static_cast<size_t>(cnt) *
                                           key_size),
      key_size);
  // Invalid rows hold stale scratch (or partial gap bytes); zero them so the
  // backend sees well-defined keys. Their results are discarded through the
  // validity mask below regardless.
  const uint64_t all_valid =
      (uint64_t{1} << static_cast<size_t>(cnt)) - 1;
  if ((valid & all_valid) != all_valid) {
    for (int i = 0; i < cnt; i++) {
      if (!(valid & (uint64_t{1} << i))) {
        std::memset(keys.data() + static_cast<size_t>(i) * key_size, 0,
                    key_size);
      }
    }
  }
  uint64_t hits = gen->table->LookupBatch(
      classifier::ConstBytes(keys.data(),
                             static_cast<size_t>(cnt) * key_size),
      key_size, gates.data(), static_cast<size_t>(cnt));
  hits &= valid;

  for (int i = 0; i < cnt; i++) {
    const gate_idx_t gate = (hits & (uint64_t{1} << i))
                                ? static_cast<gate_idx_t>(gates[i])
                                : default_gate;
    EmitPacket(ctx, batch->packet(i), gate);
  }
}

std::string ExactMatch::GetDesc() const {
  return bess::utils::Format("%zu fields, %zu rules", field_specs_.size(),
                             table_->size());
}

Error ExactMatch::RuleFieldsFromPb(
    const RepeatedPtrField<bess::pb::FieldData> &fields,
    std::vector<std::vector<uint8_t>> *rule) {
  if (fields.size() != static_cast<int>(field_specs_.size())) {
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

// add/delete/clear write the shared table in place (G1.2 mode C): one
// rte_hash operation each, with workers reading throughout. No generation is
// built -- except the rare, amortized growth in EnsureCapacity.
CommandResponse ExactMatch::CommandAdd(
    const bess::pb::ExactMatchCommandAddArg &arg) {
  Rule rule;
  Error ret = RuleFromPb(arg, &rule);
  std::vector<std::byte> key;
  if (!ret.first) {
    ret = PackKey(rule.fields, &key);
  }
  if (ret.first) {
    return CommandFailure(ret.first, "%s", ret.second.c_str());
  }
  if (!EnsureCapacity(/*force=*/false, &ret)) {
    return CommandFailure(ret.first, "%s", ret.second.c_str());
  }
  const classifier::ConstBytes k(key.data(), key.size());
  using classifier::ConcurrentExactTable;
  if (table_->Upsert(k, rule.gate) == ConcurrentExactTable::UpsertResult::kFull) {
    // Full inside the headroom: a burst of deletes still in their grace
    // period outran it (or displacement ran out of room). Grow rather than
    // refuse.
    if (!EnsureCapacity(/*force=*/true, &ret)) {
      return CommandFailure(ret.first, "%s", ret.second.c_str());
    }
    if (table_->Upsert(k, rule.gate) ==
        ConcurrentExactTable::UpsertResult::kFull) {
      return CommandFailure(ENOSPC, "rule table is full");
    }
  }
  return CommandSuccess();
}

CommandResponse ExactMatch::CommandDelete(
    const bess::pb::ExactMatchCommandDeleteArg &arg) {
  if (arg.fields_size() == 0) {
    return CommandFailure(EINVAL, "argument must be a list");
  }

  std::vector<std::vector<uint8_t>> fields;
  Error ret = RuleFieldsFromPb(arg.fields(), &fields);
  std::vector<std::byte> key;
  if (!ret.first) {
    ret = PackKey(fields, &key);
  }
  if (ret.first) {
    return CommandFailure(ret.first, "%s", ret.second.c_str());
  }
  if (!table_->Erase(classifier::ConstBytes(key.data(), key.size()))) {
    return CommandFailure(ENOENT, "rule doesn't exist");
  }
  return CommandSuccess();
}

CommandResponse ExactMatch::CommandClear(const bess::pb::EmptyArg &) {
  // Rules go, the default gate stays -- what ClearRules() did to the live
  // table. Deleted in place, rule by rule; workers never stop reading.
  std::vector<std::vector<std::byte>> keys;
  keys.reserve(table_->size());
  table_->ForEach([&](classifier::ConstBytes key, uint64_t) {
    keys.emplace_back(key.begin(), key.end());
  });
  for (const auto &key : keys) {
    table_->Erase(classifier::ConstBytes(key.data(), key.size()));
  }
  return CommandSuccess();
}

CommandResponse ExactMatch::CommandSetDefaultGate(
    const bess::pb::ExactMatchCommandSetDefaultGateArg &arg) {
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

ADD_MODULE(ExactMatch, "em", "Multi-field classifier with an exact match table")
