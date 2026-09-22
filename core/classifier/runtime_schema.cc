// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "classifier/runtime_schema.h"

#include <limits>
#include <string>
#include <utility>

namespace bess::classifier {
namespace {

ClassifierResult<void> Error(ClassifierErrorCode code, std::string message,
                             size_t index) {
  return std::unexpected(
      ClassifierError{code, std::move(message), std::optional<size_t>(index)});
}

bool Fits(size_t offset, size_t size, size_t limit) {
  return offset <= limit && size <= limit - offset;
}

template <typename Field, typename Member>
bool Overlaps(const Field &lhs, const Field &rhs, Member member) {
  const size_t lhs_begin = lhs.*member;
  const size_t lhs_end = lhs_begin + lhs.size;
  const size_t rhs_begin = rhs.*member;
  const size_t rhs_end = rhs_begin + rhs.size;
  return lhs_begin < rhs_end && rhs_begin < lhs_end;
}

}  // namespace

ClassifierResult<void> RuntimeClassifierSchema::Validate() const {
  if (key_size == 0) {
    return std::unexpected(ClassifierError{
        ClassifierErrorCode::kEmptyKey, "classifier key size is zero", std::nullopt});
  }

  for (size_t i = 0; i < key_fields.size(); i++) {
    const RuntimeKeyField &field = key_fields[i];
    if (field.size == 0) {
      return Error(ClassifierErrorCode::kZeroFieldSize,
                   "classifier key field has zero size", i);
    }
    if (field.source != SourceKind::kPacket &&
        field.source != SourceKind::kMetadata) {
      return Error(ClassifierErrorCode::kInvalidSource,
                   "classifier key field has an invalid source", i);
    }
    if (!Fits(field.key_offset, field.size, key_size)) {
      return Error(ClassifierErrorCode::kKeyOutOfBounds,
                   "classifier key field exceeds normalized key size", i);
    }
    if (!field.normalization.mask.empty() &&
        field.normalization.mask.size() != field.size) {
      return Error(ClassifierErrorCode::kInvalidPlan,
                   "classifier key field normalization mask size mismatch", i);
    }
  }
  for (size_t i = 0; i < key_fields.size(); i++) {
    for (size_t j = i + 1; j < key_fields.size(); j++) {
      if (Overlaps(key_fields[i], key_fields[j],
                   &RuntimeKeyField::key_offset)) {
        return Error(ClassifierErrorCode::kOverlappingFields,
                     "classifier key fields overlap", j);
      }
    }
  }

  for (size_t i = 0; i < result_fields.size(); i++) {
    const RuntimeResultField &field = result_fields[i];
    if (field.size == 0) {
      return Error(ClassifierErrorCode::kZeroFieldSize,
                   "classifier result field has zero size", i);
    }
    if (!Fits(field.value_offset, field.size, value_size)) {
      return Error(ClassifierErrorCode::kResultOutOfBounds,
                   "classifier result field exceeds value size", i);
    }
    if (field.destination_offset >
        std::numeric_limits<size_t>::max() - field.size) {
      return Error(ClassifierErrorCode::kResultOutOfBounds,
                   "classifier result destination overflows", i);
    }
  }

  for (size_t i = 0; i < result_fields.size(); i++) {
    for (size_t j = i + 1; j < result_fields.size(); j++) {
      if (Overlaps(result_fields[i], result_fields[j],
                   &RuntimeResultField::destination_offset)) {
        return Error(ClassifierErrorCode::kOverlappingFields,
                     "classifier result destinations overlap", j);
      }
    }
  }

  return {};
}

}  // namespace bess::classifier
