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

#ifndef BESS_CLASSIFIER_EXTRACT_PLAN_H_
#define BESS_CLASSIFIER_EXTRACT_PLAN_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "classifier/runtime_schema.h"

namespace bess::classifier {

struct SourceView {
  ConstBytes packet;
  ConstBytes metadata;
};

struct ExtractOp {
  SourceKind source = SourceKind::kPacket;
  size_t source_offset = 0;
  size_t destination_offset = 0;
  size_t size = 0;
  std::vector<std::byte> mask;  // empty = no masking; non-empty ANDed after copy
};

enum class ExtractKernel : uint8_t {
  kSinglePacket,
  kSingleMetadata,
  kGeneric,
};

class ExtractPlan;
using ExtractBatchFn = bool (*)(const ExtractPlan &,
                                std::span<const SourceView>, MutableBytes,
                                size_t key_stride) noexcept;

class ExtractPlan {
 public:
  static ClassifierResult<ExtractPlan> Compile(
      const RuntimeClassifierSchema &schema);

  // No allocation, strings, metadata lookup or configuration traversal.
  [[nodiscard]] bool Execute(const SourceView &source,
                             MutableBytes key) const noexcept;

  // `output` contains one key per source at `key_stride` bytes. The function
  // returns false if an output or checked source range is insufficient.
  [[nodiscard]] bool ExecuteBatch(std::span<const SourceView> sources,
                                  MutableBytes output,
                                  size_t key_stride) const noexcept;

  [[nodiscard]] size_t key_size() const noexcept { return key_size_; }
  [[nodiscard]] BoundsPolicy bounds() const noexcept { return bounds_; }
  [[nodiscard]] ExtractKernel kernel() const noexcept { return kernel_kind_; }
  [[nodiscard]] std::span<const ExtractOp> ops() const noexcept { return ops_; }

 private:
  ExtractPlan(size_t key_size, BoundsPolicy bounds, std::vector<ExtractOp> ops,
              ExtractBatchFn kernel, ExtractKernel kernel_kind)
      : key_size_(key_size),
        bounds_(bounds),
        ops_(std::move(ops)),
        kernel_(kernel),
        kernel_kind_(kernel_kind) {}

  [[nodiscard]] bool ExecuteOne(const SourceView &source,
                                MutableBytes key) const noexcept;
  static bool ExecuteGeneric(const ExtractPlan &, std::span<const SourceView>,
                             MutableBytes, size_t) noexcept;
  static bool ExecuteSinglePacket(const ExtractPlan &,
                                  std::span<const SourceView>, MutableBytes,
                                  size_t) noexcept;
  static bool ExecuteSingleMetadata(const ExtractPlan &,
                                    std::span<const SourceView>, MutableBytes,
                                    size_t) noexcept;

  size_t key_size_;
  BoundsPolicy bounds_;
  std::vector<ExtractOp> ops_;
  ExtractBatchFn kernel_;
  ExtractKernel kernel_kind_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_EXTRACT_PLAN_H_
