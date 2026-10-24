// SPDX-License-Identifier: BSD-3-Clause

// A classifier table bound to a module (D-189): the key fields it reads and
// the result fields it writes as metadata attributes or packet offsets,
// resolved against the module's current metadata offsets, compiled into the
// pieces a published generation holds, refreshed at PreResume, and published
// through an RcuPtr. A module holds one KeyFields per table key and one
// ResultFields per record layout, and its own generation struct around the
// CompiledKey/CompiledResult they produce; nothing here assumes one table
// per module.
//
// Header-only on purpose: bess_framework does not link bess_classifier
// (tools/layer_dag.json), so the classifier calls these make are emitted in
// the module or plugin that uses them, which may. The packet-path half is
// framework/table_batch.h.

#ifndef BESS_FRAMEWORK_TABLE_FIELDS_H_
#define BESS_FRAMEWORK_TABLE_FIELDS_H_

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "classifier/record_arena.h"
#include "classifier/table_schema.h"
#include "dataplane/resource_registry.h"
#include "gate.h"
#include "message.h"
#include "metadata.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"
#include "utils/logging.h"

namespace bess::framework {

using Error = std::pair<int, std::string>;

// One key field as configured: a packet offset, or a metadata attribute the
// module reads, registered once at Init (a rebuild that registered it again
// would fail with EEXIST).
struct KeyField {
  bool by_offset = true;
  int offset = 0;         // by_offset
  std::string attr_name;  // otherwise
  int attr_id = -1;
  int size = 0;
};

// No per-field normalization: the key is the bytes as extracted.
struct NoNormalization {
  bool operator()(size_t /*field*/, classifier::Normalization * /*out*/,
                  Error * /*err*/) const {
    return true;
  }
};

// The key fields of one table, in key order, and the module's limits on
// them (its wire compatibility; the classifier has none).
class KeyFields {
 public:
  KeyFields(size_t max_fields, size_t max_field_size)
      : max_fields_(max_fields), max_field_size_(max_field_size) {}

  void Clear() { fields_.clear(); }
  void Add(KeyField field) { fields_.push_back(std::move(field)); }
  [[nodiscard]] const std::vector<KeyField> &fields() const noexcept {
    return fields_;
  }
  [[nodiscard]] size_t size() const noexcept { return fields_.size(); }
  // The dense key width: the sum of the field sizes.
  [[nodiscard]] size_t key_size() const noexcept {
    size_t total = 0;
    for (const KeyField &field : fields_) {
      total += static_cast<size_t>(field.size);
    }
    return total;
  }

  // Field `i`'s source offset under the module's current metadata offsets:
  // its packet offset, its attribute's metadata offset, or
  // classifier::kUnresolvedOffset.
  [[nodiscard]] size_t SourceOffset(const Module &module, size_t i) const {
    const KeyField &field = fields_[i];
    if (field.by_offset) {
      return static_cast<size_t>(field.offset);
    }
    const metadata::mt_offset_t offset =
        module.attr_offset(static_cast<size_t>(field.attr_id));
    return metadata::IsValidOffset(offset) ? static_cast<size_t>(offset)
                                           : classifier::kUnresolvedOffset;
  }

  // The dense key under the current metadata offsets, checking the module's
  // limits and packet offsets 0-1024. `normalize(i, &normalization, err)`
  // gives field i its mask (or fails). An unresolved metadata field fails,
  // or with `tolerate_unresolved` marks the layout unresolved (the
  // fail-closed path).
  template <typename Normalize = NoNormalization>
  bool Layout(const Module &module, bool tolerate_unresolved,
              classifier::KeyLayout *layout, Error *err,
              Normalize &&normalize = {}) const {
    if (fields_.size() > max_fields_) {
      *err = Error(EINVAL, "too many fields (max " + std::to_string(max_fields_) + ")");
      return false;
    }
    *layout = classifier::KeyLayout();
    layout->key_fields.reserve(fields_.size());
    layout->baked_offsets.reserve(fields_.size());
    for (size_t i = 0; i < fields_.size(); i++) {
      const KeyField &field = fields_[i];
      const std::string idx = "idx " + std::to_string(i);
      if (field.size < 1 || static_cast<size_t>(field.size) > max_field_size_) {
        *err = Error(EINVAL, idx + ": 'size' must be in [1," +
                                 std::to_string(max_field_size_) + "]");
        return false;
      }
      if (field.by_offset && (field.offset < 0 || field.offset > 1024)) {
        *err = Error(EINVAL, idx + ": invalid 'offset'");
        return false;
      }
      const size_t source = SourceOffset(module, i);
      if (source == classifier::kUnresolvedOffset && !tolerate_unresolved) {
        *err = Error(EINVAL, idx + ": metadata attribute '" + field.attr_name +
                                 "' has no valid offset (pipeline graph "
                                 "changed?)");
        return false;
      }
      classifier::Normalization normalization;
      if (!normalize(i, &normalization, err)) {
        return false;
      }
      layout->Append(field.by_offset ? classifier::SourceKind::kPacket
                                     : classifier::SourceKind::kMetadata,
                     source, static_cast<size_t>(field.size),
                     std::move(normalization));
    }
    return true;
  }

  // The key a generation runs under the current metadata offsets; nullopt,
  // with *err set, when a field is invalid or unresolved.
  template <typename Normalize = NoNormalization>
  std::optional<classifier::CompiledKey> Compile(const Module &module, Error *err,
                                                 Normalize &&normalize = {}) const {
    classifier::KeyLayout layout;
    if (!Layout(module, /*tolerate_unresolved=*/false, &layout, err,
                std::forward<Normalize>(normalize))) {
      return std::nullopt;
    }
    auto key = classifier::CompileKey(std::move(layout));
    if (!key) {
      *err = Error(EINVAL, "extraction plan: " + key.error().message);
      return std::nullopt;
    }
    return std::move(*key);
  }

  // The key of a fail-closed generation: unresolved metadata tolerated, never
  // run (valid == false). Only for a configuration that compiled before, so
  // a failure is a programming error, reported under `label`.
  template <typename Normalize = NoNormalization>
  classifier::CompiledKey CompileFailClosed(const Module &module,
                                            std::string_view label,
                                            Normalize &&normalize = {}) const {
    classifier::KeyLayout layout;
    Error err;
    CHECK(Layout(module, /*tolerate_unresolved=*/true, &layout, &err,
                 std::forward<Normalize>(normalize)))
        << "degraded " << label << " build failed: " << err.second;
    CHECK_GT(layout.key_size, 0U);
    auto key = classifier::CompileKey(std::move(layout));
    CHECK(key.has_value()) << "degraded " << label << " plan failed to compile";
    key->valid = false;
    return std::move(*key);
  }

  // Whether some field's source moved since `key` was compiled.
  [[nodiscard]] bool Changed(const Module &module,
                             const classifier::CompiledKey &key) const {
    for (size_t i = 0; i < fields_.size(); i++) {
      if (i >= key.baked_offsets.size() ||
          key.baked_offsets[i] != SourceOffset(module, i)) {
        return true;
      }
    }
    return false;
  }

 private:
  size_t max_fields_;
  size_t max_field_size_;
  std::vector<KeyField> fields_;
};

// The result fields of one table and the records its entries name: each
// field maps a slice of an entry's record to a metadata attribute the module
// writes or to a packet offset. Empty: gate-only.
class ResultFields {
 public:
  // Parses `fields` (registering each metadata destination as a write
  // attribute of `module`) and `default_result`, the record a miss places
  // (nullopt when unset; zero-padded), and creates the record arena on `rcu`.
  CommandResponse Init(
      Module &module,
      const google::protobuf::RepeatedPtrField<bess::pb::ResultField> &fields,
      std::optional<std::string_view> default_result, rcu::RcuDomain &rcu) {
    schema_.Clear();
    attr_ids_.clear();
    records_.reset();
    default_record_id_ = 0;
    for (const bess::pb::ResultField &field : fields) {
      classifier::ResultFieldSpec spec;
      spec.value_offset = field.value_offset();
      spec.size = field.num_bytes();
      spec.reference = field.reference();
      spec.reference_be32 = field.reference_key() == bess::pb::ResultField::BE32;
      spec.reference_count = field.reference_count() == 0 ? 1 : field.reference_count();
      if (auto checked = classifier::ResultSchema::Check(spec); !checked) {
        return CommandFailure(EINVAL, "%s", checked.error().message.c_str());
      }
      int attr_id = -1;
      if (field.destination_case() == bess::pb::ResultField::kAttrName) {
        if (field.attr_name().empty()) {
          return CommandFailure(EINVAL, "a result field has an empty attribute");
        }
        spec.destination = classifier::BufferKind::kMetadata;
        spec.name = field.attr_name();
        attr_id = module.AddMetadataAttr(spec.name, spec.size,
                                         metadata::Attribute::AccessMode::kWrite);
        if (attr_id < 0) {
          return CommandFailure(-attr_id, "%s: add_metadata_attr failed",
                                spec.name.c_str());
        }
      } else if (field.destination_case() == bess::pb::ResultField::kOffset) {
        spec.destination = classifier::BufferKind::kPacket;
        spec.packet_offset = field.offset();
      } else {
        return CommandFailure(EINVAL,
                              "a result field needs an attr_name or an offset");
      }
      schema_.Add(std::move(spec));
      attr_ids_.push_back(attr_id);
    }
    const size_t record_size = schema_.record_size();
    if (record_size > classifier::RecordArena::kMaxRecordSize) {
      return CommandFailure(EINVAL, "result record is %zu bytes, the maximum is %zu",
                            record_size, classifier::RecordArena::kMaxRecordSize);
    }
    if (!schema_.empty()) {
      records_ = std::make_shared<classifier::RecordArena>(
          std::max<size_t>(1, record_size), rcu);
    }
    if (!default_result.has_value()) {
      return CommandSuccess();
    }
    if (records_ == nullptr) {
      return default_result->empty()
                 ? CommandSuccess()
                 : CommandFailure(EINVAL,
                                  "default_result needs at least one result field");
    }
    if (default_result->size() > record_size) {
      return CommandFailure(EINVAL,
                            "default_result is wider than the schema's record "
                            "(%zu > %zu)",
                            default_result->size(), record_size);
    }
    default_record_id_ = records_->Allocate();
    if (default_record_id_ == 0) {
      return CommandFailure(ENOSPC, "the result arena is full");
    }
    classifier::MutableBytes slot = records_->Record(default_record_id_);
    std::memset(slot.data(), 0, record_size);
    if (!default_result->empty()) {
      std::memcpy(slot.data(), default_result->data(), default_result->size());
    }
    return CommandSuccess();
  }

  [[nodiscard]] const classifier::ResultSchema &schema() const noexcept {
    return schema_;
  }
  [[nodiscard]] bool empty() const noexcept { return schema_.empty(); }
  [[nodiscard]] size_t record_size() const noexcept {
    return schema_.record_size();
  }
  [[nodiscard]] const std::vector<std::string> &reference_names() const noexcept {
    return schema_.reference_names();
  }
  // Null with no result fields.
  [[nodiscard]] const std::shared_ptr<classifier::RecordArena> &records() const noexcept {
    return records_;
  }
  // The default result's record, 0 when none is configured.
  [[nodiscard]] uint32_t default_record_id() const noexcept {
    return default_record_id_;
  }

  // The entries a rule's result bytes name (D-172).
  [[nodiscard]] std::vector<dataplane::Reference> References(
      classifier::ConstBytes bytes) const {
    return schema_.References(bytes);
  }

  // A rule value's checks: its gate is valid and its result fits the record.
  [[nodiscard]] std::expected<void, std::string> CheckRule(
      uint64_t gate, size_t result_bytes) const {
    if (!IsValidGateValue(gate)) {
      return std::unexpected("invalid gate " + std::to_string(gate));
    }
    if (result_bytes > schema_.record_size()) {
      return std::unexpected("result is " + std::to_string(result_bytes) +
                             " bytes, the schema's record is " +
                             std::to_string(schema_.record_size()));
    }
    return {};
  }

  // PreResume has run: metadata offsets are assigned, and metadata
  // destinations compile. Until then every attribute reads offset 0, so a
  // plan would bake one offset for all of them.
  void set_metadata_ready() noexcept { metadata_ready_ = true; }

  // Field `i`'s destination under the module's current metadata offsets: its
  // packet offset, its attribute's metadata offset, classifier::kSkippedOffset
  // when nothing downstream reads the attribute, or
  // classifier::kUnresolvedOffset when it has no usable offset (out of space,
  // a reader also reached by a path with no writer, an unresolved update).
  [[nodiscard]] size_t Destination(const Module &module, size_t i) const {
    const classifier::ResultFieldSpec &spec = schema_.fields()[i];
    if (spec.destination == classifier::BufferKind::kPacket) {
      return spec.packet_offset;
    }
    const metadata::mt_offset_t offset =
        module.attr_offset(static_cast<size_t>(attr_ids_[i]));
    if (metadata::IsValidOffset(offset)) {
      return static_cast<size_t>(static_cast<uint8_t>(offset));
    }
    // No downstream reader: the metadata model lets the writer skip the write.
    return offset == metadata::kMetadataOffsetNoWrite ? classifier::kSkippedOffset
                                                       : classifier::kUnresolvedOffset;
  }

  // The placement a generation runs under the current offsets: deferred
  // (no plan, every destination unresolved) while metadata offsets are not
  // assigned yet; false, with *err set, when a field has no usable
  // destination.
  bool Compile(const Module &module, size_t key_size,
               classifier::CompiledResult *out, Error *err) const {
    *out = classifier::CompiledResult();
    out->records = records_;
    if (schema_.empty()) {
      return true;
    }
    if (!metadata_ready_ && schema_.needs_metadata()) {
      out->baked_offsets.assign(schema_.fields().size(),
                                classifier::kUnresolvedOffset);
      out->deferred = true;
      return true;
    }
    out->baked_offsets.reserve(schema_.fields().size());
    for (size_t i = 0; i < schema_.fields().size(); i++) {
      out->baked_offsets.push_back(Destination(module, i));
    }
    auto plan = schema_.Compile(out->baked_offsets, key_size);
    if (!plan) {
      *err = Error(EINVAL, plan.error().message);
      return false;
    }
    out->plan = std::move(*plan);
    return true;
  }

  // The placement of a fail-closed generation: none.
  [[nodiscard]] classifier::CompiledResult CompileFailClosed() const {
    classifier::CompiledResult out;
    out.baked_offsets.assign(schema_.fields().size(), classifier::kUnresolvedOffset);
    out.records = records_;
    return out;
  }

  // How the current destinations differ from those `result` baked: some
  // changed, and some moved (one resolved destination replaced another, as
  // when another module replaced an attribute; deferred, skipped and
  // unresolved destinations have none to move from or to).
  struct Changes {
    bool changed = false;
    bool moved = false;
  };
  [[nodiscard]] Changes Compare(const Module &module,
                                const classifier::CompiledResult &result) const {
    Changes changes;
    const auto resolved = [](size_t at) {
      return at != classifier::kUnresolvedOffset && at != classifier::kSkippedOffset;
    };
    for (size_t i = 0; i < schema_.fields().size(); i++) {
      const size_t now = Destination(module, i);
      const size_t baked = i < result.baked_offsets.size() ? result.baked_offsets[i]
                                                           : classifier::kUnresolvedOffset;
      if (baked == now) {
        continue;
      }
      changes.changed = true;
      changes.moved = changes.moved ||
                      (!result.deferred && resolved(baked) && resolved(now));
    }
    return changes;
  }

 private:
  classifier::ResultSchema schema_;
  std::vector<int> attr_ids_;  // per field; -1 for a packet destination
  std::shared_ptr<classifier::RecordArena> records_;
  uint32_t default_record_id_ = 0;
  bool metadata_ready_ = false;
};

// Publishes `next` in `published`, retires the generation it replaces against
// a fresh grace period, and reclaims what readers are already done with.
// Control thread only, so a retired table is destroyed there, never on a
// worker.
template <typename Generation>
void PublishGeneration(rcu::RcuPtr<Generation> &published,
                       std::unique_ptr<const Generation> next) {
  published.Publish(std::move(next));
  published.domain().ReclaimReady();
}

// Replaces the published generation with `build(current)`, or leaves it
// alone when the builder returns null (the builder owns reporting why).
// Control thread only, where command and graph changes are serialized.
template <typename Generation, typename Build>
bool RepublishGeneration(rcu::RcuPtr<Generation> &published, Build &&build,
                         Error *err) {
  const Generation *current = published.Read();
  if (current == nullptr) {
    *err = Error(EINVAL, "not initialized");
    return false;  // not initialized (or already deinitialized)
  }
  std::unique_ptr<const Generation> next = build(*current);
  if (next == nullptr) {
    return false;
  }
  PublishGeneration(published, std::move(next));
  return true;
}

// The PreResume rebuild: publishes `build(&err)`, or when it fails (null) the
// fail-closed generation `fail_closed()` with an error log under `label`.
// The dispatcher ignores OnEvent errors, so resume would otherwise proceed
// with a stale plan reading a reassigned metadata region.
template <typename Generation, typename Build, typename FailClosed>
void RebuildGeneration(rcu::RcuPtr<Generation> &published, std::string_view label,
                       Build &&build, FailClosed &&fail_closed) {
  Error err;
  std::unique_ptr<const Generation> next = build(&err);
  if (next == nullptr) {
    LOG(ERROR) << label << ": metadata refresh failed (" << err.second
               << "); routing all packets to the default gate";
    next = fail_closed();
  }
  PublishGeneration(published, std::move(next));
}

// Takes a table's rules resource out of `registry` at DeInit. Rules whose
// result fields reference other resources may leave with live keys while the
// graph is disconnected around them, so the order modules are destroyed in
// does not decide whether a pipeline can be torn down (D-172); rules that
// reference nothing are unregistered, which nothing can refuse (freed ids
// still in the removal cascade complete here; workers are paused).
inline void ReleaseTableResource(dataplane::ResourceRegistry &registry,
                                 const std::string &name, bool references) {
  if (references) {
    auto released = registry.ReleaseForTeardown(std::span<const std::string>(&name, 1));
    CHECK(released) << released.error();
  } else {
    auto unregistered = registry.Unregister(name);
    CHECK(unregistered) << unregistered.error();
  }
}

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_TABLE_FIELDS_H_
