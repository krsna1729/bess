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
#include "utils/common.h"

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
using ExtractBatchFn = uint64_t (*)(const ExtractPlan &,
                                    std::span<const SourceView>, MutableBytes,
                                    size_t key_stride) noexcept;

class ExtractPlan {
 public:
  static ClassifierResult<ExtractPlan> Compile(
      const RuntimeClassifierSchema &schema);

  // No allocation, strings, metadata lookup or configuration traversal.
  [[nodiscard]] bool Execute(const SourceView &source,
                             MutableBytes key) const noexcept;

  // `output` contains one key per source at `key_stride` bytes.
  // Returns a 64-bit mask where bit i is set iff extraction for source i succeeded.
  // Under BoundsPolicy::kCheck, a short/truncated source clears bit i without
  // stopping extraction for other packets in the batch.
  //
  // Output initialization is the caller's job and depends on
  // fully_covers_key():
  //   - fully covered: a successful extraction writes every byte of the key
  //     row, so scratch needs no pre-zeroing; the caller must zero invalid
  //     rows before handing them to a backend that still looks up all rows
  //     (a failed extraction writes nothing, but scratch rows are reused).
  //   - gapped: gap bytes are never written, so the caller must pre-zero
  //     (or otherwise initialize) them; invalid rows must still be zeroed
  //     before a look-up-all-rows backend.
  [[nodiscard]] uint64_t ExecuteBatch(std::span<const SourceView> sources,
                                      MutableBytes output,
                                      size_t key_stride) const noexcept;

  [[nodiscard]] size_t key_size() const noexcept { return key_size_; }
  [[nodiscard]] BoundsPolicy bounds() const noexcept { return bounds_; }
  [[nodiscard]] ExtractKernel kernel() const noexcept { return kernel_kind_; }
  [[nodiscard]] std::span<const ExtractOp> ops() const noexcept { return ops_; }
  // True when the coalesced ops write every key byte densely from offset 0
  // (no gaps, no trailing slack). Successful extraction then fully defines
  // the key row; see ExecuteBatch for the caller contract.
  [[nodiscard]] bool fully_covers_key() const noexcept {
    return fully_covers_key_;
  }

 private:
  ExtractPlan(size_t key_size, BoundsPolicy bounds, std::vector<ExtractOp> ops,
              ExtractBatchFn kernel, ExtractKernel kernel_kind,
              size_t required_packet_bytes, size_t required_metadata_bytes,
              bool fully_covers_key)
      : key_size_(key_size),
        bounds_(bounds),
        ops_(std::move(ops)),
        kernel_(kernel),
        kernel_kind_(kernel_kind),
        required_packet_bytes_(required_packet_bytes),
        required_metadata_bytes_(required_metadata_bytes),
        fully_covers_key_(fully_covers_key) {}

  [[nodiscard]] bool ExecuteOne(const SourceView &source,
                                MutableBytes key) const noexcept;
  static uint64_t ExecuteGeneric(const ExtractPlan &, std::span<const SourceView>,
                                 MutableBytes, size_t) noexcept;
  static uint64_t ExecuteSinglePacket(const ExtractPlan &,
                                      std::span<const SourceView>, MutableBytes,
                                      size_t) noexcept;
  static uint64_t ExecuteSingleMetadata(const ExtractPlan &,
                                        std::span<const SourceView>, MutableBytes,
                                        size_t) noexcept;

  size_t key_size_;
  BoundsPolicy bounds_;
  std::vector<ExtractOp> ops_;
  ExtractBatchFn kernel_;
  ExtractKernel kernel_kind_;
  // Max source end (offset + size) over the ops reading each source. Under
  // kCheck, one comparison of the source length against the required bytes is
  // equivalent to checking every op's range, so kernels check once per
  // packet and then run exact-width copies unchecked.
  size_t required_packet_bytes_ = 0;
  size_t required_metadata_bytes_ = 0;
  bool fully_covers_key_ = false;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_EXTRACT_PLAN_H_
