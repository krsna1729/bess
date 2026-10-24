// SPDX-License-Identifier: BSD-3-Clause

// The key and result schema of one classifier table, resolved and compiled
// (D-189). A table's key is dense, `field0 || field1 || ...`, gathered by an
// ExtractPlan; a matched entry names a record in a RecordArena whose slices
// a ResultPlan scatters to the packet or its metadata. The caller resolves
// where each field lives (a packet offset, a metadata offset it looked up);
// this header turns those offsets into the plans a generation holds, decodes
// the references a record carries (D-172), and keeps nothing else. No Module,
// gate or protobuf: framework/table_fields.h binds it to a module.

#ifndef BESS_CLASSIFIER_TABLE_SCHEMA_H_
#define BESS_CLASSIFIER_TABLE_SCHEMA_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "classifier/classifier.h"
#include "classifier/extract_plan.h"
#include "classifier/record_arena.h"
#include "classifier/result_plan.h"
#include "classifier/runtime_schema.h"
#include "dataplane/resource.h"

namespace bess::classifier {

// An offset the current configuration cannot resolve: a metadata field or
// destination whose attribute has no usable offset.
inline constexpr size_t kUnresolvedOffset = static_cast<size_t>(-1);
// A result destination nothing downstream reads: the field is left out of the
// result plan (D-181).
inline constexpr size_t kSkippedOffset = static_cast<size_t>(-2);

// A dense key resolved against the current source offsets.
struct KeyLayout {
  size_t key_size = 0;
  std::vector<RuntimeKeyField> key_fields;
  // Per field, the source offset baked into the plan, or kUnresolvedOffset.
  std::vector<size_t> baked_offsets;
  // False when some field's source offset is unresolved.
  bool sources_resolved = true;

  // Appends the next field at the end of the key. An unresolved
  // `source_offset` marks the layout unresolved and gives the plan a
  // placeholder offset: such a plan must never run.
  void Append(SourceKind source, size_t source_offset, size_t size,
              Normalization normalization = {});
};

// What a generation holds for one key: the compiled extraction and the
// offsets it baked, which a later resolution is compared against.
struct CompiledKey {
  ExtractPlan extract;
  // The dense key width, also the extraction stride.
  size_t key_size = 0;
  // False: the extraction must not run (unresolved sources, a fail-closed
  // generation).
  bool valid = true;
  std::vector<size_t> baked_offsets;
};

// Compiles `layout` with every source range checked (BoundsPolicy::kCheck).
// The result is valid; the caller clears `valid` for a fail-closed key.
// Refuses a layout that leaves a key byte no field writes (Append never
// does): the packet path does not zero key rows before extracting them.
[[nodiscard]] ClassifierResult<CompiledKey> CompileKey(KeyLayout layout);

// One result field: `size` bytes at `value_offset` in an entry's record,
// written to the packet at `packet_offset` or to a metadata offset the caller
// resolves.
struct ResultFieldSpec {
  BufferKind destination = BufferKind::kMetadata;
  uint32_t packet_offset = 0;  // kPacket only
  uint32_t value_offset = 0;
  uint32_t size = 0;
  // Names the field in errors (its metadata attribute).
  std::string name;
  // The resource whose keys the field's bytes are (D-172), if any, and
  // whether they are `reference_count` consecutive big-endian 32-bit ids
  // (an all-zero id names nothing) rather than one key.
  std::string reference;
  bool reference_be32 = false;
  uint32_t reference_count = 1;
};

// The result fields of one table and the record they slice.
class ResultSchema {
 public:
  // The checks a field must pass on its own (zero width, overflow, reference
  // shape); the message is the configuration error.
  [[nodiscard]] static ClassifierResult<void> Check(const ResultFieldSpec &field);

  // Adds a field that passed Check. The record widens to its end.
  void Add(ResultFieldSpec field);
  void Clear();

  [[nodiscard]] const std::vector<ResultFieldSpec> &fields() const noexcept {
    return fields_;
  }
  [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }
  // The widest field's end: every record is this wide, zero-padded.
  [[nodiscard]] size_t record_size() const noexcept { return record_size_; }
  // The distinct resources the fields reference, in field order.
  [[nodiscard]] const std::vector<std::string> &reference_names() const noexcept {
    return reference_names_;
  }
  // Whether some field writes metadata (its offset is the graph's to assign).
  [[nodiscard]] bool needs_metadata() const noexcept;

  // The entries a record's bytes name (D-172), decoded from the record
  // zero-padded to record_size(), the bytes a later replacement or erase
  // reads back: an all-zero field names nothing.
  [[nodiscard]] std::vector<dataplane::Reference> References(ConstBytes bytes) const;

  // The plan that places the fields at `destinations` (one per field: a
  // packet or metadata offset, kSkippedOffset to leave the field out, or
  // kUnresolvedOffset, which fails). nullopt when every field is left out.
  [[nodiscard]] ClassifierResult<std::optional<ResultPlan>> Compile(
      std::span<const size_t> destinations, size_t key_size) const;

 private:
  std::vector<ResultFieldSpec> fields_;
  std::vector<std::string> reference_names_;
  size_t record_size_ = 0;
};

// What a generation holds for result placement.
struct CompiledResult {
  // The plan; nullopt with no fields, with every field left out, or while
  // deferred.
  std::optional<ResultPlan> plan;
  // Per field, the destination the plan baked (or a sentinel).
  std::vector<size_t> baked_offsets;
  // The fields write metadata whose offsets are not assigned yet: the plan
  // is to be compiled once they are, even if no offset changed.
  bool deferred = false;
  // The records the matched entries name; null with no fields.
  std::shared_ptr<RecordArena> records;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_TABLE_SCHEMA_H_
