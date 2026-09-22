// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
//   contributors may be used to endorse or promote products derived from this
//   software without specific prior written permission.
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
#ifndef BESS_CLASSIFIER_RTE_HASH_EXACT_H_
#define BESS_CLASSIFIER_RTE_HASH_EXACT_H_

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

#include <rte_config.h>
#include <rte_hash.h>

#include "classifier/backend.h"
#include "classifier/classifier.h"
#include "classifier/typed_exact.h"
#include "dpdk.h"
#include "utils/common.h"

namespace bess::classifier {

namespace detail {
inline std::string UniqueRteHashName() {
  static std::atomic<uint64_t> counter{0};
  return std::format("bess_hash_{}", counter.fetch_add(1, std::memory_order_relaxed));
}
}  // namespace detail

// Runtime-keyed exact backend using DPDK rte_hash in position mode.
// Key length is chosen at construction time. Lookups return position indices
// (>= 0), which map into an external generation-owned PackedValueStore.
//
// Satisfies ScalarExactBackend<RteHashPositionBackend, ConstBytes>,
// BatchExactBackend<RteHashPositionBackend, ConstBytes>, and
// MeasurableBackend<RteHashPositionBackend>.
//
// Concurrency: single-writer/control-plane construction; immutable concurrent
// readers on the packet path; no DPDK internal QSBR or lock-free concurrency flags.
class RteHashPositionBackend {
 public:
  using key_type = ConstBytes;
  using result_type = int32_t;

  explicit RteHashPositionBackend(uint32_t key_len, uint32_t reserve = 64)
      : key_len_(key_len) {
    if (!bess::IsDpdkInitialized()) {
      bess::InitDpdk();
    }
    const std::string name = detail::UniqueRteHashName();
    rte_hash_parameters params{};
    params.name = name.c_str();
    params.entries = std::max(reserve, 8u);
    params.key_len = key_len;
    params.hash_func = nullptr;  // Default CRC hash
    params.hash_func_init_val = 0;
    params.socket_id = SOCKET_ID_ANY;
    params.extra_flag = 0;  // Clean immutable generation, no RW concurrency flags
    table_ = rte_hash_create(&params);
  }

  // Move-only semantics.
  RteHashPositionBackend(RteHashPositionBackend &&other) noexcept
      : key_len_(other.key_len_),
        table_(std::exchange(other.table_, nullptr)),
        count_(other.count_) {}

  RteHashPositionBackend &operator=(RteHashPositionBackend &&other) noexcept {
    if (this != &other) {
      if (table_ != nullptr) {
        rte_hash_free(table_);
      }
      key_len_ = other.key_len_;
      table_ = std::exchange(other.table_, nullptr);
      count_ = other.count_;
    }
    return *this;
  }

  RteHashPositionBackend(const RteHashPositionBackend &) = delete;
  RteHashPositionBackend &operator=(const RteHashPositionBackend &) = delete;

  ~RteHashPositionBackend() {
    if (table_ != nullptr) {
      rte_hash_free(table_);
    }
  }

  [[nodiscard]] bool valid() const noexcept { return table_ != nullptr; }

  // Control path: insert key. Returns non-negative position on success,
  // or negative on error (e.g. -ENOSPC).
  int32_t add_key(ConstBytes key) {
    if (table_ == nullptr || key.size() != key_len_) {
      return -EINVAL;
    }
    int32_t pos = rte_hash_add_key(table_, key.data());
    if (pos >= 0) {
      count_ = static_cast<size_t>(rte_hash_count(table_));
    }
    return pos;
  }

  // Control path: delete key. Returns true if key was present and deleted.
  bool del_key(ConstBytes key) {
    if (table_ == nullptr || key.size() != key_len_) {
      return false;
    }
    int32_t pos = rte_hash_del_key(table_, key.data());
    if (pos >= 0) {
      count_ = static_cast<size_t>(rte_hash_count(table_));
      return true;
    }
    return false;
  }

  // Packet path: scalar lookup returning position if found.
  [[nodiscard]] std::optional<int32_t> lookup(ConstBytes key) const noexcept {
    if (table_ == nullptr || key.size() != key_len_) {
      return std::nullopt;
    }
    int32_t pos = rte_hash_lookup(table_, key.data());
    if (pos >= 0) {
      return pos;
    }
    return std::nullopt;
  }

  // Packet path: raw position lookup (negative on miss).
  [[nodiscard]] int32_t lookup_position(ConstBytes key) const noexcept {
    if (table_ == nullptr || key.size() != key_len_) {
      return -ENOENT;
    }
    return rte_hash_lookup(table_, key.data());
  }

  // Packet path: bulk lookup up to RTE_HASH_LOOKUP_BULK_MAX (64) keys.
  // Returns hit mask. results[i] receives the position for hits.
  [[nodiscard]] uint64_t lookup_batch(std::span<const ConstBytes> keys,
                                      std::span<int32_t> results) const noexcept {
    promise(keys.size() == results.size());
    promise(keys.size() <= RTE_HASH_LOOKUP_BULK_MAX);
    if (table_ == nullptr || keys.empty()) {
      return 0;
    }
    const uint32_t num_keys = static_cast<uint32_t>(keys.size());

    const void *key_ptrs[RTE_HASH_LOOKUP_BULK_MAX];
    for (uint32_t i = 0; i < num_keys; i++) {
      promise(keys[i].size() == key_len_);
      key_ptrs[i] = keys[i].data();
    }
    int32_t positions[RTE_HASH_LOOKUP_BULK_MAX];
    int ret = rte_hash_lookup_bulk(table_, key_ptrs, num_keys, positions);
    promise(ret >= 0);

    uint64_t hits = 0;
    for (uint32_t i = 0; i < num_keys; i++) {
      results[i] = positions[i];
      if (positions[i] >= 0) {
        hits |= (uint64_t{1} << i);
      }
    }
    return hits;
  }

  // Packed-key bulk lookup where keys are stored contiguously at key_stride bytes.
  // Structurally guarantees key storage without per-key span metadata.
  [[nodiscard]] uint64_t lookup_batch_packed(ConstBytes keys, size_t key_stride,
                                             std::span<int32_t> results) const noexcept {
    promise(key_stride >= key_len_);
    promise(results.empty() ||
            key_stride <= keys.size() / results.size());
    promise(results.size() <= RTE_HASH_LOOKUP_BULK_MAX);
    if (table_ == nullptr || results.empty()) {
      return 0;
    }
    const uint32_t num_keys = static_cast<uint32_t>(results.size());
    const void *key_ptrs[RTE_HASH_LOOKUP_BULK_MAX];
    for (uint32_t i = 0; i < num_keys; i++) {
      key_ptrs[i] = keys.data() + i * key_stride;
    }
    int32_t positions[RTE_HASH_LOOKUP_BULK_MAX];
    int ret = rte_hash_lookup_bulk(table_, key_ptrs, num_keys, positions);
    promise(ret >= 0);

    uint64_t hits = 0;
    for (uint32_t i = 0; i < num_keys; i++) {
      results[i] = positions[i];
      if (positions[i] >= 0) {
        hits |= (uint64_t{1} << i);
      }
    }
    return hits;
  }

  [[nodiscard]] size_t size() const noexcept { return count_; }

  [[nodiscard]] BackendInfo info() const noexcept {
    return BackendInfo{
        .kind = ExactBackendKind::kRteHash,
        .rule_count = count_,
        .key_size = key_len_,
        .result_size = sizeof(int32_t),
    };
  }

 private:
  uint32_t key_len_ = 0;
  rte_hash *table_ = nullptr;
  size_t count_ = 0;
};

// Runtime-keyed exact backend storing Result values directly in the hash
// table using DPDK's 8-byte associated data pointer.
// Suitable for scalar results like gate_idx_t, ActionId, or uint32_t.
//
// Satisfies ScalarExactBackend<RteHashDataBackend<Result>, ConstBytes>,
// BatchExactBackend<RteHashDataBackend<Result>, ConstBytes>, and
// MeasurableBackend<RteHashDataBackend<Result>>.
template <typename Result>
  requires(std::is_trivially_copyable_v<Result> && sizeof(Result) <= sizeof(uintptr_t))
class RteHashDataBackend {
 public:
  using key_type = ConstBytes;
  using result_type = Result;

  explicit RteHashDataBackend(uint32_t key_len, uint32_t reserve = 64)
      : key_len_(key_len) {
    if (!bess::IsDpdkInitialized()) {
      bess::InitDpdk();
    }
    const std::string name = detail::UniqueRteHashName();
    rte_hash_parameters params{};
    params.name = name.c_str();
    params.entries = std::max(reserve, 8u);
    params.key_len = key_len;
    params.hash_func = nullptr;
    params.hash_func_init_val = 0;
    params.socket_id = SOCKET_ID_ANY;
    params.extra_flag = 0;
    table_ = rte_hash_create(&params);
  }

  // Move-only semantics.
  RteHashDataBackend(RteHashDataBackend &&other) noexcept
      : key_len_(other.key_len_),
        table_(std::exchange(other.table_, nullptr)),
        count_(other.count_) {}

  RteHashDataBackend &operator=(RteHashDataBackend &&other) noexcept {
    if (this != &other) {
      if (table_ != nullptr) {
        rte_hash_free(table_);
      }
      key_len_ = other.key_len_;
      table_ = std::exchange(other.table_, nullptr);
      count_ = other.count_;
    }
    return *this;
  }

  RteHashDataBackend(const RteHashDataBackend &) = delete;
  RteHashDataBackend &operator=(const RteHashDataBackend &) = delete;

  ~RteHashDataBackend() {
    if (table_ != nullptr) {
      rte_hash_free(table_);
    }
  }

  [[nodiscard]] bool valid() const noexcept { return table_ != nullptr; }

  // Control path: insert key with associated data value.
  bool add_key_data(ConstBytes key, Result result) {
    if (table_ == nullptr || key.size() != key_len_) {
      return false;
    }
    uintptr_t encoded = 0;
    std::memcpy(&encoded, &result, sizeof(Result));
    int ret = rte_hash_add_key_data(table_, key.data(),
                                    reinterpret_cast<void *>(encoded));
    if (ret == 0) {
      count_ = static_cast<size_t>(rte_hash_count(table_));
      return true;
    }
    return false;
  }

  // Control path: delete key.
  bool del_key(ConstBytes key) {
    if (table_ == nullptr || key.size() != key_len_) {
      return false;
    }
    int32_t pos = rte_hash_del_key(table_, key.data());
    if (pos >= 0) {
      count_ = static_cast<size_t>(rte_hash_count(table_));
      return true;
    }
    return false;
  }

  // Packet path: scalar lookup.
  [[nodiscard]] std::optional<Result> lookup(ConstBytes key) const noexcept {
    if (table_ == nullptr || key.size() != key_len_) {
      return std::nullopt;
    }
    void *data = nullptr;
    int pos = rte_hash_lookup_data(table_, key.data(), &data);
    if (pos >= 0) {
      Result res{};
      uintptr_t encoded = reinterpret_cast<uintptr_t>(data);
      std::memcpy(&res, &encoded, sizeof(Result));
      return res;
    }
    return std::nullopt;
  }

  // Packet path: native bulk lookup using DPDK's rte_hash_lookup_bulk_data!
  // Returns hit mask natively, copying out results for set bits.
  [[nodiscard]] uint64_t lookup_batch(std::span<const ConstBytes> keys,
                                      std::span<Result> results) const noexcept {
    promise(keys.size() == results.size());
    promise(keys.size() <= RTE_HASH_LOOKUP_BULK_MAX);
    if (table_ == nullptr || keys.empty()) {
      return 0;
    }
    const uint32_t num_keys = static_cast<uint32_t>(keys.size());

    const void *key_ptrs[RTE_HASH_LOOKUP_BULK_MAX];
    void *data_ptrs[RTE_HASH_LOOKUP_BULK_MAX];
    for (uint32_t i = 0; i < num_keys; i++) {
      promise(keys[i].size() == key_len_);
      key_ptrs[i] = keys[i].data();
    }
    uint64_t hit_mask = 0;
    int ret = rte_hash_lookup_bulk_data(table_, key_ptrs, num_keys, &hit_mask, data_ptrs);
    promise(ret >= 0);

    for (uint32_t i = 0; i < num_keys; i++) {
      if (hit_mask & (uint64_t{1} << i)) {
        uintptr_t encoded = reinterpret_cast<uintptr_t>(data_ptrs[i]);
        std::memcpy(&results[i], &encoded, sizeof(Result));
      }
    }
    return hit_mask;
  }

  // Packed-key bulk lookup
  [[nodiscard]] uint64_t lookup_batch_packed(ConstBytes keys, size_t key_stride,
                                             std::span<Result> results) const noexcept {
    promise(key_stride >= key_len_);
    promise(results.empty() ||
            key_stride <= keys.size() / results.size());
    promise(results.size() <= RTE_HASH_LOOKUP_BULK_MAX);
    if (table_ == nullptr || results.empty()) {
      return 0;
    }
    const uint32_t num_keys = static_cast<uint32_t>(results.size());
    const void *key_ptrs[RTE_HASH_LOOKUP_BULK_MAX];
    void *data_ptrs[RTE_HASH_LOOKUP_BULK_MAX];
    for (uint32_t i = 0; i < num_keys; i++) {
      key_ptrs[i] = keys.data() + i * key_stride;
    }
    uint64_t hit_mask = 0;
    int ret = rte_hash_lookup_bulk_data(table_, key_ptrs, num_keys, &hit_mask, data_ptrs);
    promise(ret >= 0);

    for (uint32_t i = 0; i < num_keys; i++) {
      if (hit_mask & (uint64_t{1} << i)) {
        uintptr_t encoded = reinterpret_cast<uintptr_t>(data_ptrs[i]);
        std::memcpy(&results[i], &encoded, sizeof(Result));
      }
    }
    return hit_mask;
  }

  [[nodiscard]] size_t size() const noexcept { return count_; }

  [[nodiscard]] BackendInfo info() const noexcept {
    return BackendInfo{
        .kind = ExactBackendKind::kRteHash,
        .rule_count = count_,
        .key_size = key_len_,
        .result_size = sizeof(Result),
    };
  }

 private:
  uint32_t key_len_ = 0;
  rte_hash *table_ = nullptr;
  size_t count_ = 0;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RTE_HASH_EXACT_H_
