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

#ifndef BESS_DATAPLANE_STRONG_ID_H_
#define BESS_DATAPLANE_STRONG_ID_H_

#include <compare>
#include <cstdint>

namespace bess {
namespace dataplane {

// A strongly typed integral identifier (K2).
//
// The point is that an identifier of one kind cannot be passed where another is
// meant: a gate index, an action id, a next-hop id and a raw integer are
// different things, and mixing them silently is how dispatch bugs happen. The
// type is deliberately zero-overhead -- a single `Rep` member with defaulted
// special members, so it is trivially copyable, standard layout where the
// representation allows it, and the same size as `Rep`.
//
// The zero value is reserved by convention as "invalid" (see `ActionId`): the
// table treats it as "no object" rather than assigning it meaning of its own.
//
// `Tag` is an incomplete type used only to make each id its own type; it never
// needs a definition.
template <typename Tag, typename Rep>
class StrongId {
 public:
  using rep_type = Rep;

  constexpr StrongId() = default;

  // Explicit on purpose: implicit conversion from an integer (or from another
  // id type) would defeat the whole exercise.
  explicit constexpr StrongId(Rep value) : value_(value) {}

  constexpr Rep value() const noexcept { return value_; }

  friend constexpr bool operator==(StrongId, StrongId) = default;
  friend constexpr auto operator<=>(StrongId, StrongId) = default;

 private:
  Rep value_{};
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_STRONG_ID_H_
