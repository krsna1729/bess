// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "wildcard_match.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "packet_reshape.h"
#include "snbuf_layout.h"
#include "utils/endian.h"
#include "utils/format.h"

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

// D-140: the class has no commands and no actions. The rules are the
// "<name>/rules" resource, the default gate the "<name>/settings" resource,
// and the creation argument GetPipeline's Module.arg.
const Commands WildcardMatch::cmds = {};

const ResourceTables WildcardMatch::resource_tables = {
    ResourceTable::Map<bess::pb::WildcardMatchRuleKey,
                       bess::pb::WildcardMatchRuleValue>("rules"),
    ResourceTable::Singleton<bess::pb::WildcardMatchSettings>("settings")};

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
  if (arg.max_tuples() > kMaxTuplesLimit) {
    return CommandFailure(EINVAL, "max_tuples must be 1-%zu (0: %zu)",
                          kMaxTuplesLimit, kDefaultMaxTuples);
  }
  max_tuples_ = arg.max_tuples() == 0 ? kDefaultMaxTuples : arg.max_tuples();

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
  if (arg.has_default_gate() && !bess::IsValidGateValue(arg.default_gate())) {
    return CommandFailure(EINVAL, "Invalid default gate: %llu",
                          static_cast<unsigned long long>(arg.default_gate()));
  }
  const gate_idx_t default_gate =
      arg.has_default_gate() ? static_cast<gate_idx_t>(arg.default_gate()) : DROP_GATE;
  if (arg.has_default_gate()) {
    initial_default_gate_ = arg.default_gate();
  }
  result_specs_.clear();
  result_reference_names_.clear();
  result_value_size_ = 0;
  for (const bess::pb::ResultField &field : arg.result_fields()) {
    if (field.num_bytes() == 0) {
      return CommandFailure(EINVAL, "a result field has zero bytes");
    }
    if (field.value_offset() >
        std::numeric_limits<uint32_t>::max() - field.num_bytes()) {
      return CommandFailure(EINVAL, "a result field overflows its record");
    }
    ResultSpec spec;
    spec.value_offset = field.value_offset();
    spec.size = field.num_bytes();
    spec.reference = field.reference();
    spec.reference_be32 = field.reference_key() ==
                          bess::pb::ResultField::BE32;
    if (spec.reference_be32 && (spec.size != 4 || spec.reference.empty())) {
      return CommandFailure(
          EINVAL, "a BE32 reference is one 4-byte field naming a resource");
    }
    if (!spec.reference.empty() &&
        std::find(result_reference_names_.begin(),
                  result_reference_names_.end(),
                  spec.reference) == result_reference_names_.end()) {
      result_reference_names_.push_back(spec.reference);
    }
    if (field.destination_case() == bess::pb::ResultField::kAttrName) {
      if (field.attr_name().empty()) {
        return CommandFailure(EINVAL, "a result field has an empty attribute");
      }
      spec.by_attr = true;
      spec.attr_name = field.attr_name();
      spec.attr_id = AddMetadataAttr(
          spec.attr_name, spec.size,
          bess::metadata::Attribute::AccessMode::kWrite);
      if (spec.attr_id < 0) {
        return CommandFailure(-spec.attr_id, "%s: add_metadata_attr failed",
                              spec.attr_name.c_str());
      }
    } else if (field.destination_case() == bess::pb::ResultField::kOffset) {
      spec.by_attr = false;
      spec.offset = field.offset();
    } else {
      return CommandFailure(
          EINVAL, "a result field needs an attr_name or an offset");
    }
    result_value_size_ =
        std::max(result_value_size_,
                 static_cast<size_t>(spec.value_offset) + spec.size);
    result_specs_.push_back(std::move(spec));
  }
  if (result_value_size_ > bess::classifier::RecordArena::kMaxRecordSize) {
    return CommandFailure(
        EINVAL, "result record is %zu bytes, the maximum is %zu",
        result_value_size_, bess::classifier::RecordArena::kMaxRecordSize);
  }
  records_ = result_specs_.empty()
                 ? nullptr
                 : std::make_shared<bess::classifier::RecordArena>(
                       std::max<size_t>(1, result_value_size_),
                       init_context().rcu());
  if (arg.has_default_result() && records_ == nullptr &&
      !arg.default_result().empty()) {
    return CommandFailure(EINVAL,
                          "default_result needs at least one result field");
  }
  if (arg.has_default_result() && records_ != nullptr) {
    if (arg.default_result().size() > result_value_size_) {
      return CommandFailure(
          EINVAL, "default_result is wider than the schema's record ("
                      "%zu > %zu)",
          arg.default_result().size(), result_value_size_);
    }
    std::vector<std::byte> record(result_value_size_, std::byte{0});
    if (!arg.default_result().empty()) {
      std::memcpy(record.data(), arg.default_result().data(),
                  arg.default_result().size());
    }
    default_result_ = std::move(record);
    default_record_id_ = records_->Allocate();
    if (default_record_id_ == 0) {
      return CommandFailure(ENOSPC, "the result arena is full");
    }
    bess::classifier::MutableBytes slot = records_->Record(default_record_id_);
    std::memcpy(slot.data(), default_result_->data(),
                default_result_->size());
  }
  GenerationPtr gen = Build(*table, default_gate, &err);
  if (gen == nullptr) {
    return CommandFailure(err.first, "%s", err.second.c_str());
  }
  table_ = std::move(*table);
  published_.Initialize(std::move(gen));

  // The rules as a transactional resource (D-024), so that one transaction
  // can change them together with other modules' tables. Keys are the
  // packed mask then the packed value (fields in order); values are
  // {priority, gate}. The table filters rules being prepared itself.
  using bess::classifier::MaskedRuleResource;
  resource_ = std::make_unique<MaskedRuleResource>(
      name() + "/rules",
      MaskedRuleResource::Hooks{
          .table = [this] { return table_; },
          .check_value = [this](const MaskedRuleResource::Value &v)
              -> MaskedRuleResource::Result {
            if (!bess::IsValidGateValue(v.gate)) {
              return std::unexpected("invalid gate " +
                                     std::to_string(v.gate));
            }
            if (v.result.size() > result_value_size_) {
              return std::unexpected(
                  "result is " + std::to_string(v.result.size()) +
                  " bytes, the schema's record is " +
                  std::to_string(result_value_size_));
            }
            return {};
          },
          .references = [this](const MaskedRuleResource::Value &v) {
            return ResultReferences(v.result);
          },
          .records = records_,
          .may_reference = result_reference_names_});
  // Typed keys and values over the RPC (D-025): values and masks per field,
  // checked and packed as the class does for a rule, and
  // {priority, gate, result_id}.
  binding_ = init_context().codecs().Bind(
      *resource_,std::make_shared<bess::framework::TypedCodec<
                          bess::pb::WildcardMatchRuleKey,
                          bess::pb::WildcardMatchRuleValue>>(
      [this](const bess::pb::WildcardMatchRuleKey &key)
          -> std::expected<bess::dataplane::ResourceKey, std::string> {
        bess::pb::WildcardMatchCommandAddArg arg;
        *arg.mutable_values() = key.values();
        *arg.mutable_masks() = key.masks();
        Rule rule;
        Error err = RuleFromPb(arg, &rule);
        std::vector<std::byte> value, mask;
        if (!err.first) {
          err = PackRule(rule, &value, &mask);
        }
        if (err.first) {
          return std::unexpected(err.second);
        }
        return MaskedRuleResource::Key(
            bess::classifier::ConstBytes(mask.data(), mask.size()),
            bess::classifier::ConstBytes(value.data(), value.size()));
      },
      [this](const bess::pb::WildcardMatchRuleValue &value)
          -> std::expected<std::any, std::string> {
        if (!bess::IsValidGateValue(value.gate())) {
          return std::unexpected("invalid gate " +
                                 std::to_string(value.gate()));
        }
        if (value.result().size() > result_value_size_) {
          return std::unexpected(
              "result is " + std::to_string(value.result().size()) +
              " bytes, the schema's record is " +
              std::to_string(result_value_size_));
        }
        return std::any(MaskedRuleResource::Value{
            value.priority(), static_cast<uint16_t>(value.gate()),
            std::vector<std::byte>(
                reinterpret_cast<const std::byte *>(value.result().data()),
                reinterpret_cast<const std::byte *>(value.result().data()) +
                    value.result().size())});
      },
      // Every rule as a client sends it: per-field value and mask bytes,
      // full field width (the table holds them canonical), as the resource
      // enumerates them.
      [this](const bess::framework::TypedCodec<
             bess::pb::WildcardMatchRuleKey,
             bess::pb::WildcardMatchRuleValue>::TypedVisitor &visit) {
        bess::pb::WildcardMatchRuleKey key;
        bess::pb::WildcardMatchRuleValue value;
        table_->ForEach(
            [&](bess::classifier::ConstBytes mask,
                bess::classifier::ConstBytes bytes,
                const bess::classifier::ConcurrentMaskedTable::Rule &r) {
              key.Clear();
              value.Clear();
              size_t pos = 0;
              for (const FieldSpec &spec : field_specs_) {
                const size_t n = static_cast<size_t>(spec.size);
                key.add_values()->set_value_bin(
                    reinterpret_cast<const char *>(bytes.data() + pos), n);
                key.add_masks()->set_value_bin(
                    reinterpret_cast<const char *>(mask.data() + pos), n);
                pos += n;
              }
              value.set_priority(r.priority);
              value.set_gate(r.gate);
              if (records_ != nullptr) {
                const bess::classifier::ConstBytes record =
                    records_->Lookup(r.result_id);
                value.set_result(reinterpret_cast<const char *>(record.data()),
                                 record.size());
              }
              visit(key, value);
            });
      }));
  if (auto registered =
          init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  // The default gate as "<module>/settings" (D-129): stage validates the gate
  // and builds the replacement generation,
  // apply swaps it in and retires the one it replaced (one retire). Workers
  // may run throughout: they read the generation once per batch.
  if (auto registered = settings_.Register(
          init_context().resources(), init_context().codecs(),
          name() + "/settings",
          [this](const bess::pb::WildcardMatchSettings &v)
              -> std::expected<GenerationPtr, std::string> {
            if (!bess::IsValidGateValue(v.default_gate())) {
              return std::unexpected(bess::utils::Format(
                  "Invalid gate: %llu",
                  static_cast<unsigned long long>(v.default_gate())));
            }
            Error err;
            GenerationPtr next =
                Build(published_.Read()->table,
                      static_cast<gate_idx_t>(v.default_gate()), &err);
            if (next == nullptr) {
              return std::unexpected(err.second);
            }
            return next;
          },
          [this](GenerationPtr &next, bess::dataplane::Retirer &retirer) {
            retirer.Retire(published_.Exchange(std::move(next)));
          },
          bess::dataplane::Resource::Footprint{.retires = 1},
          [this] {
            bess::pb::WildcardMatchSettings v;
            v.set_default_gate(published_.Read()->default_gate);
            return v;
          },
          nullptr);
      !registered) {
    DeInit();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void WildcardMatch::DeInit() {
  settings_.Release();
  if (resource_ == nullptr) {
    return;
  }
  if (!result_reference_names_.empty()) {
    // A rule's result field names entries in other resources, so the rules
    // may leave with live keys while the graph is disconnected around them:
    // the order modules are destroyed in must not decide whether a pipeline
    // can be torn down (D-172).
    const std::string name = resource_->name();
    auto released =
        init_context().resources().ReleaseForTeardown(
            std::span<const std::string>(&name, 1));
    CHECK(released) << released.error();
  } else {
    // Cannot be refused: the rules reference nothing and nothing may
    // reference them; freed rule ids still in the removal cascade complete
    // here (workers are paused).
    auto unregistered =
        init_context().resources().Unregister(resource_->name());
    CHECK(unregistered) << unregistered.error();
  }
  binding_.Reset();
  resource_.reset();
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
      static_cast<uint32_t>(key_size), max_tuples_,
      init_context().rcu());
  if (!table) {
    return std::unexpected(std::make_pair(EINVAL, table.error()));
  }
  return std::shared_ptr<bess::classifier::ConcurrentMaskedTable>(
      std::move(*table));
}

std::vector<bess::dataplane::Reference> WildcardMatch::ResultReferences(
    const std::vector<std::byte> &bytes) const {
  std::vector<bess::dataplane::Reference> refs;
  // Decode from the zero-padded record, the same bytes a later
  // replacement/erase reads from the arena (D-172).
  std::vector<std::byte> record(result_value_size_, std::byte{0});
  if (!bytes.empty()) {
    std::memcpy(record.data(), bytes.data(),
                std::min(bytes.size(), record.size()));
  }
  for (const ResultSpec &spec : result_specs_) {
    if (spec.reference.empty() ||
        spec.value_offset + spec.size > record.size()) {
      continue;
    }
    bool zero = true;
    for (uint32_t i = 0; i < spec.size; i++) {
      if (record[spec.value_offset + i] != std::byte{0}) {
        zero = false;
        break;
      }
    }
    if (zero) {
      continue;
    }
    if (spec.reference_be32) {
      const auto *at = record.data() + spec.value_offset;
      const uint32_t b0 = static_cast<uint32_t>(at[0]);
      const uint32_t b1 = static_cast<uint32_t>(at[1]);
      const uint32_t b2 = static_cast<uint32_t>(at[2]);
      const uint32_t b3 = static_cast<uint32_t>(at[3]);
      const uint32_t id = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
      refs.push_back({spec.reference, bess::dataplane::EncodeKey(id)});
    } else {
      refs.push_back(
          {spec.reference,
           std::string(reinterpret_cast<const char *>(record.data() +
                                                      spec.value_offset),
                       spec.size)});
    }
  }
  return refs;
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

  std::optional<bess::classifier::ResultPlan> result;
  std::vector<size_t> result_offsets;
  bool needs_metadata = false;
  for (const ResultSpec &spec : result_specs_) {
    needs_metadata = needs_metadata || spec.by_attr;
  }
  if (records_ != nullptr && !metadata_ready_ && needs_metadata) {
    // Metadata offsets are resolved at PreResume; see ExactMatch::Build.
    result_offsets.assign(result_specs_.size(), kInvalidOffset);
  } else if (records_ != nullptr) {
    bess::classifier::RuntimeClassifierSchema schema;
    schema.key_size = layout.key_size;
    schema.value_size = result_value_size_;
    schema.bounds = bess::classifier::BoundsPolicy::kCheck;
    for (const ResultSpec &spec : result_specs_) {
      bess::classifier::RuntimeResultField field;
      field.value_offset = spec.value_offset;
      field.size = spec.size;
      if (spec.by_attr) {
        const bess::metadata::mt_offset_t offset =
            attr_offset(static_cast<size_t>(spec.attr_id));
        if (!bess::metadata::IsValidOffset(offset)) {
          *err = std::make_pair(
              EINVAL, "result field '" + spec.attr_name +
                          "' has no readable metadata offset");
          return nullptr;
        }
        field.destination = bess::classifier::BufferKind::kMetadata;
        field.destination_offset =
            static_cast<size_t>(static_cast<uint8_t>(offset));
        result_offsets.push_back(static_cast<size_t>(
            static_cast<uint8_t>(offset)));
      } else {
        field.destination = bess::classifier::BufferKind::kPacket;
        field.destination_offset = spec.offset;
        result_offsets.push_back(spec.offset);
      }
      schema.result_fields.push_back(std::move(field));
    }
    auto compiled = bess::classifier::ResultPlan::Compile(schema);
    if (!compiled) {
      *err = std::make_pair(EINVAL, "result plan: " +
                                        compiled.error().message);
      return nullptr;
    }
    result = std::move(*compiled);
  }
  auto gen = std::make_unique<Generation>(
      default_gate, std::move(*plan), std::move(table), layout.key_size,
      /*extraction_valid=*/true, std::move(layout.baked_offsets),
      std::move(result), std::move(result_offsets), records_);
  gen->result_plan_deferred = !result_specs_.empty() && !metadata_ready_ &&
                              needs_metadata;
  return gen;
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
      /*extraction_valid=*/false, std::move(layout.baked_offsets),
      std::nullopt,
      std::vector<size_t>(result_specs_.size(), kInvalidOffset), records_);
}

void WildcardMatch::RefreshForResume() {
  metadata_ready_ = true;
  const Generation *current = published_.Read();
  if (current == nullptr) {
    return;  // not initialized (or already deinitialized)
  }

  bool changed = false;
  for (size_t i = 0; i < result_specs_.size(); i++) {
    const ResultSpec &spec = result_specs_[i];
    const size_t now =
        spec.by_attr
            ? static_cast<size_t>(attr_offset(
                  static_cast<size_t>(spec.attr_id)))
            : kInvalidOffset;
    if (i >= current->baked_result_offsets.size() ||
        current->baked_result_offsets[i] != now) {
      LOG(ERROR) << "WildcardMatch '" << name()
                 << "': a result field's destination moved (another module "
                    "replaced its attribute): results take their configured "
                    "offset or stay zero";
      changed = true;
      break;
    }
  }
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
  if (!changed && !current->result_plan_deferred) {
    return;
  }

  Error err;
  GenerationPtr next = Build(current->table, current->default_gate, &err);
  if (next != nullptr) {
    published_.Publish(std::move(next));
    init_context().rcu().ReclaimReady();
    return;
  }

  // The dispatcher ignores ordinary OnEvent errors and resume would proceed
  // with a stale plan reading a reassigned metadata region. Fail closed
  // instead: keep serving rules/default-gate introspection, but route every
  // packet to the default gate.
  LOG(ERROR) << "WildcardMatch '" << name() << "': metadata refresh failed ("
             << err.second << "); routing all packets to the default gate";
  published_.Publish(BuildDegraded(current->table, current->default_gate));
  init_context().rcu().ReclaimReady();
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
  init_context().rcu().ReclaimReady();
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

// The packet path's per-batch decision, shared by ProcessBatch and
// ClassifyBatch: calls emit(i, gate) once per packet, in order. `emit` is
// inlined into each caller.
template <typename Emit>
inline void WildcardMatch::Classify(bess::PacketBatch *batch, Emit &&emit,
                                    uint64_t *out_hits) const {
  // One snapshot for the whole batch: a concurrent command can neither swap
  // the generation mid-batch nor free it under this lookup.
  const Generation *gen = published_.Read();
  const gate_idx_t default_gate = gen->default_gate;
  const int cnt = batch->cnt();

  if (!gen->extraction_valid) {
    // Fail-closed generation (metadata offsets unreadable): route everything
    // to the default gate without touching packet or metadata bytes.
    for (int i = 0; i < cnt; i++) {
      emit(i, default_gate, 0);
    }
    return;
  }

  // Stack scratch only: no packet-path allocation.
  const size_t key_size = gen->key_size;
  std::array<bess::classifier::SourceView, bess::PacketBatch::kMaxBurst>
      sources;
  std::array<std::byte, bess::PacketBatch::kMaxBurst * kMaxKeyBytes> keys;
  std::array<bess::classifier::ConcurrentMaskedTable::Match,
             bess::PacketBatch::kMaxBurst>
      matches;

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
      key_size, matches.data(), static_cast<size_t>(cnt),
      records_ != nullptr);
  hits &= valid;
  if (out_hits != nullptr) {
    *out_hits = hits;
  }

  for (int i = 0; i < cnt; i++) {
    const bool hit = (hits & (uint64_t{1} << i)) != 0;
    const gate_idx_t gate = hit ? matches[i].gate : default_gate;
    // A rule's record id lives in the rule record, so only a module with
    // result fields reads one, and only for a winner. A miss reports 0.
    const uint32_t result_id =
        hit && records_ != nullptr
            ? gen->table->RuleAt(matches[i].rule_id).result_id
            : 0;
    emit(i, gate, result_id);
  }
}

void WildcardMatch::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const Generation *gen = published_.Read();
  const gate_idx_t default_gate = gen->default_gate;
  if (!gen->extraction_valid) {
    RunChooseModule(ctx, default_gate, batch);
    return;
  }
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  if (!gen->result) {
    Classify(batch, [&](int i, gate_idx_t gate, uint32_t) {
      gates[i] = gate;
    });
    EmitBatch(ctx, batch, gates);
    return;
  }

  // Placement runs for every row that has a record: a hit's own, and the
  // configured default record on a miss. A miss without one writes nothing.
  uint32_t record_ids[bess::PacketBatch::kMaxBurst];
  uint64_t hits = 0;
  Classify(
      batch,
      [&](int i, gate_idx_t gate, uint32_t result_id) {
        gates[i] = gate;
        record_ids[i] = result_id;
      },
      &hits);
  for (int i = 0; i < batch->cnt(); i++) {
    if ((hits & (uint64_t{1} << static_cast<unsigned>(i))) == 0) {
      record_ids[i] = default_record_id_;
    }
  }

  const int cnt = batch->cnt();
  const bool packet_fields = gen->result->required_packet_bytes() != 0;
  std::array<bess::classifier::ConstBytes, bess::PacketBatch::kMaxBurst> values;
  std::array<bess::classifier::DestinationView, bess::PacketBatch::kMaxBurst>
      destinations;
  int rows = 0;
  for (int i = 0; i < cnt; i++) {
    if (record_ids[i] == 0) {
      continue;
    }
    bess::PacketRef pkt = batch->packet(i);
    bess::classifier::MutableBytes packet_view;
    if (packet_fields) {
      bess::PacketHandle handle = pkt.handle();
      if (!bess::packet::EnsureWritable(handle)) {
        gates[i] = DROP_GATE;
        continue;
      }
      batch->handles()[i] = handle;
      pkt = bess::PacketRef(handle);
      if (pkt.data_len() < gen->result->required_packet_bytes()) {
        gates[i] = DROP_GATE;
        continue;
      }
      packet_view = bess::classifier::MutableBytes(
          pkt.head_data<std::byte *>(), static_cast<size_t>(pkt.data_len()));
    }
    values[rows] = gen->records->Lookup(
        static_cast<bess::classifier::RecordArena::Id>(record_ids[i]));
    destinations[rows] = bess::classifier::DestinationView{
        .packet = packet_view,
        .metadata = bess::classifier::MutableBytes(
            reinterpret_cast<std::byte *>(pkt.metadata()), SNBUF_METADATA)};
    rows++;
  }
  if (rows > 0 &&
      !gen->result->ApplyBatch(
          std::span<const bess::classifier::ConstBytes>(
              values.data(), static_cast<size_t>(rows)),
          std::span<const bess::classifier::DestinationView>(
              destinations.data(), static_cast<size_t>(rows)))) {
    RunChooseModule(ctx, default_gate, batch);
    return;
  }
  EmitBatch(ctx, batch, gates);
}

void WildcardMatch::ClassifyBatch(bess::PacketBatch *batch, gate_idx_t *gates,
                                  uint32_t *result_ids) const {
  Classify(batch, [&](int i, gate_idx_t gate, uint32_t result_id) {
    gates[i] = gate;
    if (result_ids != nullptr) {
      result_ids[i] = result_id;
    }
  });
}

std::string WildcardMatch::GetDesc() const {
  return bess::utils::Format("%zu fields, %zu rules", field_specs_.size(),
                             table_->size());
}

ADD_MODULE(WildcardMatch, "wm",
           "Multi-field classifier with a wildcard match table")
