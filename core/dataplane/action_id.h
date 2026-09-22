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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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
