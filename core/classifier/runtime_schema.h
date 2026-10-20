// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_RUNTIME_SCHEMA_H_
#define BESS_CLASSIFIER_RUNTIME_SCHEMA_H_

#include <cstddef>
#include <vector>

#include "classifier/classifier.h"

namespace bess::classifier {

// Per-field normalization applied after extraction. An empty mask means
// plain exact matching (all-ones). When non-empty, mask.size() must equal
// the field size and is ANDed byte-by-byte into the key output region.
struct Normalization {
  std::vector<std::byte> mask;  // empty == all-ones; non-empty must match size
};

struct RuntimeKeyField {
  SourceKind source = SourceKind::kPacket;
  size_t source_offset = 0;
  size_t key_offset = 0;
  size_t size = 0;
  Normalization normalization;
};

// Where one output goes: the packet's bytes, or its metadata block. The
// mirror of RuntimeKeyField: it has a source, a mask and a size; this has a
// destination, a mask and a size. An empty mask writes the value as it is; a
// non-empty one writes value & mask, as the key side normalizes.
struct RuntimeResultField {
  BufferKind destination = BufferKind::kMetadata;
  size_t value_offset = 0;
  size_t destination_offset = 0;
  size_t size = 0;
  Normalization normalization;
};

// Resolved control-plane input. Metadata names and protobuf objects must be
// removed before constructing this value.
struct RuntimeClassifierSchema {
  size_t key_size = 0;
  size_t value_size = 0;
  BoundsPolicy bounds = BoundsPolicy::kCheck;
  std::vector<RuntimeKeyField> key_fields;
  std::vector<RuntimeResultField> result_fields;

  [[nodiscard]] ClassifierResult<void> Validate() const;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RUNTIME_SCHEMA_H_
