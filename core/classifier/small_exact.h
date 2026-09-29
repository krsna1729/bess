// SPDX-License-Identifier: BSD-3-Clause

#pragma once
#ifndef BESS_CLASSIFIER_SMALL_EXACT_H_
#define BESS_CLASSIFIER_SMALL_EXACT_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "classifier/backend.h"
#include "classifier/classifier.h"
#include "utils/common.h"
namespace bess::classifier {

// Immutable contiguous linear-scan exact backend.
// Satisfies ScalarExactBackend and MeasurableBackend. lookup_batch scans with
// the same equality and returns a hit mask, so it also satisfies
// BatchExactBackend — but the work is the scalar scan, not a vectorized path.
// Intended for tiny rule counts (≤64); no SIMD, no hashing.
template <typename Key, typename Result,
          typename Equal = DefaultTypedEqualT<Key>>
  requires TypedKeyEquality<Key, Equal>
class SmallExactBackend {
 public:
  using key_type = Key;
  using result_type = Result;

  SmallExactBackend() = default;
  SmallExactBackend(SmallExactBackend &&) = default;
  SmallExactBackend &operator=(SmallExactBackend &&) = default;
  SmallExactBackend(const SmallExactBackend &) = delete;
  SmallExactBackend &operator=(const SmallExactBackend &) = delete;

  // Control path. Returns false if already at kMaxRules.
  static constexpr size_t kMaxRules = 64;

  bool insert(const Key &key, Result result) {
    // Overwrite existing entry if key already present.
    for (auto &e : entries_) {
      if (Equal{}(e.first, key)) {
        e.second = std::move(result);
        return true;
      }
    }
    if (entries_.size() >= kMaxRules) return false;
    entries_.emplace_back(key, std::move(result));
    return true;
  }

  bool remove(const Key &key) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const auto &e) { return Equal{}(e.first, key); });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
  }

  // Packet path. Linear scan.
  [[nodiscard]] const Result *lookup(const Key &key) const noexcept {
    for (const auto &e : entries_) {
      if (Equal{}(e.first, key)) return &e.second;
    }
    return nullptr;
  }

  // Batch via scalar fallback. Returns hit mask.
  [[nodiscard]] uint64_t lookup_batch(std::span<const Key> keys,
                                      std::span<Result> results) const noexcept {
    promise(keys.size() == results.size());
    promise(keys.size() <= 64);
    uint64_t hits = 0;
    for (size_t i = 0; i < keys.size(); i++) {
      const Result *r = lookup(keys[i]);
      if (r) {
        results[i] = *r;
        hits |= (uint64_t{1} << i);
      }
    }
    return hits;
  }

  [[nodiscard]] size_t size() const noexcept { return entries_.size(); }

  [[nodiscard]] BackendInfo info() const noexcept {
    return BackendInfo{
        .kind = ExactBackendKind::kSmall,
        .rule_count = entries_.size(),
        .key_size = sizeof(Key),
        .result_size = sizeof(Result),
    };
  }

 private:
  std::vector<std::pair<Key, Result>> entries_;
};

// Sorted flat vector for benchmark comparison only. Binary search O(log n).
// Not production policy; useful for measuring search cost vs hashing.
template <ClassifierKey Key, typename Result,
          typename Compare = std::less<Key>,
          typename Equal = typename KeyTraits<Key>::equal_type>
class SortedFlatBackend {
 public:
  using key_type = Key;
  using result_type = Result;

  SortedFlatBackend() = default;
  SortedFlatBackend(SortedFlatBackend &&) = default;
  SortedFlatBackend &operator=(SortedFlatBackend &&) = default;
  SortedFlatBackend(const SortedFlatBackend &) = delete;
  SortedFlatBackend &operator=(const SortedFlatBackend &) = delete;

  // Control path. Inserts or overwrites; keeps sorted.
  void insert(const Key &key, Result result) {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                               [](const auto &e, const Key &k) {
                                 return Compare{}(e.first, k);
                               });
    if (it != entries_.end() && Equal{}(it->first, key)) {
      it->second = std::move(result);
    } else {
      entries_.insert(it, {key, std::move(result)});
    }
  }

  bool remove(const Key &key) {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                               [](const auto &e, const Key &k) {
                                 return Compare{}(e.first, k);
                               });
    if (it == entries_.end() || !Equal{}(it->first, key)) return false;
    entries_.erase(it);
    return true;
  }

  [[nodiscard]] const Result *lookup(const Key &key) const noexcept {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                               [](const auto &e, const Key &k) {
                                 return Compare{}(e.first, k);
                               });
    if (it != entries_.end() && Equal{}(it->first, key)) return &it->second;
    return nullptr;
  }

  [[nodiscard]] uint64_t lookup_batch(std::span<const Key> keys,
                                      std::span<Result> results) const noexcept {
    promise(keys.size() == results.size());
    promise(keys.size() <= 64);
    uint64_t hits = 0;
    for (size_t i = 0; i < keys.size(); i++) {
      const Result *r = lookup(keys[i]);
      if (r) { results[i] = *r; hits |= (uint64_t{1} << i); }
    }
    return hits;
  }

  [[nodiscard]] size_t size() const noexcept { return entries_.size(); }

  [[nodiscard]] BackendInfo info() const noexcept {
    return BackendInfo{
        .kind = ExactBackendKind::kSmall,
        .rule_count = entries_.size(),
        .key_size = sizeof(Key),
        .result_size = sizeof(Result),
    };
  }

 private:
  std::vector<std::pair<Key, Result>> entries_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_SMALL_EXACT_H_
