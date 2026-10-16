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
  has_action_resource_ = !arg.action_resource().empty();
  action_resource_ = arg.action_resource();
  if (has_action_resource_) {
    // D-141: the ActionId of a matching rule goes to the ActionTable in packet
    // metadata. Added before the layout is built, so the plan bakes the field
    // offsets this attribute introduces.
    action_id_attr_ =
        AddMetadataAttr("action_id", sizeof(bess::utils::be32_t),
                        bess::metadata::Attribute::AccessMode::kWrite);
    if (action_id_attr_ < 0) {
      return CommandFailure(-action_id_attr_, "add_metadata_attr() failed");
    }
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
            if (!has_action_resource_ && v.action_id != 0) {
              return std::unexpected(
                  "'action_id' needs an 'action_resource' on this "
                  "WildcardMatch");
            }
            return {};
          },
          // A rule that names an action is ordered after it and refused while
          // it is missing (G1.2b); action id 0 references nothing (D-141).
          .references = [this](const MaskedRuleResource::Value &v) {
            if (!has_action_resource_ || v.action_id == 0) {
              return std::vector<bess::dataplane::Reference>{};
            }
            return std::vector<bess::dataplane::Reference>{
                {action_resource_,
                 bess::dataplane::EncodeKey(
                     bess::dataplane::ActionId(v.action_id))}};
          },
          .may_reference = has_action_resource_
                               ? std::vector<std::string>{action_resource_}
                               : std::vector<std::string>{}});
  // Typed keys and values over the RPC (D-025): values and masks per field,
  // checked and packed as the class does for a rule, and
  // {priority, gate, action_id}.
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
        if (!has_action_resource_ && value.action_id() != 0) {
          return std::unexpected(
              "'action_id' needs an 'action_resource' on this WildcardMatch");
        }
        return std::any(MaskedRuleResource::Value{
            value.priority(), static_cast<uint16_t>(value.gate()),
            has_action_resource_ ? value.action_id() : 0});
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
              value.set_action_id(r.action_id);
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
  auto &engine = init_context().resources();
  if (has_action_resource_) {
    // In action mode the rules reference an ActionTable, so they may leave
    // with live keys while the graph is disconnected around them: the order
    // modules are destroyed in must not decide whether a pipeline can be torn
    // down (D-141).
    const std::string name = resource_->name();
    auto released =
        engine.ReleaseForTeardown(std::span<const std::string>(&name, 1));
    CHECK(released) << released.error();
  } else {
    // Cannot be refused: the rules reference nothing and nothing may
    // reference them; freed rule ids still in the removal cascade complete
    // here (workers are paused).
    auto unregistered = engine.Unregister(resource_->name());
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

  // An action resource needs a readable "action_id" attribute: the packet path
  // writes it on every hit and fails closed without it. Reported here, where a
  // resume can still be diagnosed.
  if (has_action_resource_ &&
      !bess::metadata::IsValidOffset(attr_offset(action_id_attr_))) {
    LOG(ERROR) << "WildcardMatch '" << name()
               << "': the 'action_id' attribute is unreadable (is an "
                  "ActionTable reading it?): matches take the default gate";
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
inline void WildcardMatch::Classify(bess::PacketBatch *batch,
                                    Emit &&emit) const {
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
      has_action_resource_);
  hits &= valid;

  for (int i = 0; i < cnt; i++) {
    const bool hit = (hits & (uint64_t{1} << i)) != 0;
    const gate_idx_t gate = hit ? matches[i].gate : default_gate;
    // D-141: an action id lives in the rule's record, so only a module with an
    // action resource reads one, and only for a winner. A miss delivers 0, so
    // a packet cannot carry an id from an earlier lookup.
    const uint32_t action_id =
        hit && has_action_resource_
            ? gen->table->RuleAt(matches[i].rule_id).action_id
            : 0;
    emit(i, gate, action_id);
  }
}

void WildcardMatch::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  const bess::metadata::mt_offset_t offset =
      has_action_resource_ ? attr_offset(action_id_attr_)
                           : bess::metadata::mt_offset_t{-1};
  if (has_action_resource_ && !bess::metadata::IsValidOffset(offset)) {
    // Fail closed: a match whose action id cannot be delivered would reach the
    // action table with a stale id, so every packet takes the default gate.
    // RefreshForResume() is where this is reported.
    RunChooseModule(ctx, published_.Read()->default_gate, batch);
    return;
  }
  if (has_action_resource_) {
    uint32_t action_ids[bess::PacketBatch::kMaxBurst];
    ClassifyBatch(batch, gates, action_ids);
    for (int i = 0; i < batch->cnt(); i++) {
      _set_attr_with_offset<bess::utils::be32_t>(
          offset, batch->packet(i), bess::utils::be32_t(action_ids[i]));
    }
  } else {
    // Gate mode (D-141): no action id to deliver, so the batch is classified
    // into gates alone -- the record read and the id store do not happen.
    ClassifyBatch(batch, gates, nullptr);
  }
  EmitBatch(ctx, batch, gates);
}

void WildcardMatch::ClassifyBatch(bess::PacketBatch *batch, gate_idx_t *gates,
                                  uint32_t *action_ids) const {
  Classify(batch, [&](int i, gate_idx_t gate, uint32_t action_id) {
    gates[i] = gate;
    if (action_ids != nullptr) {
      action_ids[i] = action_id;
    }
  });
}

std::string WildcardMatch::GetDesc() const {
  return bess::utils::Format("%zu fields, %zu rules", field_specs_.size(),
                             table_->size());
}

ADD_MODULE(WildcardMatch, "wm",
           "Multi-field classifier with a wildcard match table")
