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
// CONTRACT, STRICT LIABILITY, OR NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_CLASSIFIER_GENERATION_H_
#define BESS_CLASSIFIER_GENERATION_H_

#include <utility>

#include "classifier/backend.h"
#include "classifier/extract_plan.h"
#include "classifier/result_plan.h"

namespace bess::classifier {

// A generation owns every piece of one logical classifier. It is immutable
// after construction and is published by RcuPtr by the caller; RCU does not
// live inside this value. Published generations are safe for concurrent const
// reads, while builders create replacements off the packet path.
template <typename Backend>
class Generation {
 public:
  Generation(ExtractPlan extract, Backend backend, ResultPlan result,
             BackendInfo info)
      : extract_(std::move(extract)),
        backend_(std::move(backend)),
        result_(std::move(result)),
        info_(info) {}

  Generation(const Generation &) = delete;
  Generation &operator=(const Generation &) = delete;
  Generation(Generation &&) = delete;
  Generation &operator=(Generation &&) = delete;

  [[nodiscard]] const ExtractPlan &extract() const noexcept { return extract_; }
  [[nodiscard]] const Backend &backend() const noexcept { return backend_; }
  [[nodiscard]] const ResultPlan &result() const noexcept { return result_; }
  [[nodiscard]] const BackendInfo &info() const noexcept { return info_; }

 private:
  ExtractPlan extract_;
  [[no_unique_address]] Backend backend_;
  ResultPlan result_;
  BackendInfo info_;
};

using RuntimeClassifierGeneration = Generation<RuntimeExactBackend<ResultSlot>>;

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_GENERATION_H_
