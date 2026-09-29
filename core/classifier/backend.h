// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_BACKEND_H_
#define BESS_CLASSIFIER_BACKEND_H_

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "classifier/byte_key.h"

namespace bess::classifier {

template <typename Result>
using LookupBatchFn = uint64_t (*)(const void *state, ConstBytes keys,
                                   size_t key_stride,
                                   std::span<Result> results) noexcept;

using DestroyFn = void (*)(void *state) noexcept;

template <typename Result>
struct RuntimeExactOps {
  LookupBatchFn<Result> lookup_batch = nullptr;
  DestroyFn destroy = nullptr;
  BackendInfo info{};
};

// Generation-level type erasure for runtime-selected backends parameterized on
// the result type. One function-pointer dispatch per batch; returns a hit mask
// (bit i set ↔ results[i] valid).
template <typename Result>
class RuntimeExactBackend {
 public:
  RuntimeExactBackend() = default;
  RuntimeExactBackend(const RuntimeExactOps<Result> &ops, void *state)
      : ops_(ops), state_(state) {}
  // move-only
  RuntimeExactBackend(const RuntimeExactBackend &) = delete;
  RuntimeExactBackend &operator=(const RuntimeExactBackend &) = delete;
  RuntimeExactBackend(RuntimeExactBackend &&other) noexcept
      : ops_(other.ops_), state_(std::exchange(other.state_, nullptr)) {
    other.ops_ = {};
  }
  RuntimeExactBackend &operator=(RuntimeExactBackend &&other) noexcept {
    if (this != &other) {
      Reset();
      ops_ = other.ops_;
      state_ = std::exchange(other.state_, nullptr);
      other.ops_ = {};
    }
    return *this;
  }
  ~RuntimeExactBackend() { Reset(); }

  // Returns hit mask. Bit i set if results[i] is valid.
  [[nodiscard]] uint64_t lookup_batch(ConstBytes keys, size_t key_stride,
                                      std::span<Result> results) const noexcept {
    return ops_.lookup_batch(state_, keys, key_stride, results);
  }

  BackendInfo info() const noexcept { return ops_.info; }

  explicit operator bool() const noexcept {
    return ops_.lookup_batch != nullptr &&
           (state_ == nullptr || ops_.destroy != nullptr);
  }

 private:
  void Reset() noexcept {
    if (state_ != nullptr && ops_.destroy != nullptr) ops_.destroy(state_);
    ops_ = {};
    state_ = nullptr;
  }
  RuntimeExactOps<Result> ops_{};
  void *state_ = nullptr;
};

// Slot-erased alias used by the generic runtime path and RuntimeClassifierGeneration.
using RuntimeSlotBackend = RuntimeExactBackend<ResultSlot>;

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_BACKEND_H_
