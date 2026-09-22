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

#pragma once
#ifndef BESS_CLASSIFIER_CUCKOO_EXACT_H_
#define BESS_CLASSIFIER_CUCKOO_EXACT_H_

#include <cstddef>
#include <optional>
#include <utility>

#include "classifier/backend.h"
#include "classifier/byte_key.h"
#include "classifier/classifier.h"
#include "classifier/typed_exact.h"
#include "utils/common.h"
#include "utils/cuckoo_map.h"

namespace bess::classifier {

// Typed exact-match backend wrapping CuckooMap<Key, Result, Hash, Equal>.
//
// Satisfies ScalarExactBackend<CuckooExactBackend, Key> and
// MeasurableBackend<CuckooExactBackend>.
//
// Does NOT satisfy BatchExactBackend — it has no native bulk lookup path.
// ExactTable<Key, Result, CuckooExactBackend<...>> falls back to the scalar
// loop, setting one hit-mask bit per non-null result.
//
// Build contract: insert()/remove() are control-plane only. Once the generation
// is published, the table is immutable; only lookup() and info() are called on
// the packet path.
template <ClassifierKey Key, typename Result,
          typename Hash = typename KeyTraits<Key>::hash_type,
          typename Equal = typename KeyTraits<Key>::equal_type>
class CuckooExactBackend {
 public:
  using key_type = Key;
  using result_type = Result;

  CuckooExactBackend() = default;

  // Move-constructible so it can be placed into ExactTable (move-only).
  CuckooExactBackend(CuckooExactBackend &&) = default;
  CuckooExactBackend &operator=(CuckooExactBackend &&) = default;
  CuckooExactBackend(const CuckooExactBackend &) = delete;
  CuckooExactBackend &operator=(const CuckooExactBackend &) = delete;

  // Packet path — O(1) amortised. Returns nullptr on miss.
  [[nodiscard]] const Result *lookup(const Key &key) const noexcept {
    const auto *entry = map_.Find(key, Hash{}, Equal{});
    return entry ? &entry->second : nullptr;
  }

  // Control path — insert or overwrite. Returns false only when CuckooMap
  // cannot find an empty slot (ENOSPC; extremely rare for small tables).
  bool insert(const Key &key, Result result) {
    return map_.Insert(key, std::move(result), Hash{}, Equal{}) != nullptr;
  }

  // Control path — returns false if the key was not present.
  bool remove(const Key &key) {
    return map_.Remove(key, Hash{}, Equal{});
  }

  [[nodiscard]] size_t size() const noexcept { return map_.Count(); }

  [[nodiscard]] BackendInfo info() const noexcept {
    return BackendInfo{
        .kind = ExactBackendKind::kCuckoo,
        .rule_count = map_.Count(),
        .key_size = sizeof(Key),
        .result_size = sizeof(Result),
    };
  }

 private:
  bess::utils::CuckooMap<Key, Result, Hash, Equal> map_;
};

template <typename Result>
struct RuntimeExactRule {
  ConstBytes key;
  Result result;
};

namespace detail {

// Internal fixed-width storage classes for adapting CuckooMap to runtime keys.
// Storage classes: 8, 16, 32, 64, 128, 256 bytes.
// Logical size is checked in equality and hashing; bytes beyond logical_size
// are ignored.
template <size_t StorageBytes>
struct RuntimeCuckooKey {
  std::array<std::byte, StorageBytes> bytes{};
  size_t logical_size = 0;

  bool operator==(const RuntimeCuckooKey &other) const noexcept {
    return logical_size == other.logical_size &&
           std::memcmp(bytes.data(), other.bytes.data(), logical_size) == 0;
  }
};

template <size_t StorageBytes>
struct RuntimeCuckooHash {
  size_t operator()(const RuntimeCuckooKey<StorageBytes> &k) const noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < k.logical_size; i++) {
      hash ^= static_cast<uint8_t>(k.bytes[i]);
      hash *= 1099511628211ull;
    }
    return static_cast<size_t>(hash);
  }
};

template <size_t StorageBytes, typename Result>
struct RuntimeCuckooState {
  using MapType = bess::utils::CuckooMap<
      RuntimeCuckooKey<StorageBytes>, Result,
      RuntimeCuckooHash<StorageBytes>>;
  MapType map;
  size_t logical_key_size = 0;
};

template <size_t StorageBytes, typename Result>
uint64_t RuntimeCuckooLookupBatch(const void *raw_state, ConstBytes keys,
                                  size_t key_stride,
                                  std::span<Result> results) noexcept {
  auto *state = static_cast<const RuntimeCuckooState<StorageBytes, Result> *>(raw_state);
  promise(key_stride >= state->logical_key_size);
  promise(keys.size() >= results.size() * key_stride);
  promise(results.size() <= 64);
  uint64_t hits = 0;
  const size_t n = results.size();
  RuntimeCuckooKey<StorageBytes> key{};
  key.logical_size = state->logical_key_size;

  for (size_t i = 0; i < n; i++) {
    std::memcpy(key.bytes.data(), keys.data() + i * key_stride,
                state->logical_key_size);
    const auto *entry = state->map.Find(key);
    if (entry != nullptr) {
      results[i] = entry->second;
      hits |= (uint64_t{1} << i);
    }
  }
  return hits;
}

template <size_t StorageBytes, typename Result>
void RuntimeCuckooDestroy(void *raw_state) noexcept {
  delete static_cast<RuntimeCuckooState<StorageBytes, Result> *>(raw_state);
}

template <size_t StorageBytes, typename Result>
ClassifierResult<RuntimeExactBackend<Result>> BuildCuckooBackendImpl(
    size_t logical_key_size,
    std::span<const RuntimeExactRule<Result>> rules) {
  auto *state = new RuntimeCuckooState<StorageBytes, Result>();
  state->logical_key_size = logical_key_size;

  RuntimeCuckooKey<StorageBytes> key{};
  key.logical_size = logical_key_size;

  for (size_t i = 0; i < rules.size(); i++) {
    const auto &rule = rules[i];
    if (rule.key.size() != logical_key_size) {
      delete state;
      return std::unexpected(ClassifierError{
          .code = ClassifierErrorCode::kInvalidPlan,
          .message = "rule key size mismatch with backend key size",
          .field_index = i,
      });
    }
    std::memcpy(key.bytes.data(), rule.key.data(), logical_key_size);
    if (state->map.Insert(key, rule.result) == nullptr) {
      delete state;
      return std::unexpected(ClassifierError{
          .code = ClassifierErrorCode::kInvalidPlan,
          .message = "cuckoo table insertion failed (capacity exceeded)",
          .field_index = i,
      });
    }
  }

  RuntimeExactOps<Result> ops{
      .lookup_batch = RuntimeCuckooLookupBatch<StorageBytes, Result>,
      .destroy = RuntimeCuckooDestroy<StorageBytes, Result>,
      .info = BackendInfo{
          .kind = ExactBackendKind::kCuckoo,
          .rule_count = rules.size(),
          .key_size = logical_key_size,
          .result_size = sizeof(Result),
          .storage_bytes = 0,
      },
  };
  return RuntimeExactBackend<Result>(ops, state);
}

}  // namespace detail

// Factory for building a populated runtime-erased CuckooMap backend adapted to an arbitrary
// runtime key width using the 8/16/32/64/128/256 internal storage class hierarchy.
template <typename Result>
ClassifierResult<RuntimeExactBackend<Result>> BuildRuntimeCuckooBackend(
    size_t logical_key_size,
    std::span<const RuntimeExactRule<Result>> rules) {
  if (logical_key_size == 0) {
    return std::unexpected(ClassifierError{
        .code = ClassifierErrorCode::kEmptyKey,
        .message = "logical key size cannot be 0",
    });
  }
  if (logical_key_size <= 8) {
    return detail::BuildCuckooBackendImpl<8, Result>(logical_key_size, rules);
  } else if (logical_key_size <= 16) {
    return detail::BuildCuckooBackendImpl<16, Result>(logical_key_size, rules);
  } else if (logical_key_size <= 32) {
    return detail::BuildCuckooBackendImpl<32, Result>(logical_key_size, rules);
  } else if (logical_key_size <= 64) {
    return detail::BuildCuckooBackendImpl<64, Result>(logical_key_size, rules);
  } else if (logical_key_size <= 128) {
    return detail::BuildCuckooBackendImpl<128, Result>(logical_key_size, rules);
  } else if (logical_key_size <= 256) {
    return detail::BuildCuckooBackendImpl<256, Result>(logical_key_size, rules);
  }
  return std::unexpected(ClassifierError{
      .code = ClassifierErrorCode::kInvalidPlan,
      .message = "logical key size exceeds maximum supported 256 bytes",
  });
}

template <typename Result>
RuntimeExactBackend<Result> MakeRuntimeCuckooBackend(size_t logical_key_size) {
  auto res = BuildRuntimeCuckooBackend<Result>(logical_key_size, {});
  return res.has_value() ? std::move(*res) : RuntimeExactBackend<Result>{};
}

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_CUCKOO_EXACT_H_
