// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FLOW_FLOW_KEY_H_
#define BESS_FLOW_FLOW_KEY_H_

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bess::flow {

// BESS does not say what a flow is. A key is any trivially copyable type the
// application defines: a five-tuple, a tunnel id, a subscriber, a MAC and a
// domain. Trivial copyability is what the table needs to keep keys in slot
// storage and to hand them back by copy; it says nothing about how a key is
// hashed or compared.
template <typename K>
concept FixedFlowKey = std::is_object_v<K> && !std::is_const_v<K> &&
                       !std::is_volatile_v<K> && std::is_trivially_copyable_v<K>;

// Whether two keys that compare equal always have equal bytes, so the bytes
// may be hashed and compared directly. A struct with padding does not
// qualify by itself: its padding bytes are indeterminate after
// `Key k; k.a = ...;`, and hashing them would make equal keys hash apart (the
// hidden-padding bug). The default is the compiler's own answer.
//
// An author who knows better specialises the trait and takes the duty:
// every Key object the application builds has zeroed padding (value-initialise
// it, or memset it first), or the type has explicit pad fields it always
// zeroes. A key with a mask, a don't-care field or a case-insensitive part is
// not canonical; give the table a Hash and an Equal instead.
template <typename K>
struct FlowKeyTraits {
  static constexpr bool canonical_representation =
      std::has_unique_object_representations_v<K>;
};

template <typename K>
concept ByteHashableFlowKey =
    FixedFlowKey<K> && FlowKeyTraits<K>::canonical_representation;

namespace detail {

inline constexpr uint64_t kHashMul = 0x9e3779b97f4a7c15ull;

// Hashes exactly N bytes, N a compile-time constant so the loop unrolls and
// the tail is one fixed-size load. Per eight bytes: xor in, multiply (which
// carries every lower input bit upward), fold the high half back down (which
// carries the upper input bits downward). A multiply alone leaves a
// difference in the top bytes of a word confined to the top bits of the
// result, which is exactly where a bucket index is taken from.
template <size_t N>
inline uint64_t HashFixedBytes(const void *p) noexcept {
  const auto *bytes = static_cast<const unsigned char *>(p);
  uint64_t h = kHashMul ^ N;
  size_t i = 0;
  for (; i + 8 <= N; i += 8) {
    uint64_t w;
    std::memcpy(&w, bytes + i, 8);
    h = (h ^ w) * kHashMul;
    h ^= h >> 32;
  }
  if constexpr (N % 8 != 0) {
    uint64_t w = 0;
    std::memcpy(&w, bytes + i, N % 8);
    h = (h ^ w) * kHashMul;
    h ^= h >> 32;
  }
  return h;
}

}  // namespace detail

// The default hash: the key's bytes. Only for keys whose bytes are canonical;
// anything else must bring its own hash, and says so at compile time.
template <typename K>
struct DefaultFlowHash {
  static_assert(ByteHashableFlowKey<K>,
                "this flow key has padding or is not trivially copyable, so "
                "hashing its bytes may split equal keys: specialise "
                "bess::flow::FlowKeyTraits<K> (canonical_representation = "
                "true) if every K object has zeroed padding, or pass a Hash "
                "and an Equal");
  uint64_t operator()(const K &key) const noexcept {
    return detail::HashFixedBytes<sizeof(K)>(&key);
  }
};

// The default equality: the key's bytes when canonical, otherwise the key's
// own operator==.
template <typename K>
struct DefaultFlowEqual {
  bool operator()(const K &a, const K &b) const noexcept {
    if constexpr (ByteHashableFlowKey<K>) {
      return std::memcmp(&a, &b, sizeof(K)) == 0;
    } else {
      static_assert(std::equality_comparable<K>,
                    "a non-canonical flow key needs operator== or an Equal");
      return a == b;
    }
  }
};

// The hash and equality the typed fast path inlines: stateless or copyable
// functors, non-throwing, callable on const keys.
template <typename K, typename Hash, typename Equal>
concept FlowKeyOps =
    FixedFlowKey<K> && std::copy_constructible<Hash> &&
    std::copy_constructible<Equal> &&
    requires(const Hash &hash, const Equal &equal, const K &a, const K &b) {
      { hash(a) } noexcept -> std::convertible_to<uint64_t>;
      { equal(a, b) } noexcept -> std::same_as<bool>;
    };

}  // namespace bess::flow

#endif  // BESS_FLOW_FLOW_KEY_H_
