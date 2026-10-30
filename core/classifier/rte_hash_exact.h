// SPDX-License-Identifier: BSD-3-Clause

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

#include <rte_common.h>
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_hash.h>

#include "classifier/backend.h"
#include "classifier/classifier.h"
#include "classifier/typed_exact.h"
#include "dpdk.h"
#include "utils/common.h"
#include "utils/dpdk_memory.h"

namespace bess::classifier {

namespace detail {
inline std::string UniqueRteHashName() {
  static std::atomic<uint64_t> counter{0};
  return std::format("bess_hash_{}", counter.fetch_add(1, std::memory_order_relaxed));
}

// The bytes rte_hash_create allocates for `entries` keys of `key_len` bytes
// (DPDK 25.11 lib/hash/rte_cuckoo_hash.c), for the memory policy: the
// buckets (64 B per 8 entries, entries rounded up to a power of two), the key
// store (one slot more than entries, each the key and an 8-byte data
// pointer, 16-byte aligned), the free-slot ring (4 B per slot, a power of
// two) and the RCU defer queue (24 B per entry, a power of two; counted
// whether or not the table attaches one). An estimate: the per-lcore caches
// and extendable buckets, off by default, are not counted.
inline size_t RteHashBytes(uint32_t entries, uint32_t key_len) {
  const size_t n = entries;
  return 64 * rte_align64pow2(n) / 8 + RTE_ALIGN_CEIL(8 + key_len, 16) * (n + 1) +
         4 * rte_align64pow2(n + 1) + 24 * rte_align64pow2(n + 2);
}

// rte_hash_create under the memory policy (D-029, D-206). Under --no-huge
// the estimate is admitted against the process memory limit first. Created
// where this thread's admission placed it (utils::PlacementSocket: the
// normal-page fallback heap once the hugepage cap is spent), and a creation
// that fails for lack of memory on the memory node is retried once on the
// fallback heap, which utils::FallbackSocket grants or refuses
// (MemoryRefused, propagated to the control boundary); a retry that still
// lacks memory -- DPDK takes the tailq entries from its native heaps, never
// the fallback heap -- is refused the same way. Where it landed is recorded
// against the admission. Any other failure -- bad parameters, a name clash
// -- returns nullptr as rte_hash_create does.
inline rte_hash *CreateRteHash(rte_hash_parameters params) {
  const size_t bytes = RteHashBytes(params.entries, params.key_len);
  if (rte_eal_has_hugepages() == 0) {
    bess::utils::AdmitNormalMemory(bytes);
  }
  params.socket_id = bess::utils::PlacementSocket(params.socket_id, bytes);
  // rte_hash_create leaves rte_errno untouched when an rte_zmalloc fails, and
  // sets ENOMEM when its ring's memzone does.
  rte_errno = 0;
  rte_hash *table = rte_hash_create(&params);
  if (table == nullptr && (rte_errno == 0 || rte_errno == ENOMEM)) {
    params.socket_id = bess::utils::FallbackSocket(bytes);
    rte_errno = 0;
    table = rte_hash_create(&params);
    if (table == nullptr && (rte_errno == 0 || rte_errno == ENOMEM)) {
      bess::utils::RefuseDpdkMemory(bytes);
    }
  }
  if (table != nullptr) {
    bess::utils::NotePlacement(bytes, params.socket_id);
  }
  return table;
}
}  // namespace detail

// Runtime-keyed exact backend using DPDK rte_hash in position mode.
// Key length is chosen at construction time. Lookups return position indices
// (>= 0), which map into an external generation-owned PackedValueStore.
//
// Satisfies ScalarExactBackend<RteHashPositionBackend, ConstBytes>,
// BatchExactBackend<RteHashPositionBackend, ConstBytes, int32_t>, and
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
    params.socket_id = bess::utils::MemoryNode();
    params.extra_flag = 0;  // Clean immutable generation, no RW concurrency flags
    table_ = detail::CreateRteHash(params);
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

  // Memory (D-198): what the constructor allocates, the rte_hash for
  // max(reserve, 8) keys (detail::RteHashBytes, an estimate that also counts
  // the defer queue this table does not attach). Fixed: add_key never grows
  // it (-ENOSPC).
  [[nodiscard]] static size_t Bytes(uint32_t key_len, uint32_t reserve = 64) {
    return detail::RteHashBytes(std::max(reserve, 8u), key_len);
  }
  [[nodiscard]] size_t bytes() const noexcept {
    return table_ == nullptr
               ? 0
               : detail::RteHashBytes(
                     static_cast<uint32_t>(rte_hash_max_key_id(table_)),
                     key_len_);
  }

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
// BatchExactBackend<RteHashDataBackend<Result>, ConstBytes, Result>, and
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
    params.socket_id = bess::utils::MemoryNode();
    params.extra_flag = 0;
    table_ = detail::CreateRteHash(params);
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

  // Memory (D-198): as RteHashPositionBackend::Bytes / bytes().
  [[nodiscard]] static size_t Bytes(uint32_t key_len, uint32_t reserve = 64) {
    return detail::RteHashBytes(std::max(reserve, 8u), key_len);
  }
  [[nodiscard]] size_t bytes() const noexcept {
    return table_ == nullptr
               ? 0
               : detail::RteHashBytes(
                     static_cast<uint32_t>(rte_hash_max_key_id(table_)),
                     key_len_);
  }

  // Control path: insert key with associated data value.
  bool add_key_data(ConstBytes key, Result result) {
    if (table_ == nullptr || key.size() != key_len_) {
      return false;
    }
    // The result's bytes are the data pointer's (memcpy, not an
    // integer-to-pointer cast); lookups copy them back out.
    void *data = nullptr;
    std::memcpy(&data, &result, sizeof(Result));
    int ret = rte_hash_add_key_data(table_, key.data(), data);
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
