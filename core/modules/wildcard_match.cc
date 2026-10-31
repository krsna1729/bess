// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "wildcard_match.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/memory_scope.h"
#include "framework/resource_bindings.h"
#include "framework/table_batch.h"
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
  bess::framework::KeyField spec;
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

  keys_.Add(std::move(spec));
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

  keys_.Clear();
  for (int i = 0; i < arg.fields_size(); i++) {
    CommandResponse err = AddFieldOne(arg.fields(i), i);
    if (err.error().code() != 0) {
      return err;
    }
  }

  if (arg.has_default_gate() && !bess::IsValidGateValue(arg.default_gate())) {
    return CommandFailure(EINVAL, "Invalid default gate: %llu",
                          static_cast<unsigned long long>(arg.default_gate()));
  }
  // The record arena ("results") and the rule table ("rules") are admitted
  // after every argument check (D-198).
  if (CommandResponse results = results_.Init(
          *this, arg.result_fields(),
          arg.has_default_result()
              ? std::optional<std::string_view>(arg.default_result())
              : std::nullopt,
          init_context().rcu(), init_context().memory(), "results");
      results.error().code() != 0) {
    return results;
  }
  auto table = NewTable();
  if (!table) {
    return CommandFailure(table.error().first, "%s",
                          table.error().second.c_str());
  }
  Error err;
  const gate_idx_t default_gate =
      arg.has_default_gate() ? static_cast<gate_idx_t>(arg.default_gate()) : DROP_GATE;
  if (arg.has_default_gate()) {
    initial_default_gate_ = arg.default_gate();
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
          .check_value = [this](const MaskedRuleResource::Value &v) {
            return results_.CheckRule(v.gate, v.result.size());
          },
          .references = [this](const MaskedRuleResource::Value &v) {
            return results_.References(v.result);
          },
          .records = results_.records(),
          .may_reference = results_.reference_names()});
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
        *arg.mutable_ranges() = key.ranges();
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
            bess::classifier::ConstBytes(value.data(), value.size()),
            rule.ranges);
      },
      [this](const bess::pb::WildcardMatchRuleValue &value)
          -> std::expected<std::any, std::string> {
        if (auto ok = results_.CheckRule(value.gate(), value.result().size());
            !ok) {
          return std::unexpected(ok.error());
        }
        return std::any(MaskedRuleResource::Value{
            value.priority(), static_cast<uint16_t>(value.gate()),
            std::vector<std::byte>(
                reinterpret_cast<const std::byte *>(value.result().data()),
                reinterpret_cast<const std::byte *>(value.result().data()) +
                    value.result().size())});
      },
      // Every rule as a client sends it: per-field value and mask bytes,
      // full field width (the table holds them canonical), and its ranges
      // in field order, as the resource enumerates them. Resumable: the
      // cursor is the table's WalkCursor (capacity, slot, then the mask).
      [this](std::string_view cursor,
             const bess::framework::TypedCodec<
                 bess::pb::WildcardMatchRuleKey,
                 bess::pb::WildcardMatchRuleValue>::TypedPageVisitor &visit) -> std::string {
        bess::classifier::ConcurrentMaskedTable::WalkCursor at;
        std::array<uint32_t, 2> fields{};  // capacity, slot
        if (std::string_view mask; bess::framework::CursorFields(cursor, &fields, &mask)) {
          const auto *begin = reinterpret_cast<const std::byte *>(mask.data());
          at = {std::vector<std::byte>(begin, begin + mask.size()), fields[0], fields[1]};
        }
        bess::pb::WildcardMatchRuleKey key;
        bess::pb::WildcardMatchRuleValue value;
        const bool more = table_->VisitFrom(
            &at,
            [&](bess::classifier::ConstBytes mask,
                bess::classifier::ConstBytes bytes,
                const bess::classifier::ConcurrentMaskedTable::Rule &r,
                bess::classifier::ConcurrentMaskedTable::Ranges ranges) {
              key.Clear();
              value.Clear();
              size_t pos = 0;
              auto range = ranges.begin();
              for (size_t i = 0; i < keys_.size(); i++) {
                const size_t n = static_cast<size_t>(keys_.fields()[i].size);
                key.add_values()->set_value_bin(
                    reinterpret_cast<const char *>(bytes.data() + pos), n);
                key.add_masks()->set_value_bin(
                    reinterpret_cast<const char *>(mask.data() + pos), n);
                if (range != ranges.end() && range->offset == pos) {
                  bess::pb::WildcardMatchFieldRange *out = key.add_ranges();
                  out->set_field(static_cast<uint32_t>(i));
                  out->set_low(range->low);
                  out->set_high(range->high);
                  ++range;
                }
                pos += n;
              }
              value.set_priority(r.priority);
              value.set_gate(r.gate);
              if (const auto &records = results_.records(); records != nullptr) {
                const bess::classifier::ConstBytes record =
                    records->Lookup(r.result_id);
                value.set_result(reinterpret_cast<const char *>(record.data()),
                                 record.size());
              }
              return visit(key, value);
            });
        if (!more) {
          return {};
        }
        return bess::framework::CursorOf<2>(
            {at.capacity, at.slot},
            std::string_view(reinterpret_cast<const char *>(at.mask.data()), at.mask.size()));
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
  bess::framework::ReleaseTableResource(init_context().resources(),
                                        resource_->name(),
                                        !results_.reference_names().empty());
  binding_.Reset();
  resource_.reset();
}

Error WildcardMatch::PackRule(const Rule &rule, std::vector<std::byte> *value,
                              std::vector<std::byte> *mask) const {
  const std::vector<bess::framework::KeyField> &fields = keys_.fields();
  if (rule.values.size() != fields.size() ||
      rule.masks.size() != fields.size()) {
    return std::make_pair(
        EINVAL, bess::utils::Format("rule should have %zu fields (has %zu)",
                                    fields.size(), rule.values.size()));
  }
  value->clear();
  mask->clear();
  for (size_t i = 0; i < rule.values.size(); i++) {
    const size_t want = static_cast<size_t>(fields[i].size);
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
  using bess::classifier::ConcurrentMaskedTable;
  const auto key_len = static_cast<uint32_t>(keys_.key_size());
  // Admitted before it is built (D-198): a refusal throws MemoryRefused and
  // builds nothing. The table then admits what it grows by (record chunks,
  // tuple tables, chains) through the lease's hook, and gives back tuple
  // tables and chains as RCU frees them. The admission travels with the
  // table: it is released after the table is destroyed (the last generation
  // or retire callback holding it), so the hook outlives every free.
  bess::framework::MemoryLease lease =
      init_context().memory().LeaseFor<ConcurrentMaskedTable>(
          "rules", key_len, max_tuples_, init_context().rcu());
  auto table =
      ConcurrentMaskedTable::Create(key_len, max_tuples_, init_context().rcu());
  if (!table) {
    return std::unexpected(std::make_pair(EINVAL, table.error()));
  }
  auto owner = std::make_shared<bess::framework::Sized<ConcurrentMaskedTable>>(
      bess::framework::MemoryScope::Adopt(std::move(lease), std::move(*table)));
  ConcurrentMaskedTable *raw = owner->get();
  return std::shared_ptr<ConcurrentMaskedTable>(std::move(owner), raw);
}

WildcardMatch::GenerationPtr WildcardMatch::Build(
    std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table,
    gate_idx_t default_gate, Error *err) {
  // Wildcard masks belong to the rules, not to extraction: no normalization.
  std::optional<bess::classifier::CompiledKey> key = keys_.Compile(*this, err);
  if (!key) {
    return nullptr;
  }
  CHECK_EQ(key->key_size, table->key_len());
  bess::classifier::CompiledResult result;
  if (!results_.Compile(*this, key->key_size, &result, err)) {
    return nullptr;
  }
  return std::make_unique<Generation>(default_gate, std::move(table),
                                      std::move(*key), std::move(result));
}

WildcardMatch::GenerationPtr WildcardMatch::BuildDegraded(
    std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table,
    gate_idx_t default_gate) {
  return std::make_unique<Generation>(
      default_gate, std::move(table),
      keys_.CompileFailClosed(*this, "WildcardMatch"),
      results_.CompileFailClosed());
}

void WildcardMatch::RefreshForResume() {
  results_.set_metadata_ready();
  const Generation *current = published_.Read();
  if (current == nullptr) {
    return;  // not initialized (or already deinitialized)
  }
  const auto results = results_.Compare(*this, current->result);
  if (results.moved) {
    LOG(ERROR) << "WildcardMatch '" << name()
               << "': a result field's destination moved (another module "
                  "replaced its attribute): results take their configured "
                  "offset or stay zero";
  }
  if (!results.changed && !keys_.Changed(*this, current->key) &&
      !current->result.deferred) {
    return;
  }
  // A failed rebuild fails closed: rules and default gate stay introspectable,
  // every packet takes the default gate.
  bess::framework::RebuildGeneration(
      published_, "WildcardMatch '" + name() + "'",
      [&](Error *err) {
        return Build(current->table, current->default_gate, err);
      },
      [&] { return BuildDegraded(current->table, current->default_gate); });
}

int WildcardMatch::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  RefreshForResume();
  // Return 0 (not -ENOTSUP) to stay registered for future resumes.
  return 0;
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
                         keys_.fields()[i].size, &out->back(), "field", i, &err)) {
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
  const std::vector<bess::framework::KeyField> &fields = keys_.fields();
  if (static_cast<size_t>(arg.values_size()) != fields.size()) {
    return std::make_pair(
        EINVAL, bess::utils::Format("must specify %zu values", fields.size()));
  }
  if (static_cast<size_t>(arg.masks_size()) != fields.size()) {
    return std::make_pair(
        EINVAL, bess::utils::Format("must specify %zu masks", fields.size()));
  }

  Error ret = RuleFieldsFromPb(arg.values(), fields.size(), &rule->values);
  if (ret.first) {
    return ret;
  }
  ret = RuleFieldsFromPb(arg.masks(), fields.size(), &rule->masks);
  if (ret.first) {
    return ret;
  }

  // The canonical-rule invariant, checked per field exactly as the legacy
  // per-field check did (and again on the dense key by the substrate).
  for (size_t i = 0; i < fields.size(); i++) {
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

  // Ranges (D-186): refused rather than widened. The field's mask must be
  // zero (the range replaces it); a range over the whole field is the rule
  // without it, so it is dropped; low == high stays a range.
  rule->ranges.clear();
  std::vector<size_t> offsets;
  size_t pos = 0;
  for (const bess::framework::KeyField &spec : fields) {
    offsets.push_back(pos);
    pos += static_cast<size_t>(spec.size);
  }
  std::vector<bool> ranged(fields.size(), false);
  for (const bess::pb::WildcardMatchFieldRange &range : arg.ranges()) {
    const uint32_t field = range.field();
    if (field >= fields.size()) {
      return std::make_pair(
          EINVAL, bess::utils::Format("range on field %u: the module has %zu "
                                      "fields",
                                      field, fields.size()));
    }
    if (ranged[field]) {
      return std::make_pair(
          EINVAL,
          bess::utils::Format("field %u has more than one range", field));
    }
    ranged[field] = true;
    const auto width = static_cast<uint32_t>(fields[field].size);
    const uint64_t max =
        width == 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * width)) - 1;
    if (range.low() > range.high() || range.high() > max) {
      return std::make_pair(
          EINVAL, bess::utils::Format("field %u: range [%llu, %llu] is empty "
                                      "or wider than the %u-byte field",
                                      field,
                                      static_cast<unsigned long long>(range.low()),
                                      static_cast<unsigned long long>(range.high()),
                                      width));
    }
    for (const uint8_t m : rule->masks[field]) {
      if (m != 0) {
        return std::make_pair(
            EINVAL, bess::utils::Format("field %u has a range and a nonzero "
                                        "mask",
                                        field));
      }
    }
    if (range.low() == 0 && range.high() == max) {
      continue;
    }
    rule->ranges.push_back({.offset = static_cast<uint32_t>(offsets[field]),
                            .width = width,
                            .low = range.low(),
                            .high = range.high()});
  }
  std::ranges::sort(rule->ranges, {},
                    &bess::classifier::ConcurrentMaskedTable::KeyRange::offset);

  rule->priority = arg.priority();
  rule->gate = gate;
  return std::make_pair(0, std::string());
}

// The packet path's per-batch decision, shared by ProcessBatch and
// ClassifyBatch: calls emit(i, gate) once per packet, in order. `emit` is
// inlined into each caller.
template <typename Emit>
inline void WildcardMatch::Classify(bess::PacketBatch *batch, bool with_result,
                                    Emit &&emit, uint32_t miss_result_id) const {
  // One snapshot for the whole batch: a concurrent command can neither swap
  // the generation mid-batch nor free it under this lookup.
  const Generation *gen = published_.Read();
  const int cnt = batch->cnt();

  if (!gen->key.valid) {
    // Fail-closed generation (metadata offsets unreadable): route everything
    // to the default gate without touching packet or metadata bytes.
    const gate_idx_t default_gate = gen->default_gate;
    for (int i = 0; i < cnt; i++) {
      emit(i, default_gate, 0);
    }
    return;
  }

  // Stack scratch only: no packet-path allocation. The masked backend
  // examines every row, so failed rows hold zeros; their results are
  // discarded through `valid` below regardless.
  const size_t key_size = gen->key.key_size;
  std::array<std::byte, bess::PacketBatch::kMaxBurst * kMaxKeyBytes> keys;
  std::array<bess::classifier::ConcurrentMaskedTable::Match,
             bess::PacketBatch::kMaxBurst>
      matches;
  const uint64_t valid = bess::framework::ExtractKeys(gen->key, *batch, keys.data());

  // A rule's record id lives in the rule record, so only a caller that places
  // results (or reports their ids) reads one, and only for a winner.
  const bool with_rule_id = with_result && results_.records() != nullptr;
  uint64_t hits = gen->table->LookupBatch(
      bess::classifier::ConstBytes(keys.data(),
                                   static_cast<size_t>(cnt) * key_size),
      key_size, matches.data(), static_cast<size_t>(cnt), with_rule_id);
  hits &= valid;
  // Read after the lookup call, not before it: a value live across the call
  // takes a callee-saved register, and with none left the compiler reloaded
  // it from the stack for every miss. Loaded here it stays in a register for
  // the loop. (A generation is immutable; this reads the same value.)
  const gate_idx_t default_gate = gen->default_gate;

  for (int i = 0; i < cnt; i++) {
    const bool hit = (hits & (uint64_t{1} << i)) != 0;
    const gate_idx_t gate = hit ? matches[i].gate : default_gate;
    const uint32_t result_id =
        hit ? (with_rule_id ? gen->table->RuleAt(matches[i].rule_id).result_id : 0)
            : miss_result_id;
    emit(i, gate, result_id);
  }
}

void WildcardMatch::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const Generation *gen = published_.Read();
  const gate_idx_t default_gate = gen->default_gate;
  if (!gen->key.valid) {
    RunChooseModule(ctx, default_gate, batch);
    return;
  }
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  if (!gen->result.plan) {
    // No plan (gate-only, or every result field compiled out): no rule ids.
    Classify(batch, /*with_result=*/false, [&](int i, gate_idx_t gate, uint32_t) {
      gates[i] = gate;
    });
    EmitBatch(ctx, batch, gates);
    return;
  }

  // Placement runs for every row that has a record: a hit's own, and the
  // configured default record on a miss. A miss without one writes nothing.
  uint32_t record_ids[bess::PacketBatch::kMaxBurst];
  Classify(
      batch, /*with_result=*/true,
      [&](int i, gate_idx_t gate, uint32_t result_id) {
        gates[i] = gate;
        record_ids[i] = result_id;
      },
      results_.default_record_id());
  if (!bess::framework::PlaceResults(*gen->result.plan, *gen->result.records,
                                     batch, record_ids, gates)) {
    RunChooseModule(ctx, default_gate, batch);
    return;
  }
  EmitBatch(ctx, batch, gates);
}

void WildcardMatch::ClassifyBatch(bess::PacketBatch *batch, gate_idx_t *gates,
                                  uint32_t *result_ids) const {
  Classify(batch, /*with_result=*/true,
           [&](int i, gate_idx_t gate, uint32_t result_id) {
    gates[i] = gate;
    if (result_ids != nullptr) {
      result_ids[i] = result_id;
    }
  });
}

std::string WildcardMatch::GetDesc() const {
  return bess::utils::Format("%zu fields, %zu rules", keys_.size(),
                             table_->size());
}

ADD_MODULE(WildcardMatch, "wm",
           "Multi-field classifier with a wildcard match table")
