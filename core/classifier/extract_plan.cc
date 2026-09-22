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
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace bess::classifier {
namespace {

ConstBytes Source(const SourceView &view, SourceKind kind) {
  return kind == SourceKind::kPacket ? view.packet : view.metadata;
}

// Exact-width copy: constant sizes compile to single loads/stores (no libc
// memcpy call for the common 1/2/4/8-byte fields); unaligned-safe via memcpy
// semantics. Never over-reads: exactly `size` source bytes are touched.
inline void CopyBytes(std::byte *dst, const std::byte *src,
                      size_t size) noexcept {
  switch (size) {
    case 1:
      dst[0] = src[0];
      break;
    case 2:
      std::memcpy(dst, src, 2);
      break;
    case 4:
      std::memcpy(dst, src, 4);
      break;
    case 8:
      std::memcpy(dst, src, 8);
      break;
    default:
      std::memcpy(dst, src, size);
      break;
  }
}

// Exact-width copy + mask. Width-specific ANDs keep masked 1/2/4/8-byte
// fields on the same single-instruction path as unmasked ones.
inline void CopyMasked(std::byte *dst, const std::byte *src, size_t size,
                       const std::byte *mask) noexcept {
  switch (size) {
    case 1:
      dst[0] = static_cast<std::byte>(src[0] & mask[0]);
      break;
    case 2: {
      uint16_t v;
      uint16_t m;
      std::memcpy(&v, src, 2);
      std::memcpy(&m, mask, 2);
      v &= m;
      std::memcpy(dst, &v, 2);
      break;
    }
    case 4: {
      uint32_t v;
      uint32_t m;
      std::memcpy(&v, src, 4);
      std::memcpy(&m, mask, 4);
      v &= m;
      std::memcpy(dst, &v, 4);
      break;
    }
    case 8: {
      uint64_t v;
      uint64_t m;
      std::memcpy(&v, src, 8);
      std::memcpy(&m, mask, 8);
      v &= m;
      std::memcpy(dst, &v, 8);
      break;
    }
    default:
      std::memcpy(dst, src, size);
      for (size_t k = 0; k < size; k++) {
        dst[k] &= mask[k];
      }
      break;
  }
}

// Unchecked op execution: the caller has established that the source spans
// cover every op (one required-bytes check per packet) and that the key span
// covers the key (key.size() >= key_size_, with destination ranges validated
// at Compile time).
inline void CopyOpUnchecked(const ExtractOp &op, ConstBytes source_bytes,
                            std::byte *dst) noexcept {
  const std::byte *src = source_bytes.data() + op.source_offset;
  if (op.mask.empty()) {
    CopyBytes(dst + op.destination_offset, src, op.size);
  } else {
    CopyMasked(dst + op.destination_offset, src, op.size, op.mask.data());
  }
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

// Width dispatch for one op whose source range is already established:
// `src` points at the op's first source byte, `dst` at its first key byte.
inline void CopySingleUnchecked(const ExtractOp &op, const std::byte *src,
                                std::byte *dst) noexcept {
  if (op.mask.empty()) {
    CopyBytes(dst, src, op.size);
  } else {
    CopyMasked(dst, src, op.size, op.mask.data());
  }
}

// Saturating end computation for required-bytes precomputation: source
// offsets are runtime values, so guard against offset + size overflow.
size_t SaturatingEnd(size_t offset, size_t size) {
  if (size > std::numeric_limits<size_t>::max() - offset) {
    return std::numeric_limits<size_t>::max();
  }
  return offset + size;
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

  // One required-bytes check per source per packet is equivalent to checking
  // every op: all ops fit iff the furthest op end fits.
  size_t required_packet_bytes = 0;
  size_t required_metadata_bytes = 0;
  for (const ExtractOp &op : coalesced) {
    const size_t end = SaturatingEnd(op.source_offset, op.size);
    if (op.source == SourceKind::kPacket) {
      required_packet_bytes = std::max(required_packet_bytes, end);
    } else {
      required_metadata_bytes = std::max(required_metadata_bytes, end);
    }
  }

  // Dense coverage: ops (sorted by destination) tile [0, key_size) exactly.
  // Validation already rejects overlaps and out-of-key destinations, so
  // checking exact tiling suffices.
  size_t covered_end = 0;
  bool covers = true;
  for (const ExtractOp &op : coalesced) {
    if (op.destination_offset != covered_end) {
      covers = false;
      break;
    }
    covered_end += op.size;
  }
  covers = covers && covered_end == schema.key_size;

  return ExtractPlan(schema.key_size, schema.bounds, std::move(coalesced),
                     kernel, kernel_kind, required_packet_bytes,
                     required_metadata_bytes, covers);
}

bool ExtractPlan::ExecuteOne(const SourceView &source,
                             MutableBytes key) const noexcept {
  if (key.size() < key_size_) {
    return false;
  }
  // One check per source covers every op (see Compile); a failed check
  // writes nothing, so invalid rows stay pristine for the caller to zero.
  if (bounds_ == BoundsPolicy::kCheck) {
    if (source.packet.size() < required_packet_bytes_) {
      return false;
    }
    if (source.metadata.size() < required_metadata_bytes_) {
      return false;
    }
  }
  for (const ExtractOp &op : ops_) {
    // Destination ranges are validated at Compile time against key_size_,
    // which key.size() covers.
    CopyOpUnchecked(op, Source(source, op.source), key.data());
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
  promise(sources.empty() || key_stride <= output.size() / sources.size());
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
  const bool check = plan.bounds_ == BoundsPolicy::kCheck;
  const size_t need_packet = plan.required_packet_bytes_;
  const size_t need_metadata = plan.required_metadata_bytes_;
  for (size_t i = 0; i < sources.size(); i++) {
    const SourceView &source = sources[i];
    if (check && (source.packet.size() < need_packet ||
                  source.metadata.size() < need_metadata)) {
      continue;
    }
    std::byte *dst = output.data() + i * key_stride;
    for (const ExtractOp &op : plan.ops_) {
      CopyOpUnchecked(op, Source(source, op.source), dst);
    }
    valid |= (uint64_t{1} << i);
  }
  return valid;
}

uint64_t ExtractPlan::ExecuteSinglePacket(const ExtractPlan &plan,
                                          std::span<const SourceView> sources,
                                          MutableBytes output,
                                          size_t key_stride) noexcept {
  uint64_t valid = 0;
  const ExtractOp &op = plan.ops_[0];
  const bool check = plan.bounds_ == BoundsPolicy::kCheck;
  const size_t need = plan.required_packet_bytes_;
  for (size_t i = 0; i < sources.size(); i++) {
    const ConstBytes packet = sources[i].packet;
    if (check && packet.size() < need) {
      continue;
    }
    CopySingleUnchecked(op, packet.data() + op.source_offset,
                        output.data() + i * key_stride +
                            op.destination_offset);
    valid |= (uint64_t{1} << i);
  }
  return valid;
}

uint64_t ExtractPlan::ExecuteSingleMetadata(const ExtractPlan &plan,
                                            std::span<const SourceView> sources,
                                            MutableBytes output,
                                            size_t key_stride) noexcept {
  uint64_t valid = 0;
  const ExtractOp &op = plan.ops_[0];
  const bool check = plan.bounds_ == BoundsPolicy::kCheck;
  const size_t need = plan.required_metadata_bytes_;
  for (size_t i = 0; i < sources.size(); i++) {
    const ConstBytes metadata = sources[i].metadata;
    if (check && metadata.size() < need) {
      continue;
    }
    CopySingleUnchecked(op, metadata.data() + op.source_offset,
                        output.data() + i * key_stride +
                            op.destination_offset);
    valid |= (uint64_t{1} << i);
  }
  return valid;
}

}  // namespace bess::classifier
