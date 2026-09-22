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

#include "classifier/result_plan.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "utils/common.h"

namespace bess::classifier {
namespace {

bool Fits(size_t offset, size_t size, size_t limit) {
  return offset <= limit && size <= limit - offset;
}

bool CanCoalesce(const PlaceOp &previous, const PlaceOp &current) {
  return current.value_offset >= previous.value_offset &&
         current.value_offset - previous.value_offset == previous.size &&
         current.destination_offset >= previous.destination_offset &&
         current.destination_offset - previous.destination_offset == previous.size &&
         previous.size <= std::numeric_limits<size_t>::max() - current.size;
}

}  // namespace

ClassifierResult<ResultPlan> ResultPlan::Compile(
    const RuntimeClassifierSchema &schema) {
  ClassifierResult<void> valid = schema.Validate();
  if (!valid) {
    return std::unexpected(valid.error());
  }

  std::vector<PlaceOp> ops;
  ops.reserve(schema.result_fields.size());
  for (const RuntimeResultField &field : schema.result_fields) {
    ops.push_back(
        PlaceOp{field.value_offset, field.destination_offset, field.size});
  }
  std::stable_sort(ops.begin(), ops.end(),
                   [](const PlaceOp &lhs, const PlaceOp &rhs) {
                     return lhs.destination_offset < rhs.destination_offset;
                   });

  std::vector<PlaceOp> coalesced;
  coalesced.reserve(ops.size());
  for (const PlaceOp &op : ops) {
    if (!coalesced.empty()) {
      PlaceOp &previous = coalesced.back();
      const bool contiguous = CanCoalesce(previous, op);
      if (contiguous) {
        previous.size += op.size;
        continue;
      }
    }
    coalesced.push_back(op);
  }

  ResultBatchFn kernel = &ResultPlan::ApplyGeneric;
  ResultKernel kernel_kind = ResultKernel::kGeneric;
  if (coalesced.size() == 1) {
    kernel = &ResultPlan::ApplySingle;
    kernel_kind = ResultKernel::kSingle;
  }
  return ResultPlan(schema.value_size, std::move(coalesced), kernel,
                    kernel_kind);
}

bool ResultPlan::ApplyOne(ConstBytes value,
                          MutableBytes metadata) const noexcept {
  if (value.size() < value_size_) {
    return false;
  }
  for (const PlaceOp &op : ops_) {
    if (!Fits(op.value_offset, op.size, value.size()) ||
        !Fits(op.destination_offset, op.size, metadata.size())) {
      return false;
    }
    std::memcpy(metadata.data() + op.destination_offset,
                value.data() + op.value_offset, op.size);
  }
  return true;
}

bool ResultPlan::Apply(ConstBytes value, MutableBytes metadata) const noexcept {
  return ApplyOne(value, metadata);
}

bool ResultPlan::ApplyBatch(std::span<const ConstBytes> values,
                            std::span<MutableBytes> metadata) const noexcept {
  promise(values.size() == metadata.size());
  return kernel_(*this, values, metadata);
}

bool ResultPlan::ApplySingle(const ResultPlan &plan,
                             std::span<const ConstBytes> values,
                             std::span<MutableBytes> metadata) noexcept {
  const PlaceOp &op = plan.ops_[0];
  for (size_t i = 0; i < values.size(); i++) {
    if (values[i].size() < plan.value_size_ ||
        !Fits(op.value_offset, op.size, values[i].size()) ||
        !Fits(op.destination_offset, op.size, metadata[i].size())) {
      return false;
    }
    std::memcpy(metadata[i].data() + op.destination_offset,
                values[i].data() + op.value_offset, op.size);
  }
  return true;
}

bool ResultPlan::ApplyGeneric(const ResultPlan &plan,
                              std::span<const ConstBytes> values,
                              std::span<MutableBytes> metadata) noexcept {
  for (size_t i = 0; i < values.size(); i++) {
    if (!plan.ApplyOne(values[i], metadata[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace bess::classifier
