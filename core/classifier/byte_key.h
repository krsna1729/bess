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

#ifndef BESS_CLASSIFIER_BYTE_KEY_H_
#define BESS_CLASSIFIER_BYTE_KEY_H_

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include "classifier/classifier.h"

namespace bess::classifier {

template <size_t N>
using ByteKey = std::array<std::byte, N>;

template <size_t N>
struct ByteKeyHash {
  size_t operator()(const ByteKey<N> &key) const noexcept {
    uint64_t hash = 1469598103934665603ull;
    for (std::byte byte : key) {
      hash ^= std::to_integer<unsigned char>(byte);
      hash *= 1099511628211ull;
    }
    return static_cast<size_t>(hash);
  }
};

template <size_t N>
struct ByteKeyEqual {
  constexpr bool operator()(const ByteKey<N> &lhs,
                           const ByteKey<N> &rhs) const noexcept {
    return lhs == rhs;
  }
};

// A typed key must opt into representation-level hashing explicitly. The
// default intentionally rejects arbitrary structs whose padding may be
// indeterminate.
template <typename Key>
struct KeyTraits {
  static constexpr bool canonical_representation = false;
};

template <size_t N>
struct KeyTraits<ByteKey<N>> {
  static constexpr bool canonical_representation = true;
  using hash_type = ByteKeyHash<N>;
  using equal_type = ByteKeyEqual<N>;
};

// Use a registered equality operation when one exists; otherwise, typed
// backends fall back to the author's operator==. Hashing has no such
// fallback: arbitrary object representation is never hashed implicitly.
template <typename Key, typename = void>
struct DefaultTypedEqual {
  using type = std::equal_to<Key>;
};

template <typename Key>
struct DefaultTypedEqual<Key, std::void_t<typename KeyTraits<Key>::equal_type>> {
  using type = typename KeyTraits<Key>::equal_type;
};

template <typename Key>
using DefaultTypedEqualT = typename DefaultTypedEqual<Key>::type;

template <typename Key, typename Equal>
concept TypedKeyEquality =
    std::is_object_v<Key> && requires(const Key &lhs, const Key &rhs) {
      { std::declval<Equal>()(lhs, rhs) } -> std::same_as<bool>;
    };

template <typename Key, typename Hash, typename Equal>
concept TypedKeyOperations =
    TypedKeyEquality<Key, Equal> && requires(const Key &key) {
      { std::declval<Hash>()(key) } -> std::convertible_to<size_t>;
    };

template <typename Key>
concept CanonicalByteKey =
    std::is_trivially_copyable_v<Key> &&
    KeyTraits<Key>::canonical_representation;

template <typename Key>
concept TypedClassifierKey =
    std::is_object_v<Key> && requires(const Key &lhs, const Key &rhs) {
      typename KeyTraits<Key>::hash_type;
      typename KeyTraits<Key>::equal_type;
      { std::declval<typename KeyTraits<Key>::hash_type>()(lhs) } ->
          std::convertible_to<size_t>;
      { std::declval<typename KeyTraits<Key>::equal_type>()(lhs, rhs) } ->
          std::same_as<bool>;
    };

template <typename Key>
concept ClassifierKey = CanonicalByteKey<Key> || TypedClassifierKey<Key>;

template <typename Result>
concept InlineResult = std::is_trivially_copyable_v<Result> &&
                       std::is_copy_constructible_v<Result> &&
                       sizeof(Result) <= sizeof(uint64_t);

template <typename Result>
concept ReferencedResult = std::is_object_v<Result> && !InlineResult<Result>;

template <std::unsigned_integral T>
constexpr T NativeToBigEndian(T value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return std::byteswap(value);
  } else {
    return value;
  }
}

template <typename To, typename From>
  requires(std::is_trivially_copyable_v<To> &&
           std::is_trivially_copyable_v<From> && sizeof(To) == sizeof(From))
constexpr To BitCast(const From &value) noexcept {
  return std::bit_cast<To>(value);
}

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_BYTE_KEY_H_
