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
