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

#include "packet_reshape.h"

#include <cstring>
#include <limits>

#include "packet_mutation.h"

namespace bess::packet {
namespace detail {

struct ChainInfo {
  uint16_t nb_segs;
  uint32_t pkt_len;
};

std::expected<ChainInfo, ReshapeError> ValidateChain(
    ::bess::PacketHandle packet) noexcept {
  if (packet == nullptr) {
    return std::unexpected(ReshapeError::kNullPacket);
  }
  if (packet->nb_segs == 0) {
    return std::unexpected(ReshapeError::kMalformedChain);
  }

  uint64_t logical_length = 0;
  ::bess::PacketHandle segment = packet;
  for (uint16_t index = 0; index < packet->nb_segs; index++) {
    if (segment == nullptr || segment->pool == nullptr ||
        segment->buf_addr == nullptr || segment->data_off > segment->buf_len ||
        segment->data_len > segment->buf_len - segment->data_off) {
      return std::unexpected(ReshapeError::kMalformedChain);
    }
    logical_length += segment->data_len;
    if (logical_length > std::numeric_limits<uint32_t>::max()) {
      return std::unexpected(ReshapeError::kMalformedChain);
    }
    segment = segment->next;
  }

  if (segment != nullptr || logical_length != packet->pkt_len) {
    return std::unexpected(ReshapeError::kMalformedChain);
  }
  return ChainInfo{packet->nb_segs, packet->pkt_len};
}

std::expected<bool, ReshapeError> ChainPayloadWritable(
    ::bess::PacketHandle packet) noexcept {
  const auto chain = ValidateChain(packet);
  if (!chain) {
    return std::unexpected(chain.error());
  }

  bool writable = true;
  ::bess::PacketHandle segment = packet;
  for (uint16_t index = 0; index < chain->nb_segs; index++) {
    if (segment->data_len != 0 &&
        PayloadWriteabilityOf(::bess::PacketRef(segment)) !=
            PayloadWriteability::kWritable) {
      writable = false;
    }
    segment = segment->next;
  }
  return writable;
}

void CopyBessPacketPrivate(::bess::PacketHandle destination,
                           ::bess::PacketHandle source) noexcept {
  std::memcpy(rte_mbuf_to_priv(destination), rte_mbuf_to_priv(source),
              ::bess::kPacketPrivateSize);
}

void CopyPacketHeadMetadata(::bess::PacketHandle destination,
                            ::bess::PacketHandle source) noexcept {
  __rte_pktmbuf_copy_hdr(destination, source);
  destination->timesync = source->timesync;
  destination->ol_flags =
      source->ol_flags & ~(RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL);
}
std::expected<::bess::PacketHandle, ReshapeError> CopyPacketForReplacement(
    ::bess::PacketHandle source) noexcept {
  ::bess::PacketHandle destination_head = nullptr;
  ::bess::PacketHandle destination_tail = nullptr;
  for (::bess::PacketHandle segment = source; segment != nullptr;
       segment = segment->next) {
    ::bess::PacketHandle copy = rte_pktmbuf_alloc(segment->pool);
    if (copy == nullptr) {
      if (destination_head != nullptr) {
        ::bess::PacketFree(destination_head);
      }
      return std::unexpected(ReshapeError::kAllocationFailed);
    }
    if (segment->data_off > copy->buf_len ||
        segment->data_len > copy->buf_len - segment->data_off) {
      rte_pktmbuf_free(copy);
      if (destination_head != nullptr) {
        ::bess::PacketFree(destination_head);
      }
      return std::unexpected(ReshapeError::kInsufficientContiguousCapacity);
    }

    copy->data_off = segment->data_off;
    copy->data_len = segment->data_len;
    copy->pkt_len = segment->data_len;
    if (segment->data_len != 0) {
      std::memcpy(::bess::PacketRef(copy).head_data<std::byte *>(),
                  ::bess::PacketRef(segment).head_data<const std::byte *>(),
                  segment->data_len);
    }

    if (destination_head == nullptr) {
      destination_head = copy;
      CopyPacketHeadMetadata(destination_head, source);
    } else {
      destination_tail->next = copy;
    }
    destination_tail = copy;
  }
  if (destination_head == nullptr) {
    return std::unexpected(ReshapeError::kMalformedChain);
  }
  destination_head->pkt_len = source->pkt_len;
  destination_head->nb_segs = source->nb_segs;
  return destination_head;
}

// Promotion changes logical packet-head state, not the surviving segment's
// payload representation or storage.
void PromoteLogicalHeadState(::bess::PacketHandle destination,
                             ::bess::PacketHandle source) noexcept {
  constexpr uint64_t kRepresentationFlags =
      RTE_MBUF_F_INDIRECT | RTE_MBUF_F_EXTERNAL;
  const uint64_t destination_representation_flags =
      destination->ol_flags & kRepresentationFlags;
  __rte_pktmbuf_copy_hdr(destination, source);
  destination->timesync = source->timesync;
  destination->ol_flags = (source->ol_flags & ~kRepresentationFlags) |
                          destination_representation_flags;
  CopyBessPacketPrivate(destination, source);
}

std::expected<::bess::PacketHandle, ReshapeError> CopyPacketLinear(
    ::bess::PacketHandle source) noexcept {
  const uint16_t data_room = rte_pktmbuf_data_room_size(source->pool);
  if (data_room < RTE_PKTMBUF_HEADROOM ||
      source->pkt_len > static_cast<size_t>(data_room) - RTE_PKTMBUF_HEADROOM) {
    return std::unexpected(ReshapeError::kInsufficientContiguousCapacity);
  }

  ::bess::PacketHandle replacement = rte_pktmbuf_alloc(source->pool);
  if (replacement == nullptr) {
    return std::unexpected(ReshapeError::kAllocationFailed);
  }
  if (rte_pktmbuf_tailroom(replacement) < source->pkt_len) {
    rte_pktmbuf_free(replacement);
    return std::unexpected(ReshapeError::kInsufficientContiguousCapacity);
  }

  CopyPacketHeadMetadata(replacement, source);
  size_t copied = 0;
  std::byte *destination =
      ::bess::PacketRef(replacement).head_data<std::byte *>();
  for (::bess::PacketHandle segment = source; segment != nullptr;
       segment = segment->next) {
    if (segment->data_len == 0) {
      continue;
    }
    std::memcpy(destination + copied,
                ::bess::PacketRef(segment).head_data<const std::byte *>(),
                segment->data_len);
    copied += segment->data_len;
  }

  replacement->data_len = static_cast<uint16_t>(source->pkt_len);
  replacement->pkt_len = source->pkt_len;
  replacement->nb_segs = 1;
  replacement->next = nullptr;
  CopyBessPacketPrivate(replacement, source);
  return replacement;
}

}  // namespace detail

std::expected<void, ReshapeError> EnsureWritable(
    ::bess::PacketHandle &packet) noexcept {
  const auto writable = detail::ChainPayloadWritable(packet);
  if (!writable) {
    return std::unexpected(writable.error());
  }
  if (*writable) {
    return {};
  }

  auto replacement = detail::CopyPacketForReplacement(packet);
  if (!replacement) {
    return std::unexpected(replacement.error());
  }
  detail::CopyBessPacketPrivate(*replacement, packet);

  ::bess::PacketHandle original = packet;
  packet = *replacement;
  ::bess::PacketFree(original);
  return {};
}

std::expected<void, ReshapeError> EnsureLinear(
    ::bess::PacketHandle &packet) noexcept {
  const auto chain = detail::ValidateChain(packet);
  if (!chain) {
    return std::unexpected(chain.error());
  }
  if (chain->nb_segs == 1) {
    return {};
  }

  const size_t bytes_to_append =
      static_cast<size_t>(packet->pkt_len) - packet->data_len;
  const size_t head_tailroom = static_cast<size_t>(packet->buf_len) -
                               packet->data_off - packet->data_len;
  if (PayloadWriteabilityOf(::bess::PacketRef(packet)) ==
          PayloadWriteability::kWritable &&
      bytes_to_append <= head_tailroom) {
    if (rte_pktmbuf_linearize(packet) == 0) {
      return {};
    }
  }

  const auto replacement = detail::CopyPacketLinear(packet);
  if (!replacement) {
    return std::unexpected(replacement.error());
  }

  ::bess::PacketHandle original = packet;
  packet = *replacement;
  ::bess::PacketFree(original);
  return {};
}

std::expected<MutableBytes, ReshapeError> EnsureContiguous(
    ::bess::PacketHandle &packet, size_t offset, size_t bytes) noexcept {
  const auto chain = detail::ValidateChain(packet);
  if (!chain) {
    return std::unexpected(chain.error());
  }
  if (offset > chain->pkt_len ||
      bytes > static_cast<size_t>(chain->pkt_len) - offset) {
    return std::unexpected(ReshapeError::kLengthOutOfRange);
  }
  if (bytes == 0) {
    return MutableBytes{};
  }

  size_t segment_offset = 0;
  ::bess::PacketHandle segment = packet;
  for (uint16_t index = 0; index < chain->nb_segs; index++) {
    const size_t segment_length = segment->data_len;
    if (offset >= segment_offset && offset - segment_offset < segment_length &&
        bytes <= segment_length - (offset - segment_offset)) {
      if (PayloadWriteabilityOf(::bess::PacketRef(segment)) ==
          PayloadWriteability::kWritable) {
        return MutableBytes(::bess::PacketRef(segment).head_data<std::byte *>(
                                static_cast<uint16_t>(offset - segment_offset)),
                            bytes);
      }
      break;
    }
    segment_offset += segment_length;
    segment = segment->next;
  }

  if (chain->nb_segs == 1) {
    const auto writable = EnsureWritable(packet);
    if (!writable) {
      return std::unexpected(writable.error());
    }
  } else {
    const auto linear = EnsureLinear(packet);
    if (!linear) {
      return std::unexpected(linear.error());
    }
  }
  return MutableBytes(::bess::PacketRef(packet).head_data<std::byte *>(
                          static_cast<uint16_t>(offset)),
                      bytes);
}

std::expected<void, ReshapeError> RemovePrefix(::bess::PacketHandle &packet,
                                               size_t bytes) noexcept {
  const auto chain = detail::ValidateChain(packet);
  if (!chain) {
    return std::unexpected(chain.error());
  }
  if (bytes > chain->pkt_len) {
    return std::unexpected(ReshapeError::kLengthOutOfRange);
  }
  if (bytes == 0) {
    return {};
  }
  if (bytes == chain->pkt_len) {
    ::bess::PacketHandle removed = packet->next;
    packet->next = nullptr;
    packet->data_len = 0;
    packet->pkt_len = 0;
    packet->nb_segs = 1;
    if (removed != nullptr) {
      ::bess::PacketFree(removed);
    }
    return {};
  }
  if (bytes < packet->data_len) {
    packet->data_off = static_cast<uint16_t>(packet->data_off + bytes);
    packet->data_len = static_cast<uint16_t>(packet->data_len - bytes);
    packet->pkt_len = chain->pkt_len - static_cast<uint32_t>(bytes);
    return {};
  }

  size_t remaining = bytes;
  ::bess::PacketHandle segment = packet;
  ::bess::PacketHandle last_removed = nullptr;
  ::bess::PacketHandle survivor = nullptr;
  size_t removed_segments = 0;
  size_t bytes_into_survivor = 0;
  while (segment != nullptr) {
    if (remaining < segment->data_len) {
      survivor = segment;
      bytes_into_survivor = remaining;
      break;
    }
    remaining -= segment->data_len;
    last_removed = segment;
    ++removed_segments;
    if (remaining == 0) {
      survivor = segment->next;
      break;
    }
    segment = segment->next;
  }
  if (survivor == nullptr || last_removed == nullptr ||
      removed_segments >= chain->nb_segs) {
    return std::unexpected(ReshapeError::kMalformedChain);
  }

  if (packet->priv_size < ::bess::kPacketPrivateSize ||
      survivor->priv_size < ::bess::kPacketPrivateSize) {
    return std::unexpected(ReshapeError::kMalformedChain);
  }

  detail::PromoteLogicalHeadState(survivor, packet);
  if (bytes_into_survivor != 0) {
    survivor->data_off =
        static_cast<uint16_t>(survivor->data_off + bytes_into_survivor);
    survivor->data_len =
        static_cast<uint16_t>(survivor->data_len - bytes_into_survivor);
  }
  survivor->pkt_len = chain->pkt_len - static_cast<uint32_t>(bytes);
  survivor->nb_segs = static_cast<uint16_t>(chain->nb_segs - removed_segments);

  last_removed->next = nullptr;
  ::bess::PacketHandle original = packet;
  ::bess::PacketFree(original);
  packet = survivor;
  return {};
}

std::expected<void, ReshapeError> TrimSuffix(::bess::PacketHandle &packet,
                                             size_t bytes) noexcept {
  const auto chain = detail::ValidateChain(packet);
  if (!chain) {
    return std::unexpected(chain.error());
  }
  if (bytes > chain->pkt_len) {
    return std::unexpected(ReshapeError::kLengthOutOfRange);
  }
  if (bytes == 0) {
    return {};
  }
  if (bytes == chain->pkt_len) {
    ::bess::PacketHandle removed = packet->next;
    packet->next = nullptr;
    packet->data_len = 0;
    packet->pkt_len = 0;
    packet->nb_segs = 1;
    if (removed != nullptr) {
      ::bess::PacketFree(removed);
    }
    return {};
  }

  const size_t kept_length = chain->pkt_len - static_cast<uint32_t>(bytes);
  size_t prefix_length = 0;
  size_t last_segment_length = 0;
  size_t kept_segments = 0;
  bool cut_found = false;
  ::bess::PacketHandle segment = packet;
  ::bess::PacketHandle last_kept = nullptr;
  ::bess::PacketHandle first_removed = nullptr;
  while (segment != nullptr) {
    if (kept_length == prefix_length) {
      first_removed = segment;
      cut_found = true;
      break;
    }

    const size_t segment_length = segment->data_len;
    const size_t keep_in_segment = kept_length - prefix_length;
    if (keep_in_segment > segment_length) {
      prefix_length += segment_length;
      last_segment_length = segment_length;
      last_kept = segment;
      ++kept_segments;
      segment = segment->next;
      continue;
    }

    last_segment_length = keep_in_segment;
    last_kept = segment;
    ++kept_segments;
    first_removed = segment->next;
    cut_found = true;
    break;
  }
  if (!cut_found || last_kept == nullptr || kept_segments == 0 ||
      kept_segments > chain->nb_segs) {
    return std::unexpected(ReshapeError::kMalformedChain);
  }

  if (last_segment_length < last_kept->data_len) {
    last_kept->data_len = static_cast<uint16_t>(last_segment_length);
  }
  last_kept->next = nullptr;
  packet->pkt_len = static_cast<uint32_t>(kept_length);
  packet->nb_segs = static_cast<uint16_t>(kept_segments);
  if (first_removed != nullptr) {
    ::bess::PacketFree(first_removed);
  }
  return {};
}

}  // namespace bess::packet
