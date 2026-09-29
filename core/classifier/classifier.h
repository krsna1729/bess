// SPDX-License-Identifier: BSD-3-Clause

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
#include "dataplane/batch_tuning.h"
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
  // The batch-lookup body chosen at build (K4.6); kAuto when not applicable.
  dataplane::LookupBody lookup_body = dataplane::LookupBody::kAuto;
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
