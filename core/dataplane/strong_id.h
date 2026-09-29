// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_STRONG_ID_H_
#define BESS_DATAPLANE_STRONG_ID_H_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>

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
// `StrongId` itself does not assign validity semantics to any representation
// value. Individual ID domains may reserve a value: `ActionId` reserves zero,
// while an index-like domain may use zero as its first valid object.
//
// `Tag` is an incomplete type used only to make each id its own type; it never
// needs a definition.
template <typename Tag, std::unsigned_integral Rep>
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


// Hash support is explicit so a strong id can be used in unordered containers
// without exposing an implicit conversion to its underlying representation.
template <typename Id>
struct StrongIdHash;

template <typename Tag, std::unsigned_integral Rep>
struct StrongIdHash<StrongId<Tag, Rep>> {
  using id_type = StrongId<Tag, Rep>;

  size_t operator()(id_type id) const noexcept {
    return std::hash<Rep>{}(id.value());
  }
};
}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_STRONG_ID_H_
