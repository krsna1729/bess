// SPDX-License-Identifier: BSD-3-Clause

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
