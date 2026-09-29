// SPDX-License-Identifier: BSD-3-Clause

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
