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
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_CLASSIFIER_TYPED_EXACT_H_
#define BESS_CLASSIFIER_TYPED_EXACT_H_

#include <cstddef>
#include <span>
#include <utility>

#include "classifier/backend.h"

#include "utils/common.h"

namespace bess::classifier {

// Static module authors use their real Key/Result types and a backend whose
// lookup is directly visible to the compiler. Runtime schema plans are not part
// of this type.
template <ClassifierKey Key, typename Result, typename Backend>
  requires ExactBackend<Backend, Key, Result>
class ExactTable {
 public:
  using key_type = Key;
  using result_type = Result;
  using backend_type = Backend;
  using lookup_result = decltype(std::declval<const Backend &>().lookup(
      std::declval<const Key &>()));

  explicit ExactTable(Backend backend) : backend_(std::move(backend)) {}

  [[nodiscard]] lookup_result lookup(const Key &key) const noexcept {
    return backend_.lookup(key);
  }


  void lookup_batch(std::span<const Key> keys,
                    std::span<lookup_result> results) const noexcept {
    promise(keys.size() == results.size());
    for (size_t i = 0; i < keys.size(); i++) {
      results[i] = backend_.lookup(keys[i]);
    }
  }
  [[nodiscard]] size_t size() const noexcept {
    return static_cast<size_t>(backend_.size());
  }

  [[nodiscard]] const Backend &backend() const noexcept { return backend_; }

 private:
  [[no_unique_address]] Backend backend_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_TYPED_EXACT_H_
