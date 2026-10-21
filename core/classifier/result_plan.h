// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_RESULT_PLAN_H_
#define BESS_CLASSIFIER_RESULT_PLAN_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "classifier/runtime_schema.h"

namespace bess::classifier {

// One output placement, packed for the packet path: copy `size` bytes from
// `value_offset` in the record to `destination_offset` in the packet or the
// metadata block. `mask_offset` indexes masks_ when `masked` is 1; unmasked ops
// copy the bytes as they are. Sixteen bytes, so a bucket of four ops is one
// cache line; the control-plane form that carries a std::vector per op is not.
struct PackedOp {
  uint32_t value_offset = 0;
  uint32_t destination_offset = 0;
  uint32_t mask_offset = 0;
  uint16_t size = 0;
  uint8_t destination = 0;  // BufferKind
  uint8_t masked = 0;
};

static_assert(sizeof(PackedOp) == 16, "PackedOp must stay one 16-byte slot");

// The per-packet buffers a result plan writes. The mirror of SourceView.
struct DestinationView {
  MutableBytes packet;
  MutableBytes metadata;
};

enum class ResultKernel : uint8_t {
  kSingle,
  kTwoOp,
  kGeneric,
};

class ResultPlan;
using ResultBatchFn = bool (*)(const ResultPlan &,
                                std::span<const ConstBytes>,
                                std::span<const DestinationView>) noexcept;

class ResultPlan {
 public:
  static ClassifierResult<ResultPlan> Compile(
      const RuntimeClassifierSchema &schema);

  [[nodiscard]] bool Apply(ConstBytes value,
                           const DestinationView &destination) const noexcept;

  [[nodiscard]] bool ApplyBatch(
      std::span<const ConstBytes> values,
      std::span<const DestinationView> destinations) const noexcept;

  [[nodiscard]] size_t value_size() const noexcept { return value_size_; }
  [[nodiscard]] ResultKernel kernel() const noexcept { return kernel_kind_; }
  [[nodiscard]] std::span<const PackedOp> ops() const noexcept { return ops_; }
  // The most a destination buffer must have for every op to fit. One check per
  // pair replaces one check per op per pair.
  [[nodiscard]] size_t required_packet_bytes() const noexcept {
    return required_packet_bytes_;
  }
  [[nodiscard]] size_t required_metadata_bytes() const noexcept {
    return required_metadata_bytes_;
  }

 private:
  // A run of ops_ that share a destination, a size class and a mask state, so
  // the loop body needs no branch on any of them.
  struct Bucket {
    uint32_t begin = 0;
    uint32_t end = 0;
    uint8_t destination = 0;  // BufferKind
    uint8_t size_class = 0;   // 1, 2, 4, 8, 16, 32 -> 0..5; other -> 6
    bool masked = false;
  };

  ResultPlan(size_t value_size, std::vector<PackedOp> ops,
             std::vector<std::byte> masks, std::array<Bucket, 28> buckets,
             size_t bucket_count, ResultBatchFn kernel,
             ResultKernel kernel_kind, size_t required_packet_bytes,
             size_t required_metadata_bytes)
      : value_size_(value_size),
        ops_(std::move(ops)),
        masks_(std::move(masks)),
        buckets_(buckets),
        bucket_count_(bucket_count),
        kernel_(kernel),
        kernel_kind_(kernel_kind),
        required_packet_bytes_(required_packet_bytes),
        required_metadata_bytes_(required_metadata_bytes) {}

  [[nodiscard, gnu::always_inline]] inline bool Fits(
      ConstBytes value, const DestinationView &destination) const noexcept;

  // One op with its width and mask state known at kernel-selection time; the
  // common coalesced case runs straight-line.
  template <size_t Size, bool Masked>
  static bool ApplySingleSized(const ResultPlan &,
                               std::span<const ConstBytes>,
                               std::span<const DestinationView>) noexcept;
  static bool ApplyTwoOps(const ResultPlan &, std::span<const ConstBytes>,
                          std::span<const DestinationView>) noexcept;
  template <size_t N>
  static bool ApplyNOps(const ResultPlan &, std::span<const ConstBytes>,
                        std::span<const DestinationView>) noexcept;
  static bool ApplyGeneric(const ResultPlan &, std::span<const ConstBytes>,
                           std::span<const DestinationView>) noexcept;
  static ResultBatchFn SelectSingle(const PackedOp &op) noexcept;

  size_t value_size_;
  std::vector<PackedOp> ops_;
  std::vector<std::byte> masks_;
  std::array<Bucket, 28> buckets_;
  size_t bucket_count_;
  ResultBatchFn kernel_;
  ResultKernel kernel_kind_;
  size_t required_packet_bytes_ = 0;
  size_t required_metadata_bytes_ = 0;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RESULT_PLAN_H_
