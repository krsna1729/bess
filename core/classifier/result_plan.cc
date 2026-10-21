// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/result_plan.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "classifier/plan_copy.h"
#include "utils/common.h"

namespace bess::classifier {
namespace {

[[gnu::always_inline]] inline size_t SizeClass(size_t size) {
  switch (size) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 4:
      return 2;
    case 8:
      return 3;
    case 16:
      return 4;
    case 32:
      return 5;
    default:
      return 6;
  }
}

// Bucket order: destination, then unmasked before masked, then size class. The
// sort makes each bucket a contiguous run of ops_ with the same copy shape.
[[gnu::always_inline]] inline size_t Rank(const PackedOp &op) {
  return (static_cast<size_t>(op.destination) * 2 + op.masked) * 7 +
         SizeClass(op.size);
}

// The control-plane form used while compiling; the packet path uses PackedOp.
struct CoalescedOp {
  BufferKind destination = BufferKind::kMetadata;
  size_t value_offset = 0;
  size_t destination_offset = 0;
  size_t size = 0;
  std::vector<std::byte> mask;
};

bool CanCoalesce(const CoalescedOp &previous, const CoalescedOp &current) {
  if (previous.destination != current.destination) {
    return false;
  }
  // A masked field's bytes are value & mask; merging it with another op would
  // need a combined mask, so the key side leaves masked fields alone too.
  if (!previous.mask.empty() || !current.mask.empty()) {
    return false;
  }
  return current.value_offset >= previous.value_offset &&
         current.value_offset - previous.value_offset == previous.size &&
         current.destination_offset >= previous.destination_offset &&
         current.destination_offset - previous.destination_offset ==
             previous.size &&
         // A run wider than PackedOp's 16-bit width would compile to a record
         // the packet path cannot index: stop the run at that bound, so every
         // field the schema accepts has a plan (each field is already at most
         // the same width, and two such sizes cannot overflow the sum).
         previous.size + current.size <=
             std::numeric_limits<uint16_t>::max();
}

[[gnu::always_inline]] inline std::byte *Base(const DestinationView &view,
                                              BufferKind kind) noexcept {
  return kind == BufferKind::kPacket ? view.packet.data()
                                     : view.metadata.data();
}

// One placement with the width known at compile time (Size 0: at run time).
template <size_t Size>
[[gnu::always_inline]] inline void PlaceOne(const PackedOp &op,
                                            const std::byte *mask,
                                            ConstBytes value,
                                            const DestinationView &view) noexcept {
  std::byte *dst = Base(view, static_cast<BufferKind>(op.destination)) +
                   op.destination_offset;
  const std::byte *src = value.data() + op.value_offset;
  const size_t width = Size != 0 ? Size : op.size;
  if (op.masked != 0) {
    CopyMasked(dst, src, width, mask);
  } else {
    CopyBytes(dst, src, width);
  }
}

// Dispatch by size class so the copy below sees a constant width; a runtime
// width would become a memcpy call per packet.
[[gnu::always_inline]] inline void Place(const PackedOp &op,
                                         const std::byte *masks,
                                         ConstBytes value,
                                         const DestinationView &view) noexcept {
  const std::byte *mask =
      op.masked != 0 ? masks + op.mask_offset : nullptr;
  switch (SizeClass(op.size)) {
    case 0:
      PlaceOne<1>(op, mask, value, view);
      break;
    case 1:
      PlaceOne<2>(op, mask, value, view);
      break;
    case 2:
      PlaceOne<4>(op, mask, value, view);
      break;
    case 3:
      PlaceOne<8>(op, mask, value, view);
      break;
    case 4:
      PlaceOne<16>(op, mask, value, view);
      break;
    case 5:
      PlaceOne<32>(op, mask, value, view);
      break;
    default:
      PlaceOne<0>(op, mask, value, view);
      break;
  }
}

template <size_t Size>
[[gnu::always_inline]] inline void ScatterBucket(
    const PackedOp *ops, uint32_t count, BufferKind destination,
    ConstBytes value, const DestinationView &view) noexcept {
  std::byte *base = Base(view, destination);
  for (uint32_t j = 0; j < count; j++) {
    const PackedOp &op = ops[j];
    const std::byte *src = value.data() + op.value_offset;
    std::byte *dst = base + op.destination_offset;
    if constexpr (Size != 0) {
      CopyBytes(dst, src, Size);
    } else {
      CopyBytes(dst, src, op.size);
    }
  }
}

}  // namespace

ClassifierResult<ResultPlan> ResultPlan::Compile(
    const RuntimeClassifierSchema &schema) {
  ClassifierResult<void> valid = schema.Validate();
  if (!valid) {
    return std::unexpected(valid.error());
  }

  std::vector<CoalescedOp> ops;
  ops.reserve(schema.result_fields.size());
  for (const RuntimeResultField &field : schema.result_fields) {
    ops.push_back(CoalescedOp{field.destination, field.value_offset,
                              field.destination_offset, field.size,
                              field.normalization.mask});
  }
  std::stable_sort(ops.begin(), ops.end(),
                   [](const CoalescedOp &lhs, const CoalescedOp &rhs) {
                     if (lhs.destination != rhs.destination) {
                       return lhs.destination < rhs.destination;
                     }
                     return lhs.destination_offset < rhs.destination_offset;
                   });

  std::vector<CoalescedOp> coalesced;
  coalesced.reserve(ops.size());
  for (const CoalescedOp &op : ops) {
    if (!coalesced.empty() && CanCoalesce(coalesced.back(), op)) {
      coalesced.back().size += op.size;
      continue;
    }
    coalesced.push_back(op);
  }

  std::vector<PackedOp> packed;
  std::vector<std::byte> masks;
  packed.reserve(coalesced.size());
  size_t required_packet = 0;
  size_t required_metadata = 0;
  for (const CoalescedOp &op : coalesced) {
    if (op.value_offset > std::numeric_limits<uint32_t>::max() ||
        op.destination_offset > std::numeric_limits<uint32_t>::max() ||
        op.size > std::numeric_limits<uint16_t>::max()) {
      return std::unexpected(ClassifierError{
          ClassifierErrorCode::kInvalidPlan,
          "classifier result plan exceeds the packed 32-bit offsets", std::nullopt});
    }
    PackedOp out;
    out.value_offset = static_cast<uint32_t>(op.value_offset);
    out.destination_offset = static_cast<uint32_t>(op.destination_offset);
    out.size = static_cast<uint16_t>(op.size);
    out.destination = static_cast<uint8_t>(op.destination);
    out.masked = op.mask.empty() ? 0 : 1;
    if (!op.mask.empty()) {
      out.mask_offset = static_cast<uint32_t>(masks.size());
      masks.insert(masks.end(), op.mask.begin(), op.mask.end());
    }
    packed.push_back(out);

    const size_t end = op.destination_offset + op.size;
    if (op.destination == BufferKind::kPacket) {
      required_packet = std::max(required_packet, end);
    } else {
      required_metadata = std::max(required_metadata, end);
    }
  }

  std::stable_sort(packed.begin(), packed.end(),
                   [](const PackedOp &lhs, const PackedOp &rhs) {
                     return Rank(lhs) < Rank(rhs);
                   });

  std::array<Bucket, 28> buckets{};
  size_t bucket_count = 0;
  for (size_t i = 0; i < packed.size();) {
    const size_t rank = Rank(packed[i]);
    size_t end = i + 1;
    while (end < packed.size() && Rank(packed[end]) == rank) {
      end++;
    }
    buckets[bucket_count++] =
        Bucket{static_cast<uint32_t>(i), static_cast<uint32_t>(end),
               packed[i].destination,
               static_cast<uint8_t>(SizeClass(packed[i].size)),
               packed[i].masked != 0};
    i = end;
  }

  ResultBatchFn kernel = &ResultPlan::ApplyGeneric;
  ResultKernel kind = ResultKernel::kGeneric;
  if (packed.size() == 1) {
    kernel = SelectSingle(packed[0]);
    kind = ResultKernel::kSingle;
  } else if (packed.size() == 2) {
    kernel = &ResultPlan::ApplyTwoOps;
    kind = ResultKernel::kTwoOp;
  } else if (packed.size() == 3) {
    kernel = &ResultPlan::ApplyNOps<3>;
  } else if (packed.size() == 4) {
    kernel = &ResultPlan::ApplyNOps<4>;
  } else if (packed.size() == 5) {
    kernel = &ResultPlan::ApplyNOps<5>;
  } else if (packed.size() == 6) {
    kernel = &ResultPlan::ApplyNOps<6>;
  } else if (packed.size() == 7) {
    kernel = &ResultPlan::ApplyNOps<7>;
  } else if (packed.size() == 8) {
    kernel = &ResultPlan::ApplyNOps<8>;
  }

  return ResultPlan(schema.value_size, std::move(packed), std::move(masks),
                    buckets, bucket_count, kernel, kind, required_packet,
                    required_metadata);
}

[[gnu::always_inline]] inline bool ResultPlan::Fits(
    ConstBytes value, const DestinationView &destination) const noexcept {
  return value.size() >= value_size_ &&
         destination.packet.size() >= required_packet_bytes_ &&
         destination.metadata.size() >= required_metadata_bytes_;
}

bool ResultPlan::Apply(ConstBytes value,
                       const DestinationView &destination) const noexcept {
  return kernel_(*this, std::span<const ConstBytes>(&value, 1),
                 std::span<const DestinationView>(&destination, 1));
}

bool ResultPlan::ApplyBatch(
    std::span<const ConstBytes> values,
    std::span<const DestinationView> destinations) const noexcept {
  promise(values.size() == destinations.size());
  return kernel_(*this, values, destinations);
}

ResultBatchFn ResultPlan::SelectSingle(const PackedOp &op) noexcept {
  const bool masked = op.masked != 0;
  switch (SizeClass(op.size)) {
    case 0:
      return masked ? &ApplySingleSized<1, true> : &ApplySingleSized<1, false>;
    case 1:
      return masked ? &ApplySingleSized<2, true> : &ApplySingleSized<2, false>;
    case 2:
      return masked ? &ApplySingleSized<4, true> : &ApplySingleSized<4, false>;
    case 3:
      return masked ? &ApplySingleSized<8, true> : &ApplySingleSized<8, false>;
    case 4:
      return masked ? &ApplySingleSized<16, true> : &ApplySingleSized<16, false>;
    case 5:
      return masked ? &ApplySingleSized<32, true> : &ApplySingleSized<32, false>;
    default:
      return masked ? &ApplySingleSized<0, true> : &ApplySingleSized<0, false>;
  }
}

template <size_t Size, bool Masked>
bool ResultPlan::ApplySingleSized(
    const ResultPlan &plan, std::span<const ConstBytes> values,
    std::span<const DestinationView> destinations) noexcept {
  const PackedOp &op = plan.ops_[0];
  // Loop invariants: the destination buffer, the offsets and the required
  // length are the same for every record. Reading them out of the op per item
  // is what made this path several times the direct copy.
  const bool to_packet =
      static_cast<BufferKind>(op.destination) == BufferKind::kPacket;
  const size_t src_off = op.value_offset;
  const size_t dst_off = op.destination_offset;
  const size_t value_size = plan.value_size_;
  const size_t need =
      to_packet ? plan.required_packet_bytes_ : plan.required_metadata_bytes_;
  [[maybe_unused]] const std::byte *mask =
      plan.masks_.data() + op.mask_offset;
  const ConstBytes *records = values.data();
  const DestinationView *views = destinations.data();
  const size_t count = values.size();
  for (size_t i = 0; i < count; i++) {
    if (records[i].size() < value_size) {
      return false;
    }
    std::byte *base;
    if (to_packet) {
      if (views[i].packet.size() < need) {
        return false;
      }
      base = views[i].packet.data();
    } else {
      if (views[i].metadata.size() < need) {
        return false;
      }
      base = views[i].metadata.data();
    }
    const std::byte *src = records[i].data() + src_off;
    std::byte *dst = base + dst_off;
    if constexpr (Size != 0) {
      if constexpr (Masked) {
        CopyMasked(dst, src, Size, mask);
      } else {
        CopyBytes(dst, src, Size);
      }
    } else {
      if constexpr (Masked) {
        CopyMasked(dst, src, op.size, mask);
      } else {
        CopyBytes(dst, src, op.size);
      }
    }
  }
  return true;
}

bool ResultPlan::ApplyTwoOps(const ResultPlan &plan,
                             std::span<const ConstBytes> values,
                             std::span<const DestinationView> destinations) noexcept {
  const PackedOp *ops = plan.ops_.data();
  const std::byte *masks = plan.masks_.data();
  for (size_t i = 0; i < values.size(); i++) {
    if (!plan.Fits(values[i], destinations[i])) {
      return false;
    }
    Place(ops[0], masks, values[i], destinations[i]);
    Place(ops[1], masks, values[i], destinations[i]);
  }
  return true;
}

template <size_t N>
bool ResultPlan::ApplyNOps(const ResultPlan &plan,
                           std::span<const ConstBytes> values,
                           std::span<const DestinationView> destinations) noexcept {
  const PackedOp *ops = plan.ops_.data();
  const std::byte *masks = plan.masks_.data();
  const size_t count = values.size();
  for (size_t i = 0; i < count; i++) {
    if (!plan.Fits(values[i], destinations[i])) {
      return false;
    }
    [&]<size_t... Is>(std::index_sequence<Is...>) {
      (Place(ops[Is], masks, values[i], destinations[i]), ...);
    }(std::make_index_sequence<N>{});
  }
  return true;
}

bool ResultPlan::ApplyGeneric(const ResultPlan &plan,
                              std::span<const ConstBytes> values,
                              std::span<const DestinationView> destinations) noexcept {
  const std::byte *masks = plan.masks_.data();
  for (size_t i = 0; i < values.size(); i++) {
    if (!plan.Fits(values[i], destinations[i])) {
      return false;
    }
    const DestinationView &view = destinations[i];
    const ConstBytes value = values[i];
    for (size_t b = 0; b < plan.bucket_count_; b++) {
      const Bucket &bucket = plan.buckets_[b];
      const PackedOp *begin = plan.ops_.data() + bucket.begin;
      const uint32_t count = bucket.end - bucket.begin;
      if (bucket.masked) {
        for (uint32_t j = 0; j < count; j++) {
          Place(begin[j], masks, value, view);
        }
        continue;
      }
      switch (bucket.size_class) {
        case 0:
          ScatterBucket<1>(begin, count,
                           static_cast<BufferKind>(bucket.destination),
                           value, view);
          break;
        case 1:
          ScatterBucket<2>(begin, count,
                           static_cast<BufferKind>(bucket.destination),
                           value, view);
          break;
        case 2:
          ScatterBucket<4>(begin, count,
                           static_cast<BufferKind>(bucket.destination),
                           value, view);
          break;
        case 3:
          ScatterBucket<8>(begin, count,
                           static_cast<BufferKind>(bucket.destination),
                           value, view);
          break;
        case 4:
          ScatterBucket<16>(begin, count,
                            static_cast<BufferKind>(bucket.destination),
                            value, view);
          break;
        case 5:
          ScatterBucket<32>(begin, count,
                            static_cast<BufferKind>(bucket.destination),
                            value, view);
          break;
        default:
          ScatterBucket<0>(begin, count,
                           static_cast<BufferKind>(bucket.destination),
                           value, view);
          break;
      }
    }
  }
  return true;
}

}  // namespace bess::classifier
