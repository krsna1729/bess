// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "exact_match.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "event.h"
#include "framework/memory_scope.h"
#include "framework/resource_bindings.h"
#include "framework/table_batch.h"
#include "metadata.h"
#include "rcu/rcu_ptr.h"
#include "utils/bits.h"
#include "utils/endian.h"
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

// The generic path: extract every key with the generation's dense key
// (failed rows zeroed), then look the batch up; the backend looks up every
// row. Sets *valid to the extraction mask; returns the hit mask.
uint64_t LookupExtracted(const bess::PacketBatch &batch,
                         const classifier::CompiledKey &key,
                         const classifier::ConcurrentExactTable &table,
                         std::byte *keys, uint64_t *values,
                         uint64_t *valid) noexcept {
  const auto cnt = static_cast<size_t>(batch.cnt());
  *valid = bess::framework::ExtractKeys(key, batch, keys);
  return table.LookupBatch(classifier::ConstBytes(keys, cnt * key.key_size),
                           key.key_size, values, cnt);
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
  if (mask.encoding_case() == bess::pb::FieldData::kValueInt) {
    mask64 = mask.value_int();
  } else if (mask.encoding_case() == bess::pb::FieldData::kValueBin) {
    const std::string &bin = mask.value_bin();
    if (bin.size() > sizeof(mask64)) {
      // The legacy parser copied the full bytestring over the stack-local
      // mask word (buffer over-read/over-write); reject it instead.
      return CommandFailure(EINVAL, "idx %d: not a valid %d-byte mask", idx,
                            size);
    }
    bess::utils::Copy(reinterpret_cast<uint8_t *>(&mask64), bin.data(),
                      bin.size());
  }

  bess::framework::KeyField spec;
  spec.size = size;
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

  keys_.Add(std::move(spec));
  field_masks_.push_back(mask64);
  return CommandSuccess();
}

bool ExactMatch::FieldMask(size_t i, classifier::Normalization *out,
                           Error *err) const {
  const bess::framework::KeyField &field = keys_.fields()[i];
  std::vector<std::byte> converted;
  bool is_default = false;
  if (!ConvertMask(field_masks_[i], field.size, field.by_offset, &converted,
                   &is_default, err, static_cast<int>(i))) {
    return false;
  }
  if (!is_default) {
    out->mask = std::move(converted);
  }
  return true;
}

Error ExactMatch::PackKey(const std::vector<std::vector<uint8_t>> &fields,
                         std::vector<std::byte> *key) const {
  const std::vector<bess::framework::KeyField> &specs = keys_.fields();
  if (fields.size() != specs.size()) {
    return std::make_pair(
        EINVAL, bess::utils::Format("rule should have %zu fields (has %zu)",
                                    specs.size(), fields.size()));
  }
  // Rule bytes are packed densely in field order WITHOUT applying the mask --
  // the legacy table copied rule bytes verbatim while masking only extracted
  // packet/metadata bytes. Preserve that distinction.
  key->clear();
  for (size_t i = 0; i < fields.size(); i++) {
    const size_t want = static_cast<size_t>(specs[i].size);
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
  // Decision D-010: room for `rules` plus the grace-period headroom.
  const auto key_len = static_cast<uint32_t>(keys_.key_size());
  const uint32_t capacity =
      classifier::ConcurrentExactTable::CapacityFor(rules);
  bess::framework::MemoryScope &memory = init_context().memory();
  // Admitted before it is built (D-198): a refusal throws MemoryRefused and
  // builds nothing. The admission travels with the table: every generation
  // that maps it shares the owner, so a table replaced by growth is released
  // when its last generation is reclaimed after the grace period (the
  // generation's RCU retire carries it; nothing is released at publish).
  bess::framework::MemoryLease lease =
      memory.LeaseFor<classifier::ConcurrentExactTable>(
          "rules", key_len, capacity, init_context().rcu(), memory.socket());
  auto table = classifier::ConcurrentExactTable::Create(
      key_len, capacity, init_context().rcu(), memory.socket());
  if (!table) {
    return std::unexpected(std::make_pair(ENOMEM, table.error()));
  }
  auto owner =
      std::make_shared<bess::framework::Sized<classifier::ConcurrentExactTable>>(
          bess::framework::MemoryScope::Adopt(std::move(lease),
                                              std::move(*table)));
  classifier::ConcurrentExactTable *raw = owner->get();
  return std::shared_ptr<classifier::ConcurrentExactTable>(std::move(owner),
                                                           raw);
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
  const bool published = bess::framework::RepublishGeneration(
      published_,
      [&](const Generation &current) {
        return Build(next_table,
                     current.default_gate.load(std::memory_order_relaxed), err);
      },
      err);
  if (published) {
    table_ = std::move(next_table);
  }
  return published;
}

ExactMatch::GenerationPtr ExactMatch::Build(
    std::shared_ptr<classifier::ConcurrentExactTable> table,
    gate_idx_t default_gate, Error *err) {
  const auto mask = [this](size_t i, classifier::Normalization *out,
                           Error *e) { return FieldMask(i, out, e); };
  std::optional<classifier::CompiledKey> key = keys_.Compile(*this, err, mask);
  if (!key) {
    return nullptr;
  }
  CHECK_EQ(key->key_size, table->key_len());
  // Metadata destinations wait for PreResume (deferred): before it every
  // attribute reads offset 0, and distinct outputs would overlap.
  classifier::CompiledResult result;
  if (!results_.Compile(*this, key->key_size, &result, err)) {
    return nullptr;
  }
  auto gen = std::make_unique<Generation>(default_gate, std::move(table),
                                          std::move(*key), std::move(result));

  // The fused kernel (D-121) takes a key that is exactly one packet field of
  // 1, 2, 4 or 8 bytes, checked against the first segment (kCheck).
  const classifier::ExtractPlan &extract = gen->key.extract;
  const size_t width = gen->key.key_size;
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
  // Never places results: its extraction is invalid, so ProcessBatch routes
  // every packet to the default gate without touching packet or metadata
  // bytes.
  const auto mask = [this](size_t i, classifier::Normalization *out,
                           Error *e) { return FieldMask(i, out, e); };
  return std::make_unique<Generation>(
      default_gate, std::move(table),
      keys_.CompileFailClosed(*this, "ExactMatch", mask),
      results_.CompileFailClosed());
}

void ExactMatch::RefreshForResume() {
  results_.set_metadata_ready();
  const Generation *current = published_.Read();
  if (current == nullptr) {
    return;  // not initialized (or already deinitialized)
  }
  if (!keys_.Changed(*this, current->key) &&
      !results_.Compare(*this, current->result).changed &&
      !current->result.deferred) {
    return;
  }
  // A failed rebuild fails closed: rules and default gate stay introspectable,
  // every packet takes the default gate.
  const gate_idx_t default_gate =
      current->default_gate.load(std::memory_order_relaxed);
  bess::framework::RebuildGeneration(
      published_, "ExactMatch '" + name() + "'",
      [&](Error *err) { return Build(current->table, default_gate, err); },
      [&] { return BuildDegraded(current->table, default_gate); });
}

int ExactMatch::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  RefreshForResume();
  // Return 0 (not -ENOTSUP) to stay registered for future resumes.
  return 0;
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
  keys_.Clear();
  field_masks_.clear();
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

  if (arg.has_default_gate() && !bess::IsValidGateValue(arg.default_gate())) {
    return CommandFailure(EINVAL, "Invalid default gate: %llu",
                          static_cast<unsigned long long>(arg.default_gate()));
  }

  // Result placement: each field maps a slice of the rule's record to a
  // metadata attribute (added here, before Build, so the plan bakes its
  // offset) or a packet byte offset. A rule's record is zero-padded to the
  // widest field's end. The record arena ("results") and the rule table
  // ("rules") are admitted after every argument check (D-198).
  if (CommandResponse results = results_.Init(
          *this, arg.result_fields(),
          arg.has_default_result()
              ? std::optional<std::string_view>(arg.default_result())
              : std::nullopt,
          init_context().rcu(), init_context().memory(), "results");
      results.error().code() != 0) {
    return results;
  }

  auto table = NewTable(0);
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
              [this](const classifier::ExactRuleResource::Value &value) {
                return results_.CheckRule(value.gate, value.bytes.size());
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
            return results_.References(v.bytes);
          },
          .records = results_.records().get(),
          .may_reference = results_.reference_names()});
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
        if (auto ok = results_.CheckRule(value.gate(), value.result().size());
            !ok) {
          return std::unexpected(ok.error());
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
          for (const bess::framework::KeyField &spec : keys_.fields()) {
            key.add_fields()->set_value_bin(
                reinterpret_cast<const char *>(packed.data() + pos),
                static_cast<size_t>(spec.size));
            pos += static_cast<size_t>(spec.size);
          }
          value.Clear();
          value.set_gate(bess::GateOfResult(v));
          if (const auto &records = results_.records(); records != nullptr) {
            const classifier::ConstBytes record =
                records->Lookup(bess::ResultIdOfResult(v));
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
  bess::framework::ReleaseTableResource(init_context().resources(),
                                        resource_->name(),
                                        !results_.reference_names().empty());
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

  if (!gen.key.valid) {
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
          : LookupExtracted(*batch, gen.key, *gen.table, keys.data(),
                            gates.data(), &valid);
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
  if (!gen->key.valid) {
    RunChooseModule(ctx, default_gate, batch);
    return;
  }
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  if (!gen->result.plan) {
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
    record_ids[i] =
        hit ? bess::ResultIdOfResult(value) : results_.default_record_id();
  });
  if (!bess::framework::PlaceResults(*gen->result.plan, *gen->result.records,
                                     batch, record_ids, gates)) {
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
  const bool published = bess::framework::RepublishGeneration(
      published_,
      [&](const Generation &current) {
        return Build(table_,
                     current.default_gate.load(std::memory_order_relaxed), err);
      },
      err);
  if (!published) {
    fused_enabled_ = was;
  }
  return published;
}

std::string ExactMatch::GetDesc() const {
  return bess::utils::Format("%zu fields, %zu rules", keys_.size(),
                             table_->size());
}

Error ExactMatch::RuleFieldsFromPb(
    const RepeatedPtrField<bess::pb::FieldData> &fields,
    std::vector<std::vector<uint8_t>> *rule) {
  const std::vector<bess::framework::KeyField> &specs = keys_.fields();
  if (fields.size() != static_cast<int>(specs.size())) {
    return std::make_pair(
        EINVAL, bess::utils::Format("rule should have %zu fields (has %d)",
                                    specs.size(), fields.size()));
  }

  for (auto i = 0; i < fields.size(); i++) {
    int field_size = specs[i].size;

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
