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

std::expected<bool, ReshapeError> ChainPayloadWritable(
    ::bess::PacketHandle packet) noexcept {
  if (packet == nullptr) {
    return std::unexpected(ReshapeError::kNullPacket);
  }
  if (packet->nb_segs == 0) {
    return std::unexpected(ReshapeError::kMalformedChain);
  }

  bool writable = true;
  uint64_t logical_length = 0;
  ::bess::PacketHandle segment = packet;
  for (uint16_t index = 0; index < packet->nb_segs; index++) {
    if (segment == nullptr) {
      return std::unexpected(ReshapeError::kMalformedChain);
    }
    if (segment->pool == nullptr ||
        segment->data_off > segment->buf_len ||
        segment->data_len > segment->buf_len - segment->data_off) {
      return std::unexpected(ReshapeError::kMalformedChain);
    }
    logical_length += segment->data_len;
    if (logical_length > std::numeric_limits<uint32_t>::max()) {
      return std::unexpected(ReshapeError::kMalformedChain);
    }
    if (segment->data_len != 0 &&
        PayloadWriteabilityOf(::bess::PacketRef(segment)) !=
            PayloadWriteability::kWritable) {
      writable = false;
    }
    segment = segment->next;
  }

  if (segment != nullptr || logical_length != packet->pkt_len) {
    return std::unexpected(ReshapeError::kMalformedChain);
  }
  return writable;
}

::bess::PacketHandle CopyPacketForReplacement(
    ::bess::PacketHandle source) noexcept {
  return ::bess::PacketCopy(source);
}

void CopyBessPacketPrivate(::bess::PacketHandle destination,
                           ::bess::PacketHandle source) noexcept {
  std::memcpy(rte_mbuf_to_priv(destination), rte_mbuf_to_priv(source),
              ::bess::kPacketPrivateSize);
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

  ::bess::PacketHandle replacement =
      detail::CopyPacketForReplacement(packet);
  if (replacement == nullptr) {
    return std::unexpected(ReshapeError::kAllocationFailed);
  }
  detail::CopyBessPacketPrivate(replacement, packet);

  ::bess::PacketHandle original = packet;
  packet = replacement;
  ::bess::PacketFree(original);
  return {};
}

}  // namespace bess::packet
