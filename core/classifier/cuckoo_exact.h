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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

#include <rte_hash_crc.h>

#include "classifier/backend.h"
#include "classifier/byte_key.h"
#include "classifier/classifier.h"
#include "classifier/typed_exact.h"
#include "utils/common.h"
#include "dataplane/batch_stages.h"
#include "utils/cuckoo_map.h"

namespace bess::classifier {

// Typed exact-match backend wrapping CuckooMap<Key, Result, Hash, Equal>.
//
// Satisfies ScalarExactBackend<CuckooExactBackend, Key> and
// MeasurableBackend<CuckooExactBackend>.
//
// Does NOT satisfy BatchExactBackend — it has no native bulk lookup path.
// ExactTable<Key, Result, CuckooExactBackend<...>> falls back to the scalar
// loop, setting one hit-mask bit per non-null result. A typed author who wants
// a batch entry point at all can supply one; the contract stays scalar-first.
//
// The key need not be a ByteKey or a KeyTraits specialization when the author
// supplies explicit Hash and Equal operations. No object-representation hash
// is inferred.
template <typename Key, typename Result,
          typename Hash = typename KeyTraits<Key>::hash_type,
          typename Equal = DefaultTypedEqualT<Key>>
  requires TypedKeyOperations<Key, Hash, Equal>
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
// Storage classes: 8, 16, 32, 64, 128, 256 bytes. The logical key length
// lives in the state (and in the stateful hash/equality functors below), not
// in every stored entry: an 8-byte logical key occupies an 8-byte entry.
template <size_t StorageBytes>
struct alignas(8) RuntimeCuckooKey {
  // The tail is intentionally not initialized: stateful hash/equality only
  // inspect the leading logical bytes. Builders use value-initialization when
  // they need deterministic stored padding.
  std::array<std::byte, StorageBytes> bytes;
};

// Stateful hash over the leading logical_size bytes. CRC32C (portable
// DPDK-backed; hardware CRC on x86) measured ~6.5x faster than the K3.3
// byte-at-a-time FNV-1a on identical 8-byte keys (see modules/
// exact_match_bench.cc BM_HashFNV_Key vs BM_HashCRC_Key). Always pass an
// instance carrying the table's logical size to direct stored-key
// Insert()/Find(): the default-constructed functor hashes zero bytes and must
// never be used.
template <size_t StorageBytes>
struct RuntimeCuckooHash {
  size_t logical_size = 0;

  size_t operator()(const RuntimeCuckooKey<StorageBytes> &k) const noexcept {
    return static_cast<size_t>(rte_hash_crc(
        k.bytes.data(), static_cast<uint32_t>(logical_size), 0));
  }
};

// Stateful equality over the leading logical_size bytes. Same caveat as the
// hash: always pass the instance carrying the table's logical size.
template <size_t StorageBytes>
struct RuntimeCuckooEqual {
  size_t logical_size = 0;

  bool operator()(const RuntimeCuckooKey<StorageBytes> &a,
                  const RuntimeCuckooKey<StorageBytes> &b) const noexcept {
    if (logical_size == sizeof(uint64_t)) {
      uint64_t lhs;
      uint64_t rhs;
      std::memcpy(&lhs, a.bytes.data(), sizeof(lhs));
      std::memcpy(&rhs, b.bytes.data(), sizeof(rhs));
      return lhs == rhs;
    }
    return std::memcmp(a.bytes.data(), b.bytes.data(), logical_size) == 0;
  }
};

// Borrowed lookup probe into the packed runtime key buffer. The probe is
// short-lived and never owns or copies key bytes.
struct RuntimeCuckooProbe {
  const std::byte* data;
  size_t size;
};

// Generic probe hash for logical widths without a construction-time fixed
// kernel.
struct RuntimeCuckooProbeHash {
  size_t operator()(const RuntimeCuckooProbe& probe) const noexcept {
    return static_cast<size_t>(
        rte_hash_crc(probe.data, static_cast<uint32_t>(probe.size), 0));
  }
};

// Construction-time fixed-width probe hash. The 1/2/4/8-byte kernels use
// DPDK's scalar CRC entry points; wider keys use the byte-range entry point.
template <size_t LogicalBytes>
struct RuntimeCuckooFixedProbeHash {
  size_t operator()(const RuntimeCuckooProbe& probe) const noexcept {
    if constexpr (LogicalBytes == 1) {
      return rte_hash_crc_1byte(std::to_integer<uint8_t>(probe.data[0]), 0);
    } else if constexpr (LogicalBytes == 2) {
      uint16_t value;
      std::memcpy(&value, probe.data, sizeof(value));
      return rte_hash_crc_2byte(value, 0);
    } else if constexpr (LogicalBytes == 4) {
      uint32_t value;
      std::memcpy(&value, probe.data, sizeof(value));
      return rte_hash_crc_4byte(value, 0);
    } else if constexpr (LogicalBytes == 8) {
      uint64_t value;
      std::memcpy(&value, probe.data, sizeof(value));
      return rte_hash_crc_8byte(value, 0);
    } else {
      return static_cast<size_t>(
          rte_hash_crc(probe.data, LogicalBytes, 0));
    }
  }
};

template <size_t StorageBytes>
struct RuntimeCuckooProbeEqual {
  bool operator()(const RuntimeCuckooKey<StorageBytes> &stored,
                  const RuntimeCuckooProbe &probe) const noexcept {
    return std::memcmp(stored.bytes.data(), probe.data, probe.size) == 0;
  }
};

template <size_t StorageBytes, size_t LogicalBytes>
struct RuntimeCuckooFixedProbeEqual {
  static_assert(LogicalBytes <= StorageBytes);

  bool operator()(const RuntimeCuckooKey<StorageBytes> &stored,
                  const RuntimeCuckooProbe &probe) const noexcept {
    if constexpr (LogicalBytes == 1) {
      return stored.bytes[0] == probe.data[0];
    } else if constexpr (LogicalBytes == 2) {
      uint16_t lhs;
      uint16_t rhs;
      std::memcpy(&lhs, stored.bytes.data(), sizeof(lhs));
      std::memcpy(&rhs, probe.data, sizeof(rhs));
      return lhs == rhs;
    } else if constexpr (LogicalBytes == 4) {
      uint32_t lhs;
      uint32_t rhs;
      std::memcpy(&lhs, stored.bytes.data(), sizeof(lhs));
      std::memcpy(&rhs, probe.data, sizeof(rhs));
      return lhs == rhs;
    } else if constexpr (LogicalBytes == 8) {
      uint64_t lhs;
      uint64_t rhs;
      std::memcpy(&lhs, stored.bytes.data(), sizeof(lhs));
      std::memcpy(&rhs, probe.data, sizeof(rhs));
      return lhs == rhs;
    } else {
      return std::memcmp(stored.bytes.data(), probe.data, LogicalBytes) == 0;
    }
  }
};

template <size_t StorageBytes, typename Result>
struct RuntimeCuckooState {
  using MapType = bess::utils::CuckooMap<
      RuntimeCuckooKey<StorageBytes>, Result,
      RuntimeCuckooHash<StorageBytes>, RuntimeCuckooEqual<StorageBytes>>;
  MapType map;
  size_t logical_key_size = 0;
};

// Probe directly from the packed key buffer. The hot loop computes the raw
// hash once and passes the borrowed probe plus hash into CuckooMap.
template <size_t StorageBytes, typename Result, typename ProbeHash,
          typename ProbeEqual>
uint64_t RuntimeCuckooLookupBatchPrehashedImpl(
    const void *raw_state, ConstBytes keys, size_t key_stride,
    std::span<Result> results, size_t logical_key_size,
    const ProbeHash &hash, const ProbeEqual &equal) noexcept {
  auto *state =
      static_cast<const RuntimeCuckooState<StorageBytes, Result> *>(raw_state);
  promise(key_stride >= logical_key_size);
  promise(results.empty() ||
          key_stride <= keys.size() / results.size());
  uint64_t hits = 0;
  const size_t n = results.size();
  const auto probe_at = [&](size_t i) {
    return RuntimeCuckooProbe{
        .data = keys.data() + i * key_stride,
        .size = logical_key_size,
    };
  };

  // Stage-major (K4.6): hash every key and prefetch its primary bucket, then
  // probe. The bucket -> entry misses of a batch overlap instead of being paid
  // one key at a time: 20% faster on a cache-resident table, 28-46% beyond L3
  // (classifier/cuckoo_scale_bench.cc).
  constexpr size_t kChunk = 32;
  bess::utils::HashResult hashes[kChunk];
  for (size_t base = 0; base < n; base += kChunk) {
    const size_t m = n - base < kChunk ? n - base : kChunk;
    bess::dataplane::RunStages(
        m,
        [&](size_t i) {
          hashes[i] = static_cast<bess::utils::HashResult>(
              hash(probe_at(base + i)));
          state->map.PrefetchBucketPrehashed(hashes[i]);
        },
        [&](size_t i) {
          const auto *entry =
              state->map.FindPrehashedAs(hashes[i], probe_at(base + i), equal);
          if (entry != nullptr) {
            results[base + i] = entry->second;
            hits |= (uint64_t{1} << (base + i));
          }
        });
  }
  return hits;
}

template <size_t StorageBytes, size_t LogicalBytes, typename Result>
uint64_t RuntimeCuckooLookupBatchFixed(
    const void *raw_state, ConstBytes keys, size_t key_stride,
    std::span<Result> results) noexcept {
  auto *state =
      static_cast<const RuntimeCuckooState<StorageBytes, Result> *>(raw_state);
  promise(state->logical_key_size == LogicalBytes);
  return RuntimeCuckooLookupBatchPrehashedImpl<StorageBytes, Result>(
      raw_state, keys, key_stride, results, LogicalBytes,
      RuntimeCuckooFixedProbeHash<LogicalBytes>{},
      RuntimeCuckooFixedProbeEqual<StorageBytes, LogicalBytes>{});
}

template <size_t StorageBytes, typename Result>
uint64_t RuntimeCuckooLookupBatchVariable(
    const void *raw_state, ConstBytes keys, size_t key_stride,
    std::span<Result> results) noexcept {
  auto *state =
      static_cast<const RuntimeCuckooState<StorageBytes, Result> *>(raw_state);
  return RuntimeCuckooLookupBatchPrehashedImpl<StorageBytes, Result>(
      raw_state, keys, key_stride, results, state->logical_key_size,
      RuntimeCuckooProbeHash{}, RuntimeCuckooProbeEqual<StorageBytes>{});
}

template <size_t StorageBytes, typename Result>
uint64_t RuntimeCuckooLookupBatch(
    const void *raw_state, ConstBytes keys, size_t key_stride,
    std::span<Result> results) noexcept {
  return RuntimeCuckooLookupBatchVariable<StorageBytes, Result>(
      raw_state, keys, key_stride, results);
}

// Bind the exact-width hash/equality kernels once at construction time.
template <size_t StorageBytes, typename Result>
LookupBatchFn<Result> SelectRuntimeCuckooLookup(size_t logical_key_size) {
  if (logical_key_size == 1) {
    return RuntimeCuckooLookupBatchFixed<StorageBytes, 1, Result>;
  }
  if (logical_key_size == 2) {
    return RuntimeCuckooLookupBatchFixed<StorageBytes, 2, Result>;
  }
  if (logical_key_size == 4) {
    return RuntimeCuckooLookupBatchFixed<StorageBytes, 4, Result>;
  }
  if (logical_key_size == 8) {
    return RuntimeCuckooLookupBatchFixed<StorageBytes, 8, Result>;
  }
  if constexpr (StorageBytes >= 16) {
    if (logical_key_size == 16) {
      return RuntimeCuckooLookupBatchFixed<StorageBytes, 16, Result>;
    }
  }
  return RuntimeCuckooLookupBatchVariable<StorageBytes, Result>;
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
  const RuntimeCuckooHash<StorageBytes> hash{logical_key_size};
  const RuntimeCuckooEqual<StorageBytes> equal{logical_key_size};

  RuntimeCuckooKey<StorageBytes> key{};

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
    const size_t before = state->map.Count();
    if (state->map.Insert(key, rule.result, hash, equal) == nullptr) {
      delete state;
      return std::unexpected(ClassifierError{
          .code = ClassifierErrorCode::kInvalidPlan,
          .message = "cuckoo table insertion failed (capacity exceeded)",
          .field_index = i,
      });
    }
    if (state->map.Count() == before) {
      // CuckooMap::Insert overwrites an existing key in place, so an
      // unchanged count means this rule duplicates an earlier key. Report
      // it: the caller must canonicalize (last-wins, first-wins, or reject)
      // instead of silently depending on overwrite order.
      delete state;
      return std::unexpected(ClassifierError{
          .code = ClassifierErrorCode::kInvalidPlan,
          .message = "duplicate rule key",
          .field_index = i,
      });
    }
  }

  const size_t rule_count = state->map.Count();
  RuntimeExactOps<Result> ops{
      .lookup_batch =
          SelectRuntimeCuckooLookup<StorageBytes, Result>(logical_key_size),
      .destroy = RuntimeCuckooDestroy<StorageBytes, Result>,
      .info = BackendInfo{
          .kind = ExactBackendKind::kCuckoo,
          .rule_count = rule_count,
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
