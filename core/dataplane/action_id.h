// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_ACTION_ID_H_
#define BESS_DATAPLANE_ACTION_ID_H_

#include <cstdint>

#include "dataplane/strong_id.h"

namespace bess {
namespace dataplane {

// The id a classifier returns when the right representation for its result is
// "compact token, larger immutable object on the side" (K2).
//
// It is a *continuation token*, not a dispatch result: an output gate is a
// different semantic type (`gate_idx_t`), and a classifier that means "this
// module's output gate" must be able to compile to that directly rather than
// being forced through an id and a table. Nothing in K2 requires classification
// results to be ActionIds.
//
// 32 bits: compact, ample namespace, natural CPU representation, and it fits a
// future hardware MARK-style continuation. An id space that genuinely needs
// more can use another `StrongId` specialization.
struct ActionIdTag;
using ActionId = StrongId<ActionIdTag, uint32_t>;

// `ActionId{0}` is the invalid id: an id that refers to no object. It carries no
// packet policy -- a lookup with it returns nullptr and the caller decides
// whether that means drop, miss, fallback or slow path. K2 deliberately does
// not define a "drop" id.
inline constexpr ActionId kInvalidActionId{};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_ACTION_ID_H_
