// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "exact_match.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <limits>
#include <string>
#include <span>
#include <utility>
#include <vector>

#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "event.h"
#include "metadata.h"
#include "snbuf_layout.h"
#include "utils/bits.h"
#include "utils/endian.h"
#include "rcu/rcu_ptr.h"
#include "utils/format.h"

namespace classifier = bess::classifier;

// D-140: every legacy command is gone. The rules are the "<name>/rules"
// resource and the default gate the "<name>/settings" resource; the creation
// argument is GetPipeline's Module.arg.
const Commands ExactMatch::cmds = {};

const ResourceTables ExactMatch::resource_tables = {
    ResourceTable::Map<bess::pb::ExactMatchRuleKey,
                       bess::pb::ExactMatchRuleValue>("rules"),
    ResourceTable::Singleton<bess::pb::ExactMatchSettings>("settings"),
};

namespace {

// Converts a raw configured mask into the byte mask applied on extraction,
// mirroring ExactMatchTable::DoAddField (framework/exact_match_table.h):
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

// The generic path: extract every key with the generation's plan, then look
// the batch up. ExactMatch's key layout is dense, so a fully-covering plan
// writes every byte of every valid row and the used region needs no
// pre-zeroing; a failed extraction writes nothing, and invalid rows are
// zeroed before the backend (which looks up all rows) sees them. Gapped
// generic schemas keep the caller-initializes-gap contract and pre-zero
// instead. Sets *valid to the extraction mask; returns the hit mask.
uint64_t LookupExtracted(const bess::PacketBatch &batch,
                         const classifier::ExtractPlan &extract,
                         size_t key_size,
                         const classifier::ConcurrentExactTable &table,
                         std::byte *keys, uint64_t *values,
                         uint64_t *valid) noexcept {
  const auto cnt = static_cast<size_t>(batch.cnt());
  const classifier::MutableBytes rows(keys, cnt * key_size);
  if (!extract.fully_covers_key()) {
    std::memset(rows.data(), 0, rows.size());
  }
  std::array<classifier::SourceView, bess::PacketBatch::kMaxBurst> sources;
  for (size_t i = 0; i < cnt; i++) {
    const bess::PacketRef pkt = batch.packet(i);
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
  const uint64_t ok = extract.ExecuteBatch(
      std::span<const classifier::SourceView>(sources).first(cnt), rows,
      key_size);
  // Invalid rows hold stale scratch (or partial gap bytes); zero them so the
  // backend sees well-defined keys. Their results are discarded through the
  // validity mask regardless.
  const uint64_t all_valid = (uint64_t{1} << cnt) - 1;
  if ((ok & all_valid) != all_valid) {
    for (size_t i = 0; i < cnt; i++) {
      if (!(ok & (uint64_t{1} << i))) {
        std::memset(keys + i * key_size, 0, key_size);
      }
    }
  }
  *valid = ok;
  return table.LookupBatch(rows, key_size, values, cnt);
}

// The fused fixed-layout kernel (D-121), for a key that is one packet field
// of sizeof(Word) bytes at `offset`: per packet one length check against the
// first segment, one load, the mask, the key row and its CRC32C
// (detail::CrcFixed, rte_hash's own hash, D-015); then one prehashed bulk
// lookup. A packet too short for the field fails extraction as under the
// generic plan's kCheck: nothing past data_len is read, its row is zero and
// its bit in *valid is clear. Returns the lookup's hit mask.
template <typename Word>
uint64_t LookupFused(const bess::PacketBatch &batch, size_t offset,
                     uint64_t mask, const classifier::ConcurrentExactTable &table,
                     std::byte *keys, uint64_t *values,
                     uint64_t *valid) noexcept {
  constexpr size_t kWidth = sizeof(Word);
  const auto word_mask = static_cast<Word>(mask);
  const size_t need = offset + kWidth;
  const auto cnt = static_cast<size_t>(batch.cnt());
  hash_sig_t sigs[bess::PacketBatch::kMaxBurst];
  uint64_t ok = 0;
  for (size_t i = 0; i < cnt; i++) {
    const bess::PacketRef pkt = batch.packet(i);
    Word v = 0;
    if (static_cast<size_t>(pkt.data_len()) >= need) {
      std::memcpy(&v, pkt.head_data<const std::byte *>() + offset, kWidth);
      v &= word_mask;
      ok |= uint64_t{1} << i;
    }
    std::byte *row = keys + i * kWidth;
    std::memcpy(row, &v, kWidth);
    sigs[i] = classifier::detail::CrcFixed<kWidth>(row);
  }
  *valid = ok;
  return table.LookupBatchPrehashed(classifier::ConstBytes(keys, cnt * kWidth),
                                    kWidth, sigs, values, cnt);
}

// LookupFused for a width chosen at publish: 1, 2, 4 or 8.
uint64_t LookupFused(size_t width, const bess::PacketBatch &batch,
                     size_t offset, uint64_t mask,
                     const classifier::ConcurrentExactTable &table,
                     std::byte *keys, uint64_t *values,
                     uint64_t *valid) noexcept {
  switch (width) {
    case 1:
      return LookupFused<uint8_t>(batch, offset, mask, table, keys, values,
                                  valid);
    case 2:
      return LookupFused<uint16_t>(batch, offset, mask, table, keys, values,
                                   valid);
    case 4:
      return LookupFused<uint32_t>(batch, offset, mask, table, keys, values,
                                   valid);
    default:
      return LookupFused<uint64_t>(batch, offset, mask, table, keys, values,
                                   valid);
  }
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
  layout->baked_offsets.clear();
  layout->metadata_valid = true;
  layout->key_fields.reserve(field_specs_.size());
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
      field.normalization.mask = std::move(converted);
    }
    layout->key_fields.push_back(std::move(field));
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
      init_context().rcu());
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
    return Build(next_table,
                 current.default_gate.load(std::memory_order_relaxed), err);
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

  auto gen = std::make_unique<Generation>(
      default_gate, std::move(*plan), std::move(table), layout.key_size,
      /*extraction_valid=*/true, std::move(layout.baked_offsets));

  // The fused kernel (D-121) takes a key that is exactly one packet field of
  // 1, 2, 4 or 8 bytes, checked against the first segment (kCheck).
  const classifier::ExtractPlan &extract = gen->extract;
  const size_t width = gen->key_size;
  if (fused_enabled_ &&
      extract.kernel() == classifier::ExtractKernel::kSinglePacket &&
      extract.bounds() == classifier::BoundsPolicy::kCheck &&
      extract.fully_covers_key() &&
      (width == 1 || width == 2 || width == 4 || width == 8)) {
    const classifier::ExtractOp &op = extract.ops()[0];
    gen->fused_offset = op.source_offset;
    gen->fused_mask = ~uint64_t{0};
    if (!op.mask.empty()) {
      gen->fused_mask = 0;
      std::memcpy(&gen->fused_mask, op.mask.data(), width);
    }
    gen->fused_width = width;
  }
  return gen;
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
      /*extraction_valid=*/false, std::move(layout.baked_offsets));
}

void ExactMatch::RefreshForResume() {
  const Generation *current = published_.Read();
  if (current == nullptr) {
    return;  // not initialized (or already deinitialized)
  }

  // Action mode: metadata offsets were just assigned, so an unreadable
  // 'action_id' means every match will take the default gate. Say it once per
  // resume; the packet path only fails closed.
  if (action_mode_ &&
      !bess::metadata::IsValidOffset(attr_offset(action_id_attr_))) {
    LOG(ERROR) << "ExactMatch '" << name()
               << "': the 'action_id' attribute is unreadable (is an "
                  "ActionTable downstream?); matches take the default gate";
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
  const gate_idx_t default_gate =
      current->default_gate.load(std::memory_order_relaxed);
  GenerationPtr next = Build(current->table, default_gate, &err);
  if (next != nullptr) {
    published_.Publish(std::move(next));
    init_context().rcu().ReclaimReady();
    return;
  }

  // The dispatcher ignores ordinary OnEvent errors and resume would proceed
  // with a stale plan reading a reassigned metadata region. Fail closed
  // instead: keep serving rules/default-gate introspection, but route every
  // packet to the default gate.
  LOG(ERROR) << "ExactMatch '" << name()
             << "': metadata refresh failed (" << err.second
             << "); routing all packets to the default gate";
  published_.Publish(BuildDegraded(current->table, default_gate));
  init_context().rcu().ReclaimReady();
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
  init_context().rcu().ReclaimReady();
  return true;
}

CommandResponse ExactMatch::Init(const bess::pb::ExactMatchArg &arg) {
  action_mode_ = !arg.action_resource().empty();
  action_resource_ = arg.action_resource();
  // No masks at all means "match every bit" for every field.
  const bool empty_masks = arg.masks_size() == 0;
  if (arg.fields_size() != arg.masks_size() && !empty_masks) {
    return CommandFailure(EINVAL,
                          "must provide masks for all fields (or no masks for "
                          "default match on all bits on all fields)");
  }

  // AddFieldOne only records the configuration; Build() below is what
  // validates it against the module (attr names) and applies it.
  field_specs_.clear();
  for (auto i = 0; i < arg.fields_size(); ++i) {
    CommandResponse err;

    if (empty_masks) {
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
  if (arg.has_default_gate() && !bess::IsValidGateValue(arg.default_gate())) {
    return CommandFailure(EINVAL, "Invalid default gate: %llu",
                          static_cast<unsigned long long>(arg.default_gate()));
  }
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

  // The rules as a transactional resource (D-022), so that one transaction
  // can change them together with other modules' tables. Keys are the packed
  // rule keys (the fields' bytes in order), values the gates.
  resource_ = std::make_unique<classifier::ExactRuleResource>(
      name() + "/rules",
      classifier::ExactRuleResource::Hooks{
          .table = [this]() -> classifier::ConcurrentExactTable & {
            return *table_;
          },
          .check_value =
              [this](uint64_t value) -> classifier::ExactRuleResource::Result {
            if (action_mode_) {
              if (value == 0 ||
                  value > std::numeric_limits<uint32_t>::max()) {
                return std::unexpected("invalid action id " +
                                       std::to_string(value));
              }
              return {};
            }
            if (!bess::IsValidGateValue(value)) {
              return std::unexpected("invalid gate " + std::to_string(value));
            }
            return {};
          },
          .make_room =
              [this](bool force) -> classifier::ExactRuleResource::Result {
            Error grow_err;
            if (!EnsureCapacity(force, &grow_err)) {
              return std::unexpected(grow_err.second);
            }
            return {};
          },
          // Action mode: every rule names an action, so the engine orders the
          // actions before the rules and refuses a rule whose action is
          // missing (G1.2b).
          .references =
              action_mode_
                  ? classifier::ExactRuleResource::ReferencesFn(
                        [this](uint64_t value) {
                          return std::vector<bess::dataplane::Reference>{
                              {action_resource_,
                               bess::dataplane::EncodeKey(
                                   bess::dataplane::ActionId(
                                       static_cast<uint32_t>(value)))}};
                        })
                  : classifier::ExactRuleResource::ReferencesFn(),
          .may_reference = action_mode_
                               ? std::vector<std::string>{action_resource_}
                               : std::vector<std::string>{}});
  // Typed keys and values over the RPC (D-025): the rule's fields, packed
  // here as the add command packs them, and the rule's value -- the gate, or
  // the action id in action mode.
  binding_ = init_context().codecs().Bind(
      *resource_,std::make_shared<bess::framework::TypedCodec<
                          bess::pb::ExactMatchRuleKey,
                          bess::pb::ExactMatchRuleValue>>(
      [this](const bess::pb::ExactMatchRuleKey &key)
          -> std::expected<bess::dataplane::ResourceKey, std::string> {
        std::vector<std::vector<uint8_t>> fields;
        Error err = RuleFieldsFromPb(key.fields(), &fields);
        std::vector<std::byte> packed;
        if (!err.first) {
          err = PackKey(fields, &packed);
        }
        if (err.first) {
          return std::unexpected(err.second);
        }
        return bess::dataplane::ResourceKey(
            reinterpret_cast<const char *>(packed.data()), packed.size());
      },
      [this](const bess::pb::ExactMatchRuleValue &value)
          -> std::expected<std::any, std::string> {
        if (action_mode_) {
          if (value.gate() != 0) {
            return std::unexpected(
                "this ExactMatch names actions ('action_resource' is set): "
                "a rule carries 'action_id', not 'gate'");
          }
          if (value.action_id() == 0) {
            return std::unexpected("action id 0 is not a valid action");
          }
          return std::any(uint64_t{value.action_id()});
        }
        if (!bess::IsValidGateValue(value.gate())) {
          return std::unexpected("invalid gate " +
                                 std::to_string(value.gate()));
        }
        if (value.action_id() != 0) {
          return std::unexpected(
              "'action_id' needs an 'action_resource' on this ExactMatch");
        }
        return std::any(uint64_t{value.gate()});
      },
      // Read back (D-129) from the table the command path and transactions
      // write: each key is the rule's fields packed in field order, stored
      // unmasked as given, so cutting it at the field sizes gives back every
      // field's bytes. Canonical form: every field as value_bin (one a client
      // sent as value_int decodes to the same packed key), and the value as
      // the mode's one field.
      [this](const auto &visit) {
        bess::pb::ExactMatchRuleKey key;
        bess::pb::ExactMatchRuleValue value;
        table_->ForEach([&](classifier::ConstBytes packed, uint64_t v) {
          key.Clear();
          size_t pos = 0;
          for (const FieldSpec &spec : field_specs_) {
            key.add_fields()->set_value_bin(
                reinterpret_cast<const char *>(packed.data() + pos),
                static_cast<size_t>(spec.size));
            pos += static_cast<size_t>(spec.size);
          }
          value.Clear();
          if (action_mode_) {
            value.set_action_id(static_cast<uint32_t>(v));
          } else {
            value.set_gate(v);
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

  if (action_mode_) {
    // The packet path delivers a match as an ActionId in metadata; the graph
    // connects gate 0 to the ActionTable that reads it.
    action_id_attr_ =
        AddMetadataAttr("action_id", sizeof(bess::utils::be32_t),
                        bess::metadata::Attribute::AccessMode::kWrite);
    if (action_id_attr_ < 0) {
      const std::string name = resource_->name();
      CHECK(init_context().resources().Unregister(name));
      binding_.Reset();
      resource_.reset();
      return CommandFailure(-action_id_attr_, "add_metadata_attr() failed");
    }
  }

  // The default gate as the singleton "<name>/settings" (D-129): the gate is
  // validated by the resource's codec. Applied while workers run: a relaxed
  // store into the published generation, which workers read once per batch.
  if (auto registered = settings_.Register(
          init_context().resources(), init_context().codecs(),
          name() + "/settings",
          [](const bess::pb::ExactMatchSettings &v)
              -> std::expected<gate_idx_t, std::string> {
            if (!bess::IsValidGateValue(v.default_gate())) {
              return std::unexpected(
                  "Invalid gate: " + std::to_string(v.default_gate()));
            }
            return static_cast<gate_idx_t>(v.default_gate());
          },
          [this](gate_idx_t &gate) {
            published_.Read()->default_gate.store(gate,
                                                  std::memory_order_relaxed);
          },
          [this] {
            bess::pb::ExactMatchSettings v;
            v.set_default_gate(
                published_.Read()->default_gate.load(std::memory_order_relaxed));
            return v;
          },
          nullptr);
      !registered) {
    const std::string name = resource_->name();
    CHECK(init_context().resources().Unregister(name));
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void ExactMatch::DeInit() {
  settings_.Release();
  if (resource_ == nullptr) {
    return;
  }
  auto &engine = init_context().resources();
  if (action_mode_) {
    // In action mode the rules reference an ActionTable, so they may leave
    // with live keys while the graph is disconnected around them: the order
    // modules are destroyed in must not decide whether a pipeline can be torn
    // down.
    const std::string name = resource_->name();
    auto released = engine.ReleaseForTeardown(
        std::span<const std::string>(&name, 1));
    CHECK(released) << released.error();
  } else {
    // Cannot be refused: the rules reference nothing and nothing may
    // reference them (their erases are immediate), and Apply() runs under the
    // control-plane lock this runs under.
    auto unregistered = engine.Unregister(resource_->name());
    CHECK(unregistered) << unregistered.error();
  }
  binding_.Reset();
  resource_.reset();
}

// The packet path's per-batch decision, shared by ProcessBatch and
// ClassifyBatch: calls emit(i, value, hit) once per packet, in order, where
// `value` is the matched rule's value -- a gate in gate mode, an action id in
// action mode -- and `hit` says whether the packet matched. `emit` is inlined
// into each caller; `gen` is the caller's one snapshot for the batch.
template <typename Emit>
inline void ExactMatch::Classify(bess::PacketBatch *batch,
                                const Generation &gen, Emit &&emit) const {
  const int cnt = batch->cnt();

  if (!gen.extraction_valid) {
    // Fail-closed generation (metadata offsets unreadable): route everything
    // to the default gate without touching packet or metadata bytes.
    for (int i = 0; i < cnt; i++) {
      emit(i, 0, false);
    }
    return;
  }

  // Stack scratch only: no packet-path allocation. A generation with a fused
  // kernel (one packet field of 1, 2, 4 or 8 bytes, chosen in Build) reads,
  // hashes and looks up in one pass (D-121); every other layout extracts with
  // its plan, then looks up.
  std::array<std::byte, bess::PacketBatch::kMaxBurst * kMaxKeyBytes> keys;
  std::array<uint64_t, bess::PacketBatch::kMaxBurst> gates;
  uint64_t valid = 0;
  uint64_t hits =
      gen.fused_width != 0
          ? LookupFused(gen.fused_width, *batch, gen.fused_offset,
                        gen.fused_mask, *gen.table, keys.data(), gates.data(),
                        &valid)
          : LookupExtracted(*batch, gen.extract, gen.key_size, *gen.table,
                            keys.data(), gates.data(), &valid);
  hits &= valid;
  // A key a transaction is still preparing holds kPending: a miss (D-022).
  // A separate branch-free pass, which the compiler vectorizes; folding the
  // compare into the loop below cost 4% on an E-core (ABBA).
  uint64_t pending = 0;
  for (int i = 0; i < cnt; i++) {
    pending |= uint64_t{gates[i] == classifier::ExactRuleResource::kPending}
               << i;
  }
  hits &= ~pending;

  for (int i = 0; i < cnt; i++) {
    const bool hit = (hits & (uint64_t{1} << i)) != 0;
    emit(i, hit ? gates[i] : 0, hit);
  }
}

void ExactMatch::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  // One snapshot for the whole batch: a concurrent command can neither swap
  // the generation mid-batch nor free it under this lookup.
  const Generation *gen = published_.Read();
  const gate_idx_t default_gate =
      gen->default_gate.load(std::memory_order_relaxed);
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  if (!action_mode_) {
    Classify(batch, *gen, [&](int i, uint64_t value, bool hit) {
      gates[i] = hit ? static_cast<gate_idx_t>(value) : default_gate;
    });
    EmitBatch(ctx, batch, gates);
    return;
  }

  // Action mode: a match's value is an ActionId, which goes to the ActionTable
  // in packet metadata; a miss takes the default gate.
  const bess::metadata::mt_offset_t offset = attr_offset(action_id_attr_);
  if (!bess::metadata::IsValidOffset(offset)) {
    // Fail closed: a match whose action id cannot be delivered would reach the
    // ActionTable with a stale id, so it takes the default gate instead.
    // RefreshForResume() is where this is reported.
    RunChooseModule(ctx, default_gate, batch);
    return;
  }
  Classify(batch, *gen, [&](int i, uint64_t value, bool hit) {
    if (!hit) {
      gates[i] = default_gate;
      return;
    }
    _set_attr_with_offset<bess::utils::be32_t>(
        offset, batch->packet(i), bess::utils::be32_t(static_cast<uint32_t>(value)));
    gates[i] = kActionGate;
  });
  EmitBatch(ctx, batch, gates);
}

// The gate-mode helper cannot carry a 32-bit action id.
void ExactMatch::ClassifyBatch(bess::PacketBatch *batch,
                               gate_idx_t *gates) const {
  DCHECK(!action_mode_);
  const Generation *gen = published_.Read();
  const gate_idx_t default_gate =
      gen->default_gate.load(std::memory_order_relaxed);
  Classify(batch, *gen, [&](int i, uint64_t value, bool hit) {
    gates[i] = hit ? static_cast<gate_idx_t>(value) : default_gate;
  });
}

void ExactMatch::ClassifyActionsBatch(bess::PacketBatch *batch,
                                     uint32_t *action_ids) const {
  DCHECK(action_mode_);
  const Generation *gen = published_.Read();
  Classify(batch, *gen, [&](int i, uint64_t value, bool hit) {
    action_ids[i] = hit ? static_cast<uint32_t>(value) : 0;
  });
}

size_t ExactMatch::fused_key_width() const {
  return published_.Read()->fused_width;
}

bool ExactMatch::SetFusedKernelEnabled(bool enabled, Error *err) {
  const bool was = fused_enabled_;
  fused_enabled_ = enabled;
  const bool published = Publish([&](const Generation &current) {
    return Build(table_, current.default_gate.load(std::memory_order_relaxed),
                 err);
  }, err);
  if (!published) {
    fused_enabled_ = was;
  }
  return published;
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

ADD_MODULE(ExactMatch, "em", "Multi-field classifier with an exact match table")
