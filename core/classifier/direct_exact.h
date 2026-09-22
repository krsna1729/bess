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
#ifndef BESS_CLASSIFIER_DIRECT_EXACT_H_
#define BESS_CLASSIFIER_DIRECT_EXACT_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "classifier/backend.h"
#include "classifier/classifier.h"
#include "classifier/typed_exact.h"

namespace bess::classifier {

// Experimental bounded direct array lookup backend for 1-byte or 2-byte keys.
// Lookups are direct array indexing: O(1) guaranteed with zero hashing overhead.
// Only valid for domains with genuinely bounded key spaces (uint8_t or uint16_t).
template <typename Key, typename Result>
  requires(std::is_integral_v<Key> && sizeof(Key) <= 2)
class DirectExactBackend {
 public:
  using key_type = Key;
  using result_type = Result;

  static constexpr size_t kTableCapacity =
      sizeof(Key) == 1 ? 256u : 65536u;

  DirectExactBackend() : entries_(kTableCapacity) {}

  DirectExactBackend(DirectExactBackend &&) = default;
  DirectExactBackend &operator=(DirectExactBackend &&) = default;
  DirectExactBackend(const DirectExactBackend &) = delete;
  DirectExactBackend &operator=(const DirectExactBackend &) = delete;

  // Control path
  bool insert(Key key, Result result) {
    const size_t idx = static_cast<size_t>(static_cast<std::make_unsigned_t<Key>>(key));
    if (!entries_[idx].has_value()) {
      count_++;
    }
    entries_[idx] = std::move(result);
    return true;
  }

  bool remove(Key key) {
    const size_t idx = static_cast<size_t>(static_cast<std::make_unsigned_t<Key>>(key));
    if (entries_[idx].has_value()) {
      entries_[idx] = std::nullopt;
      count_--;
      return true;
    }
    return false;
  }

  // Packet path: direct array lookup
  [[nodiscard]] const Result *lookup(Key key) const noexcept {
    const size_t idx = static_cast<size_t>(static_cast<std::make_unsigned_t<Key>>(key));
    return entries_[idx].has_value() ? &entries_[idx].value() : nullptr;
  }

  // Native batch lookup returning hit mask
  [[nodiscard]] uint64_t lookup_batch(std::span<const Key> keys,
                                      std::span<Result> results) const noexcept {
    assert(keys.size() == results.size());
    assert(keys.size() <= 64);
    uint64_t hits = 0;
    for (size_t i = 0; i < keys.size(); i++) {
      const Result *r = lookup(keys[i]);
      if (r != nullptr) {
        results[i] = *r;
        hits |= (uint64_t{1} << i);
      }
    }
    return hits;
  }

  [[nodiscard]] size_t size() const noexcept { return count_; }

  [[nodiscard]] BackendInfo info() const noexcept {
    return BackendInfo{
        .kind = ExactBackendKind::kDirect,
        .rule_count = count_,
        .key_size = sizeof(Key),
        .result_size = sizeof(Result),
        .storage_bytes = entries_.size() * sizeof(std::optional<Result>),
    };
  }

 private:
  std::vector<std::optional<Result>> entries_;
  size_t count_ = 0;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_DIRECT_EXACT_H_
