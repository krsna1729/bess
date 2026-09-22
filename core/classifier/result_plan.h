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
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_CLASSIFIER_RESULT_PLAN_H_
#define BESS_CLASSIFIER_RESULT_PLAN_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "classifier/runtime_schema.h"

namespace bess::classifier {

struct PlaceOp {
  size_t value_offset = 0;
  size_t destination_offset = 0;
  size_t size = 0;
};

enum class ResultKernel : uint8_t {
  kSingle,
  kGeneric,
};

class ResultPlan;
using ResultBatchFn = bool (*)(const ResultPlan &, std::span<const ConstBytes>,
                               std::span<MutableBytes>) noexcept;

class ResultPlan {
 public:
  static ClassifierResult<ResultPlan> Compile(
      const RuntimeClassifierSchema &schema);

  [[nodiscard]] bool Apply(ConstBytes value,
                           MutableBytes metadata) const noexcept;

  [[nodiscard]] bool ApplyBatch(std::span<const ConstBytes> values,
                                std::span<MutableBytes> metadata) const noexcept;

  [[nodiscard]] size_t value_size() const noexcept { return value_size_; }
  [[nodiscard]] ResultKernel kernel() const noexcept { return kernel_kind_; }
  [[nodiscard]] std::span<const PlaceOp> ops() const noexcept { return ops_; }

 private:
  ResultPlan(size_t value_size, std::vector<PlaceOp> ops, ResultBatchFn kernel,
             ResultKernel kernel_kind)
      : value_size_(value_size),
        ops_(std::move(ops)),
        kernel_(kernel),
        kernel_kind_(kernel_kind) {}

  [[nodiscard]] bool ApplyOne(ConstBytes value,
                              MutableBytes metadata) const noexcept;
  static bool ApplySingle(const ResultPlan &, std::span<const ConstBytes>,
                          std::span<MutableBytes>) noexcept;
  static bool ApplyGeneric(const ResultPlan &, std::span<const ConstBytes>,
                           std::span<MutableBytes>) noexcept;

  size_t value_size_;
  std::vector<PlaceOp> ops_;
  ResultBatchFn kernel_;
  ResultKernel kernel_kind_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RESULT_PLAN_H_
