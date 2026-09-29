// SPDX-License-Identifier: BSD-3-Clause

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

template <typename Result = ResultSlot>
using RuntimeClassifierGeneration = Generation<RuntimeExactBackend<Result>>;

using RuntimeSlotClassifierGeneration = RuntimeClassifierGeneration<ResultSlot>;

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_GENERATION_H_
