// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/table_schema.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace bess::classifier {

void KeyLayout::Append(SourceKind source, size_t source_offset, size_t size,
                       Normalization normalization) {
  RuntimeKeyField field;
  field.source = source;
  field.key_offset = key_size;
  field.size = size;
  if (source_offset == kUnresolvedOffset) {
    sources_resolved = false;
    field.source_offset = 0;  // placeholder; the plan never runs
  } else {
    field.source_offset = source_offset;
  }
  field.normalization = std::move(normalization);
  key_fields.push_back(std::move(field));
  baked_offsets.push_back(source_offset);
  key_size += size;
}

ClassifierResult<CompiledKey> CompileKey(KeyLayout layout) {
  RuntimeClassifierSchema schema;
  schema.key_size = layout.key_size;
  schema.bounds = BoundsPolicy::kCheck;
  schema.key_fields = std::move(layout.key_fields);
  auto plan = ExtractPlan::Compile(schema);
  if (!plan) {
    return std::unexpected(std::move(plan.error()));
  }
  // The packet path writes key rows without zeroing them first
  // (framework::ExtractKeys): every key byte must be some field's. Append
  // lays fields end to end; a hand-built layout may not.
  if (!plan->fully_covers_key()) {
    return std::unexpected(
        ClassifierError{.code = ClassifierErrorCode::kInvalidPlan,
                        .message = "the key layout leaves bytes no field writes"});
  }
  return CompiledKey{.extract = std::move(*plan),
                     .key_size = layout.key_size,
                     .valid = true,
                     .baked_offsets = std::move(layout.baked_offsets)};
}

ClassifierResult<void> ResultSchema::Check(const ResultFieldSpec &field) {
  const auto invalid = [](const char *message) {
    return std::unexpected(ClassifierError{
        .code = ClassifierErrorCode::kInvalidDestination, .message = message});
  };
  if (field.size == 0) {
    return invalid("a result field has zero bytes");
  }
  if (field.value_offset > std::numeric_limits<uint32_t>::max() - field.size) {
    return invalid("a result field overflows its record");
  }
  if (field.reference_be32) {
    if (field.reference.empty()) {
      return invalid("a BE32 reference names no resource");
    }
    if (static_cast<uint64_t>(field.size) != 4ULL * field.reference_count) {
      return invalid(
          "a BE32 reference carries reference_count 4-byte ids: num_bytes "
          "must be 4 * reference_count");
    }
  } else if (field.reference_count != 1) {
    return invalid("reference_count applies to BE32 references only");
  }
  return {};
}

void ResultSchema::Add(ResultFieldSpec field) {
  if (!field.reference.empty() &&
      std::ranges::find(reference_names_, field.reference) ==
          reference_names_.end()) {
    reference_names_.push_back(field.reference);
  }
  record_size_ = std::max(record_size_,
                          static_cast<size_t>(field.value_offset) + field.size);
  fields_.push_back(std::move(field));
}

void ResultSchema::Clear() {
  fields_.clear();
  reference_names_.clear();
  record_size_ = 0;
}

bool ResultSchema::needs_metadata() const noexcept {
  return std::ranges::any_of(fields_, [](const ResultFieldSpec &field) {
    return field.destination == BufferKind::kMetadata;
  });
}

std::vector<dataplane::Reference> ResultSchema::References(ConstBytes bytes) const {
  std::vector<dataplane::Reference> refs;
  std::vector<std::byte> record(record_size_, std::byte{0});
  if (!bytes.empty()) {
    std::memcpy(record.data(), bytes.data(), std::min(bytes.size(), record.size()));
  }
  for (const ResultFieldSpec &field : fields_) {
    if (field.reference.empty() ||
        field.value_offset + field.size > record.size()) {
      continue;
    }
    const std::byte *at = record.data() + field.value_offset;
    if (std::all_of(at, at + field.size,
                    [](std::byte b) { return b == std::byte{0}; })) {
      continue;
    }
    if (!field.reference_be32) {
      refs.push_back({field.reference,
                      std::string(reinterpret_cast<const char *>(at), field.size)});
      continue;
    }
    for (uint32_t i = 0; i < field.reference_count; i++) {
      const std::byte *chunk = at + (static_cast<size_t>(i) * 4);
      const uint32_t id = (static_cast<uint32_t>(chunk[0]) << 24U) |
                          (static_cast<uint32_t>(chunk[1]) << 16U) |
                          (static_cast<uint32_t>(chunk[2]) << 8U) |
                          static_cast<uint32_t>(chunk[3]);
      if (id == 0) {
        continue;  // 0 names nothing at this position (D-141)
      }
      refs.push_back({field.reference, dataplane::EncodeKey(id)});
    }
  }
  return refs;
}

ClassifierResult<std::optional<ResultPlan>> ResultSchema::Compile(
    std::span<const size_t> destinations, size_t key_size) const {
  RuntimeClassifierSchema schema;
  schema.key_size = key_size;
  schema.value_size = record_size_;
  schema.bounds = BoundsPolicy::kCheck;
  for (size_t i = 0; i < fields_.size(); i++) {
    const ResultFieldSpec &spec = fields_[i];
    const size_t at = i < destinations.size() ? destinations[i] : kUnresolvedOffset;
    if (at == kSkippedOffset) {
      continue;  // compiled out: nothing downstream reads it
    }
    if (at == kUnresolvedOffset) {
      return std::unexpected(ClassifierError{
          .code = ClassifierErrorCode::kInvalidDestination,
          .message = "result field '" + spec.name +
                     "' has no readable metadata offset",
          .field_index = i});
    }
    RuntimeResultField field;
    field.value_offset = spec.value_offset;
    field.size = spec.size;
    field.destination = spec.destination;
    field.destination_offset = at;
    schema.result_fields.push_back(std::move(field));
  }
  if (schema.result_fields.empty()) {
    return std::optional<ResultPlan>();  // every field compiled out
  }
  auto compiled = ResultPlan::Compile(schema);
  if (!compiled) {
    ClassifierError error = std::move(compiled.error());
    error.message = "result plan: " + error.message;
    return std::unexpected(std::move(error));
  }
  return std::optional<ResultPlan>(std::move(*compiled));
}

}  // namespace bess::classifier
