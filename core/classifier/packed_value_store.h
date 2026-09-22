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
#ifndef BESS_CLASSIFIER_PACKED_VALUE_STORE_H_
#define BESS_CLASSIFIER_PACKED_VALUE_STORE_H_

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "classifier/classifier.h"

namespace bess::classifier {

// A contiguous immutable value store indexed by ResultSlot.
//
// Slots are 1-based: slot 0 is reserved as "miss" / invalid and maps to an
// empty span. The store is built once on the control plane and then published
// via RcuPtr; no mutation after construction.
//
// Layout: values_[0..value_size-1] = slot 1,
//         values_[value_size..2*value_size-1] = slot 2, ...
//
// Value stores require value_size > 0. Zero-payload classifiers represent presence
// entirely through hit masks without allocating or indexing payload storage.
class PackedValueStore {
 public:
  // value_size: byte width of each stored value (must be > 0).
  explicit PackedValueStore(size_t value_size) : value_size_(value_size) {}

  [[nodiscard]] bool valid() const noexcept { return value_size_ > 0; }

  // Build path (control plane). Appends a copy of `value` and returns the
  // 1-based slot assigned to it. Returns ResultSlot(0) if value_size == 0
  // or on size mismatch.
  ResultSlot Add(ConstBytes value) {
    if (value_size_ == 0 || value.size() != value_size_) {
      return ResultSlot(0);
    }
    const uint32_t slot = static_cast<uint32_t>(size()) + 1;
    const size_t old = values_.size();
    values_.resize(old + value_size_);
    std::memcpy(values_.data() + old, value.data(), value_size_);
    return ResultSlot(slot);
  }

  // Lookup path (packet path). slot 0 or out-of-range returns empty span.
  [[nodiscard]] ConstBytes lookup(ResultSlot slot) const noexcept {
    const uint32_t idx = slot.value();
    if (idx == 0 || idx > size()) {
      return {};
    }
    const size_t offset = static_cast<size_t>(idx - 1) * value_size_;
    return ConstBytes(
        reinterpret_cast<const std::byte*>(values_.data()) + offset,
        value_size_);
  }

  [[nodiscard]] size_t value_size() const noexcept { return value_size_; }

  // Number of stored values (not counting slot 0).
  [[nodiscard]] size_t size() const noexcept {
    return value_size_ == 0 ? 0 : values_.size() / value_size_;
  }
  [[nodiscard]] size_t storage_bytes() const noexcept { return values_.size(); }

 private:
  size_t value_size_;
  std::vector<std::byte> values_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_PACKED_VALUE_STORE_H_
