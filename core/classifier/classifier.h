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

#ifndef BESS_CLASSIFIER_CLASSIFIER_H_
#define BESS_CLASSIFIER_CLASSIFIER_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

#include "dataplane/action_id.h"
#include "dataplane/strong_id.h"

namespace bess::classifier {

using Byte = std::byte;
using ConstBytes = std::span<const Byte>;
using MutableBytes = std::span<Byte>;

enum class ClassifierErrorCode : uint8_t {
  kEmptyKey,
  kZeroFieldSize,
  kKeyOutOfBounds,
  kResultOutOfBounds,
  kOverlappingFields,
  kInvalidSource,
  kInvalidPlan,
};

struct ClassifierError {
  ClassifierErrorCode code = ClassifierErrorCode::kInvalidPlan;
  std::string message;
  std::optional<size_t> field_index;
};

template <typename T>
using ClassifierResult = std::expected<T, ClassifierError>;

enum class SourceKind : uint8_t {
  kPacket,
  kMetadata,
};

enum class BoundsPolicy : uint8_t {
  // The caller guarantees every configured source range is available.
  kAssumeAvailable,
  // The plan checks source spans before every copy.
  kCheck,
};

enum class ResultMode : uint8_t {
  kGate,
  kInlineBytes,
  kSlot,
};

enum class ExactBackendKind : uint8_t {
  kAuto,
  kSmall,
  kDirect,
  kCuckoo,
  kRteHash,
};

enum class WildcardBackendKind : uint8_t {
  kAuto,
  kTupleSpace,
  kRteAcl,
};

struct ResultSlotTag;
using ResultSlot = dataplane::StrongId<ResultSlotTag, uint32_t>;

static_assert(!std::is_same_v<ResultSlot, dataplane::ActionId>);
static_assert(!std::is_convertible_v<ResultSlot, dataplane::ActionId>);
static_assert(!std::is_convertible_v<dataplane::ActionId, ResultSlot>);

struct BackendInfo {
  ExactBackendKind kind = ExactBackendKind::kAuto;
  size_t rule_count = 0;
  size_t key_size = 0;
  size_t result_size = 0;
  size_t storage_bytes = 0;
};

// Masked/ternary classification is a different problem shape from exact
// matching, so its metrics are not reported through BackendInfo.
struct MaskedBackendInfo {
  WildcardBackendKind kind = WildcardBackendKind::kTupleSpace;
  size_t tuple_count = 0;
  size_t rule_count = 0;
  size_t key_size = 0;
  size_t result_size = 0;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_CLASSIFIER_H_
