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
#include "packet_reshape.h"
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

std::vector<bess::dataplane::Reference> ExactMatch::ResultReferences(
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
      continue;  // an all-zero field names nothing (0 = none, D-141)
    }
    if (spec.reference_be32) {
      const auto *at = record.data() + spec.value_offset;
      for (uint32_t i = 0; i < spec.reference_count; i++) {
        const auto *chunk = at + static_cast<size_t>(i) * 4;
        const uint32_t id =
            (static_cast<uint32_t>(chunk[0]) << 24U) |
            (static_cast<uint32_t>(chunk[1]) << 16U) |
            (static_cast<uint32_t>(chunk[2]) << 8U) |
            static_cast<uint32_t>(chunk[3]);
        if (id == 0) {
          continue;  // 0 names nothing at this position (D-141)
        }
        refs.push_back({spec.reference, bess::dataplane::EncodeKey(id)});
      }
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

size_t ExactMatch::ResultDestination(const ResultSpec &spec) const {
  if (!spec.by_attr) {
    return spec.offset;
  }
  const bess::metadata::mt_offset_t offset = attr_offset(spec.attr_id);
  if (bess::metadata::IsValidOffset(offset)) {
    return static_cast<size_t>(static_cast<uint8_t>(offset));
  }
  // No downstream reader: the metadata model lets the writer skip the write.
  return offset == bess::metadata::kMetadataOffsetNoWrite ? kSkippedOffset
                                                          : kInvalidOffset;
}

bool ExactMatch::CompileResultPlan(size_t key_size,
                                   std::optional<classifier::ResultPlan> *plan,
                                   std::vector<size_t> *baked_offsets,
                                   Error *err) const {
  classifier::RuntimeClassifierSchema schema;
  schema.key_size = key_size;
  schema.value_size = result_value_size_;
  schema.bounds = classifier::BoundsPolicy::kCheck;
  baked_offsets->clear();
  baked_offsets->reserve(result_specs_.size());
  for (const ResultSpec &spec : result_specs_) {
    const size_t at = ResultDestination(spec);
    baked_offsets->push_back(at);
    if (at == kSkippedOffset) {
      continue;  // compiled out: nothing downstream reads it
    }
    if (at == kInvalidOffset) {
      *err = std::make_pair(EINVAL, "result field '" + spec.attr_name +
                                        "' has no readable metadata offset");
      return false;
    }
    classifier::RuntimeResultField field;
    field.value_offset = spec.value_offset;
    field.size = spec.size;
    field.destination = spec.by_attr ? classifier::BufferKind::kMetadata
                                     : classifier::BufferKind::kPacket;
    field.destination_offset = at;
    schema.result_fields.push_back(std::move(field));
  }
  plan->reset();
  if (schema.result_fields.empty()) {
    return true;  // every field compiled out: gate-only
  }
  auto compiled = classifier::ResultPlan::Compile(schema);
  if (!compiled) {
    *err = std::make_pair(EINVAL, "result plan: " + compiled.error().message);
    return false;
  }
  *plan = std::move(*compiled);
  return true;
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

  std::vector<size_t> result_offsets;
  std::optional<classifier::ResultPlan> result;
  bool needs_metadata = false;
  if (!result_specs_.empty()) {
    for (const ResultSpec &spec : result_specs_) {
      needs_metadata = needs_metadata || spec.by_attr;
    }
    if (!metadata_ready_ && needs_metadata) {
      // Metadata offsets are resolved at PreResume; a plan now would bake
      // every attribute as offset 0 and reject distinct outputs as
      // overlapping. RefreshForResume rebuilds with the resolved offsets.
      result_offsets.assign(result_specs_.size(), kInvalidOffset);
    } else {
      if (!CompileResultPlan(layout.key_size, &result, &result_offsets, err)) {
        return nullptr;
      }
    }
  }
  auto gen = std::make_unique<Generation>(
      default_gate, std::move(*plan), std::move(table), layout.key_size,
      /*extraction_valid=*/true, std::move(layout.baked_offsets),
      std::move(result), std::move(result_offsets), records_);
  gen->result_plan_deferred = !result_specs_.empty() && !metadata_ready_ &&
                              needs_metadata;

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

  // The degraded generation never places results: its extraction is invalid,
  // so ProcessBatch routes every packet to the default gate without touching
  // packet or metadata bytes.
  std::vector<size_t> result_offsets(result_specs_.size(), kInvalidOffset);
  std::optional<classifier::ResultPlan> result;

  return std::make_unique<Generation>(
      default_gate, std::move(*plan), std::move(table), layout.key_size,
      /*extraction_valid=*/false, std::move(layout.baked_offsets),
      std::move(result), std::move(result_offsets), records_);
}

void ExactMatch::RefreshForResume() {
  metadata_ready_ = true;
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
  if (!changed && !result_specs_.empty()) {
    for (size_t i = 0; i < result_specs_.size(); i++) {
      if (i >= current->baked_result_offsets.size() ||
          current->baked_result_offsets[i] !=
              ResultDestination(result_specs_[i])) {
        changed = true;
        break;
      }
    }
  }
  if (!changed && !current->result_plan_deferred) {
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

  // Result placement: each field maps a slice of the rule's record to a
  // metadata attribute (added here, before Build, so the plan bakes its
  // offset) or a packet byte offset. A rule's record is zero-padded to the
  // widest field's end.
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
    spec.reference_count =
        field.reference_count() == 0 ? 1 : field.reference_count();
    if (spec.reference_be32) {
      if (spec.reference.empty()) {
        return CommandFailure(EINVAL,
                              "a BE32 reference names no resource");
      }
      if (static_cast<uint64_t>(spec.size) !=
          4ull * spec.reference_count) {
        return CommandFailure(
            EINVAL,
            "a BE32 reference carries reference_count 4-byte ids: num_bytes "
            "must be 4 * reference_count");
      }
    } else if (spec.reference_count != 1) {
      return CommandFailure(
          EINVAL, "reference_count applies to BE32 references only");
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
  if (result_value_size_ > classifier::RecordArena::kMaxRecordSize) {
    return CommandFailure(
        EINVAL, "result record is %zu bytes, the maximum is %zu",
        result_value_size_, classifier::RecordArena::kMaxRecordSize);
  }
  records_ = result_specs_.empty()
                 ? nullptr
                 : std::make_shared<classifier::RecordArena>(
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
    classifier::MutableBytes slot = records_->Record(default_record_id_);
    std::memcpy(slot.data(), default_result_->data(),
                default_result_->size());
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
              [this](const classifier::ExactRuleResource::Value &value)
              -> classifier::ExactRuleResource::Result {
            if (!bess::IsValidGateValue(value.gate)) {
              return std::unexpected("invalid gate " +
                                     std::to_string(value.gate));
            }
            if (value.bytes.size() > result_value_size_) {
              return std::unexpected(
                  "result is " + std::to_string(value.bytes.size()) +
                  " bytes, the schema's record is " +
                  std::to_string(result_value_size_));
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
          .references = [this](const classifier::ExactRuleResource::Value &v) {
            return ResultReferences(v.bytes);
          },
          .records = records_.get(),
          .may_reference = result_reference_names_});
  // Typed keys and values over the RPC (D-025): the rule's fields, packed
  // here as the add command packs them, and the rule's value -- the gate and
  // its result record.
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
        classifier::RuleResult result;
        result.gate = static_cast<gate_idx_t>(value.gate());
        const auto *begin =
            reinterpret_cast<const std::byte *>(value.result().data());
        result.bytes.assign(begin, begin + value.result().size());
        return std::any(std::move(result));
      },
      // Read back (D-129) from the table the command path and transactions
      // write: each key is the rule's fields packed in field order, stored
      // unmasked as given, so cutting it at the field sizes gives back every
      // field's bytes. Canonical form: every field as value_bin (one a client
      // sent as value_int decodes to the same packed key), and the value as
      // its gate and result record (D-172).
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
          value.set_gate(bess::GateOfResult(v));
          if (records_ != nullptr) {
            const classifier::ConstBytes record =
                records_->Lookup(bess::ResultIdOfResult(v));
            value.set_result(record.data(), record.size());
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
  if (!result_reference_names_.empty()) {
    // A rule's result field names entries in other resources, so the rules
    // may leave with live keys while the graph is disconnected around them:
    // the order modules are destroyed in must not decide whether a pipeline
    // can be torn down (D-172).
    const std::string name = resource_->name();
    auto released =
        engine.ReleaseForTeardown(std::span<const std::string>(&name, 1));
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
// `value` is the matched rule's packed {gate, result id} and `hit` says
// whether the packet matched. `emit` is inlined into each caller; `gen` is the
// caller's one snapshot for the batch.
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
  if (!gen->extraction_valid) {
    RunChooseModule(ctx, default_gate, batch);
    return;
  }
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  if (!gen->result) {
    Classify(batch, *gen, [&](int i, uint64_t value, bool hit) {
      gates[i] = hit ? bess::GateOfResult(value) : default_gate;
    });
    EmitBatch(ctx, batch, gates);
    return;
  }

  // Placement runs for every row that has a record: a hit's own, and the
  // application's default record on a miss. A miss without one writes
  // nothing and only takes the default gate.
  uint32_t record_ids[bess::PacketBatch::kMaxBurst];
  Classify(batch, *gen, [&](int i, uint64_t value, bool hit) {
    gates[i] = hit ? bess::GateOfResult(value) : default_gate;
    record_ids[i] = hit ? bess::ResultIdOfResult(value) : default_record_id_;
  });

  const int cnt = batch->cnt();
  const bool packet_fields = gen->result->required_packet_bytes() != 0;
  std::array<classifier::ConstBytes, bess::PacketBatch::kMaxBurst> values;
  std::array<classifier::DestinationView, bess::PacketBatch::kMaxBurst>
      destinations;
  int rows = 0;
  for (int i = 0; i < cnt; i++) {
    if (record_ids[i] == 0) {
      continue;
    }
    bess::PacketRef pkt = batch->packet(i);
    classifier::MutableBytes packet_view;
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
      packet_view = classifier::MutableBytes(
          pkt.head_data<std::byte *>(), static_cast<size_t>(pkt.data_len()));
    }
    values[rows] = records_->Lookup(static_cast<classifier::RecordArena::Id>(record_ids[i]));
    destinations[rows] = classifier::DestinationView{
        .packet = packet_view,
        .metadata = classifier::MutableBytes(
            reinterpret_cast<std::byte *>(pkt.metadata()), SNBUF_METADATA)};
    rows++;
  }
  if (rows > 0 &&
      !gen->result->ApplyBatch(
          std::span<const classifier::ConstBytes>(values.data(),
                                                  static_cast<size_t>(rows)),
          std::span<const classifier::DestinationView>(
              destinations.data(), static_cast<size_t>(rows)))) {
    // The checks above make this unreachable for a valid schema; fail closed
    // rather than emit a packet whose fields were only partly written.
    RunChooseModule(ctx, default_gate, batch);
    return;
  }
  EmitBatch(ctx, batch, gates);
}

// The decision without placing: gates and the matched rule's record id (0 on
// a miss), for tests and benchmarks that drive the module from a reader
// thread. The packet path's own miss behaviour is the default gate.
void ExactMatch::ClassifyBatch(bess::PacketBatch *batch, gate_idx_t *gates,
                               uint32_t *result_ids) const {
  const Generation *gen = published_.Read();
  const gate_idx_t default_gate =
      gen->default_gate.load(std::memory_order_relaxed);
  Classify(batch, *gen, [&](int i, uint64_t value, bool hit) {
    gates[i] = hit ? bess::GateOfResult(value) : default_gate;
    result_ids[i] = hit ? bess::ResultIdOfResult(value) : 0;
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
