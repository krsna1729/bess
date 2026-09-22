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

template <typename Backend, typename Key, typename Result>
concept ExactBackend =
    requires(const Backend &backend, const Key &key) {
      { backend.lookup(key) };
      { backend.size() } -> std::convertible_to<size_t>;
    } &&
    (std::same_as<decltype(std::declval<const Backend &>().lookup(
                    std::declval<const Key &>())),
                  std::optional<Result>> ||
     std::same_as<decltype(std::declval<const Backend &>().lookup(
                    std::declval<const Key &>())),
                  const Result *>);

using LookupBatchFn = void (*)(const void *state, ConstBytes keys,
                               size_t key_stride,
                               std::span<ResultSlot> slots) noexcept;
using DestroyFn = void (*)(void *state) noexcept;

struct RuntimeExactOps {
  LookupBatchFn lookup_batch = nullptr;
  DestroyFn destroy = nullptr;
  BackendInfo info{};
};

// Generation-level type erasure for runtime-selected backends. It is never
// used by the typed API and performs at most one function-pointer dispatch per
// batch.
class RuntimeExactBackend {
 public:
  RuntimeExactBackend() = default;
  RuntimeExactBackend(const RuntimeExactOps &ops, void *state)
      : ops_(ops), state_(state) {}

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

  void lookup_batch(ConstBytes keys, size_t key_stride,
                    std::span<ResultSlot> slots) const noexcept {
    ops_.lookup_batch(state_, keys, key_stride, slots);
  }

  BackendInfo info() const noexcept { return ops_.info; }

  explicit operator bool() const noexcept {
    return ops_.lookup_batch != nullptr && ops_.destroy != nullptr &&
           state_ != nullptr;
  }

 private:
  void Reset() noexcept {
    if (state_ != nullptr && ops_.destroy != nullptr) {
      ops_.destroy(state_);
    }
    ops_ = {};
    state_ = nullptr;
  }

  RuntimeExactOps ops_{};
  void *state_ = nullptr;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_BACKEND_H_
