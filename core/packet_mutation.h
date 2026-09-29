// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_PACKET_MUTATION_H_
#define BESS_PACKET_MUTATION_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

#include "packet.h"

namespace bess::packet {

enum class MutationError : uint8_t {
  kNullPacket,
  kLengthOutOfRange,
  kInsufficientHeadroom,
  kInsufficientTailroom,
  kCrossesSegment,
  kSharedStorage,
};

using MutableBytes = std::span<std::byte>;

enum class PayloadWriteability : uint8_t {
  kWritable,
  kShared,
};

// Ownership contract:
// The caller exclusively owns the mbuf descriptor chain being mutated.
// Payload storage itself may be shared; operations that expose writable
// payload bytes additionally enforce PayloadWriteabilityOf().

// Reports whether bytes addressed through one mbuf segment can be changed
// without changing the payload observed by another live mbuf. Descriptor
// metadata ownership is a separate concern: this predicate only covers the
// backing payload representation.
inline PayloadWriteability PayloadWriteabilityOf(
    ::bess::PacketRef segment) noexcept {
  ::rte_mbuf *mbuf = segment.handle();
  if (mbuf == nullptr) {
    return PayloadWriteability::kShared;
  }

  if (RTE_MBUF_HAS_EXTBUF(mbuf)) {
    return mbuf->shinfo != nullptr &&
                   rte_mbuf_ext_refcnt_read(mbuf->shinfo) == 1
               ? PayloadWriteability::kWritable
               : PayloadWriteability::kShared;
  }

  if (!RTE_MBUF_DIRECT(mbuf)) {
    ::rte_mbuf *direct = rte_mbuf_from_indirect(mbuf);
    return direct != nullptr && rte_mbuf_refcnt_read(direct) == 1
               ? PayloadWriteability::kWritable
               : PayloadWriteability::kShared;
  }

  return rte_mbuf_refcnt_read(mbuf) == 1 ? PayloadWriteability::kWritable
                                         : PayloadWriteability::kShared;
}

namespace detail {

inline bool MutationLengthFits(::rte_mbuf *mbuf, size_t bytes) noexcept {
  return bytes <= std::numeric_limits<uint16_t>::max() &&
         bytes <= std::numeric_limits<uint32_t>::max() - mbuf->pkt_len;
}

inline bool MutationRemovalLengthFits(::rte_mbuf *mbuf,
                                      size_t bytes) noexcept {
  return bytes <= std::numeric_limits<uint16_t>::max() &&
         bytes <= mbuf->pkt_len;
}

inline std::unexpected<MutationError> LengthOutOfRange() noexcept {
  return std::unexpected(MutationError::kLengthOutOfRange);
}

}  // namespace detail

// These operations never allocate, create or destroy segments, linearize, or
// replace the packet head. Prepend/append additionally require exclusive
// backing-payload storage because they return writable bytes.
inline std::expected<MutableBytes, MutationError> PrependInPlace(
    ::bess::PacketRef packet, size_t bytes) noexcept {
  ::rte_mbuf *mbuf = packet.handle();
  if (mbuf == nullptr) {
    return std::unexpected(MutationError::kNullPacket);
  }
  if (bytes == 0) {
    return MutableBytes{};
  }
  if (!detail::MutationLengthFits(mbuf, bytes)) {
    return detail::LengthOutOfRange();
  }
  if (bytes > rte_pktmbuf_headroom(mbuf)) {
    return std::unexpected(MutationError::kInsufficientHeadroom);
  }
  if (PayloadWriteabilityOf(packet) != PayloadWriteability::kWritable) {
    return std::unexpected(MutationError::kSharedStorage);
  }

  void *data = rte_pktmbuf_prepend(
      mbuf, static_cast<uint16_t>(bytes));
  if (data == nullptr) {
    return std::unexpected(MutationError::kInsufficientHeadroom);
  }
  return MutableBytes(reinterpret_cast<std::byte *>(data), bytes);
}

inline std::expected<MutableBytes, MutationError> AppendInPlace(
    ::bess::PacketRef packet, size_t bytes) noexcept {
  ::rte_mbuf *mbuf = packet.handle();
  if (mbuf == nullptr) {
    return std::unexpected(MutationError::kNullPacket);
  }
  if (bytes == 0) {
    return MutableBytes{};
  }
  if (!detail::MutationLengthFits(mbuf, bytes)) {
    return detail::LengthOutOfRange();
  }

  ::rte_mbuf *last = rte_pktmbuf_lastseg(mbuf);
  if (last == nullptr) {
    return std::unexpected(MutationError::kNullPacket);
  }
  if (bytes > rte_pktmbuf_tailroom(last)) {
    return std::unexpected(MutationError::kInsufficientTailroom);
  }
  if (PayloadWriteabilityOf(::bess::PacketRef(last)) !=
      PayloadWriteability::kWritable) {
    return std::unexpected(MutationError::kSharedStorage);
  }

  void *data = rte_pktmbuf_append(mbuf, static_cast<uint16_t>(bytes));
  if (data == nullptr) {
    return std::unexpected(MutationError::kInsufficientTailroom);
  }
  return MutableBytes(reinterpret_cast<std::byte *>(data), bytes);
}

inline std::expected<void, MutationError> RemovePrefixInPlace(
    ::bess::PacketRef packet, size_t bytes) noexcept {
  ::rte_mbuf *mbuf = packet.handle();
  if (mbuf == nullptr) {
    return std::unexpected(MutationError::kNullPacket);
  }
  if (!detail::MutationRemovalLengthFits(mbuf, bytes)) {
    return detail::LengthOutOfRange();
  }
  if (bytes == 0) {
    return {};
  }
  if (bytes > mbuf->data_len) {
    return std::unexpected(MutationError::kCrossesSegment);
  }
  if (rte_pktmbuf_adj(mbuf, static_cast<uint16_t>(bytes)) == nullptr) {
    return std::unexpected(MutationError::kCrossesSegment);
  }
  return {};
}

inline std::expected<void, MutationError> TrimSuffixInPlace(
    ::bess::PacketRef packet, size_t bytes) noexcept {
  ::rte_mbuf *mbuf = packet.handle();
  if (mbuf == nullptr) {
    return std::unexpected(MutationError::kNullPacket);
  }
  if (!detail::MutationRemovalLengthFits(mbuf, bytes)) {
    return detail::LengthOutOfRange();
  }
  if (bytes == 0) {
    return {};
  }

  ::rte_mbuf *last = rte_pktmbuf_lastseg(mbuf);
  if (last == nullptr || bytes > last->data_len) {
    return std::unexpected(MutationError::kCrossesSegment);
  }
  if (rte_pktmbuf_trim(mbuf, static_cast<uint16_t>(bytes)) != 0) {
    return std::unexpected(MutationError::kCrossesSegment);
  }
  return {};
}

}  // namespace bess::packet

#endif  // BESS_PACKET_MUTATION_H_
