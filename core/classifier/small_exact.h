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
// Satisfies ScalarExactBackend and MeasurableBackend. lookup_batch falls back
// to scalar loop and returns hit mask, so it also satisfies BatchExactBackend
// in ExactTable's if constexpr sense — but only via the scalar path.
// Intended for tiny rule counts (≤64); no SIMD, no hashing.
template <ClassifierKey Key, typename Result,
          typename Equal = typename KeyTraits<Key>::equal_type>
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
