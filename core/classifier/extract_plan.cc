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

#include "classifier/extract_plan.h"

#include "utils/common.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace bess::classifier {
namespace {

bool Fits(size_t offset, size_t size, size_t limit) {
  return offset <= limit && size <= limit - offset;
}

ConstBytes Source(const SourceView &view, SourceKind kind) {
  return kind == SourceKind::kPacket ? view.packet : view.metadata;
}

bool CanCoalesce(const ExtractOp &previous, const ExtractOp &current) {
  if (!previous.mask.empty() || !current.mask.empty()) {
    return false;
  }
  return previous.source == current.source &&
         current.source_offset >= previous.source_offset &&
         current.source_offset - previous.source_offset == previous.size &&
         current.destination_offset >= previous.destination_offset &&
         current.destination_offset - previous.destination_offset == previous.size &&
         previous.size <= std::numeric_limits<size_t>::max() - current.size;
}

bool CopyOperation(const ExtractPlan &plan, const ExtractOp &op,
                   MutableBytes key, ConstBytes source_bytes) noexcept {
  if (!Fits(op.destination_offset, op.size, key.size())) {
    return false;
  }
  if (plan.bounds() == BoundsPolicy::kCheck &&
      !Fits(op.source_offset, op.size, source_bytes.size())) {
    return false;
  }

  std::byte *dst = key.data() + op.destination_offset;
  std::memcpy(dst, source_bytes.data() + op.source_offset, op.size);
  if (!op.mask.empty()) {
    for (size_t k = 0; k < op.size; k++) {
      dst[k] &= op.mask[k];
    }
  }
  return true;
}

bool CopySingle(const ExtractPlan &plan, const ExtractOp &op,
                ConstBytes source, std::byte *destination) noexcept {
  if (plan.bounds() == BoundsPolicy::kCheck &&
      !Fits(op.source_offset, op.size, source.size())) {
    return false;
  }
  std::memcpy(destination, source.data() + op.source_offset, op.size);
  if (!op.mask.empty()) {
    for (size_t k = 0; k < op.size; k++) {
      destination[k] &= op.mask[k];
    }
  }
  return true;
}

}  // namespace

ClassifierResult<ExtractPlan> ExtractPlan::Compile(
    const RuntimeClassifierSchema &schema) {
  ClassifierResult<void> valid = schema.Validate();
  if (!valid) {
    return std::unexpected(valid.error());
  }

  std::vector<ExtractOp> ops;
  ops.reserve(schema.key_fields.size());
  for (const RuntimeKeyField &field : schema.key_fields) {
    ExtractOp op{field.source, field.source_offset, field.key_offset,
                 field.size, field.normalization.mask};
    ops.push_back(std::move(op));
  }
  std::stable_sort(ops.begin(), ops.end(),
                   [](const ExtractOp &lhs, const ExtractOp &rhs) {
                     return lhs.destination_offset < rhs.destination_offset;
                   });

  std::vector<ExtractOp> coalesced;
  coalesced.reserve(ops.size());
  for (const ExtractOp &op : ops) {
    if (!coalesced.empty()) {
      ExtractOp &previous = coalesced.back();
      const bool contiguous = CanCoalesce(previous, op);
      if (contiguous) {
        previous.size += op.size;
        continue;
      }
    }
    coalesced.push_back(op);
  }

  ExtractBatchFn kernel = &ExtractPlan::ExecuteGeneric;
  ExtractKernel kernel_kind = ExtractKernel::kGeneric;
  if (coalesced.size() == 1) {
    if (coalesced[0].source == SourceKind::kPacket) {
      kernel = &ExtractPlan::ExecuteSinglePacket;
      kernel_kind = ExtractKernel::kSinglePacket;
    } else {
      kernel = &ExtractPlan::ExecuteSingleMetadata;
      kernel_kind = ExtractKernel::kSingleMetadata;
    }
  }

  return ExtractPlan(schema.key_size, schema.bounds, std::move(coalesced),
                     kernel, kernel_kind);
}

bool ExtractPlan::ExecuteOne(const SourceView &source,
                             MutableBytes key) const noexcept {
  if (key.size() < key_size_) {
    return false;
  }
  for (const ExtractOp &op : ops_) {
    if (!CopyOperation(*this, op, key, Source(source, op.source))) {
      return false;
    }
  }
  return true;
}

bool ExtractPlan::Execute(const SourceView &source,
                          MutableBytes key) const noexcept {
  return ExecuteOne(source, key);
}

uint64_t ExtractPlan::ExecuteBatch(std::span<const SourceView> sources,
                                   MutableBytes output,
                                   size_t key_stride) const noexcept {
  promise(sources.size() <= 64);
  promise(key_stride >= key_size_);
  promise(output.size() >= sources.size() * key_stride);
  if (sources.empty()) {
    return 0;
  }
  return kernel_(*this, sources, output, key_stride);
}

uint64_t ExtractPlan::ExecuteGeneric(const ExtractPlan &plan,
                                     std::span<const SourceView> sources,
                                     MutableBytes output,
                                     size_t key_stride) noexcept {
  uint64_t valid = 0;
  for (size_t i = 0; i < sources.size(); i++) {
    if (plan.ExecuteOne(sources[i], output.subspan(i * key_stride,
                                                   plan.key_size()))) {
      valid |= (uint64_t{1} << i);
    }
  }
  return valid;
}

uint64_t ExtractPlan::ExecuteSinglePacket(const ExtractPlan &plan,
                                          std::span<const SourceView> sources,
                                          MutableBytes output,
                                          size_t key_stride) noexcept {
  uint64_t valid = 0;
  const ExtractOp &op = plan.ops_[0];
  for (size_t i = 0; i < sources.size(); i++) {
    if (CopySingle(plan, op, sources[i].packet,
                   output.data() + i * key_stride + op.destination_offset)) {
      valid |= (uint64_t{1} << i);
    }
  }
  return valid;
}

uint64_t ExtractPlan::ExecuteSingleMetadata(const ExtractPlan &plan,
                                            std::span<const SourceView> sources,
                                            MutableBytes output,
                                            size_t key_stride) noexcept {
  uint64_t valid = 0;
  const ExtractOp &op = plan.ops_[0];
  for (size_t i = 0; i < sources.size(); i++) {
    if (CopySingle(plan, op, sources[i].metadata,
                   output.data() + i * key_stride + op.destination_offset)) {
      valid |= (uint64_t{1} << i);
    }
  }
  return valid;
}

}  // namespace bess::classifier
