// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_TYPED_EXACT_H_
#define BESS_CLASSIFIER_TYPED_EXACT_H_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "classifier/backend.h"

#include "utils/common.h"

namespace bess::classifier {

namespace detail {
template <typename T>
struct is_optional : std::false_type {};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;
}  // namespace detail

// A backend that can look up one key at a time.
// lookup() returns either optional<Result> or const Result*.
template <typename B, typename Key>
concept ScalarExactBackend =
    requires(const B &b, const Key &k) {
      b.lookup(k);
      { b.size() } -> std::convertible_to<size_t>;
    } &&
    (detail::is_optional_v<std::remove_cvref_t<decltype(
         std::declval<const B &>().lookup(std::declval<const Key &>()))>> ||
     std::is_pointer_v<decltype(
         std::declval<const B &>().lookup(std::declval<const Key &>()))>);

// A backend that additionally exposes a native batch lookup returning a hit
// mask. Bit i of the return value is set iff results[i] is valid. The result
// type is supplied by the caller, so a backend does not need to declare a
// result_type alias to participate.
template <typename B, typename Key, typename Result>
concept BatchExactBackend =
    ScalarExactBackend<B, Key> &&
    requires(const B &b, std::span<const Key> keys,
             std::span<Result> results) {
      { b.lookup_batch(keys, results) } -> std::same_as<uint64_t>;
    };

// A backend that can expose capacity/storage metrics.
template <typename B>
concept MeasurableBackend = requires(const B &b) {
  { b.info() } -> std::convertible_to<BackendInfo>;
};

// ExactTable owns a typed backend and never serializes Key or Result. The
// backend's explicit operations define their semantics; ByteKey is optional,
// not an implicit requirement for natural author-defined structs.
template <typename Key, typename Result, typename Backend>
  requires(std::is_object_v<Key> && ScalarExactBackend<Backend, Key>)
class ExactTable {
 public:
  using key_type = Key;
  using result_type = Result;
  using backend_type = Backend;
  using lookup_result = decltype(std::declval<const Backend &>().lookup(
      std::declval<const Key &>()));

  explicit ExactTable(Backend backend) : backend_(std::move(backend)) {}

  [[nodiscard]] lookup_result lookup(const Key &key) const noexcept {
    return backend_.lookup(key);
  }

  // Returns a hit mask (bit i set iff results[i] is valid). Writes
  // lookup_result (std::optional<Result> or const Result*).
  [[nodiscard]] uint64_t lookup_batch(std::span<const Key> keys,
                                      std::span<lookup_result> results) const noexcept {
    promise(keys.size() == results.size());
    promise(keys.size() <= 64);
    uint64_t hits = 0;
    for (size_t i = 0; i < keys.size(); i++) {
      results[i] = backend_.lookup(keys[i]);
      if constexpr (std::is_pointer_v<lookup_result>) {
        if (results[i] != nullptr) hits |= (uint64_t{1} << i);
      } else {
        // std::optional
        if (results[i].has_value()) hits |= (uint64_t{1} << i);
      }
    }
    return hits;
  }

  // Returns a hit mask (bit i set iff results[i] is valid). Writes Result
  // directly into results[i] on hit. Uses the backend's native batch path
  // when available, otherwise falls back to scalar lookups.
  [[nodiscard]] uint64_t lookup_batch(std::span<const Key> keys,
                                      std::span<Result> results) const noexcept
    requires(!std::same_as<Result, lookup_result>) {
    promise(keys.size() == results.size());
    promise(keys.size() <= 64);
    if constexpr (BatchExactBackend<Backend, Key, Result>) {
      return backend_.lookup_batch(keys, results);
    } else {
      uint64_t hits = 0;
      for (size_t i = 0; i < keys.size(); i++) {
        lookup_result res = backend_.lookup(keys[i]);
        if constexpr (std::is_pointer_v<lookup_result>) {
          if (res != nullptr) {
            results[i] = *res;
            hits |= (uint64_t{1} << i);
          }
        } else {
          if (res.has_value()) {
            results[i] = *res;
            hits |= (uint64_t{1} << i);
          }
        }
      }
      return hits;
    }
  }

  [[nodiscard]] size_t size() const noexcept {
    return static_cast<size_t>(backend_.size());
  }

  [[nodiscard]] const Backend &backend() const noexcept { return backend_; }

 private:
  [[no_unique_address]] Backend backend_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_TYPED_EXACT_H_
