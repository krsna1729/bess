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

#ifndef BESS_PACKET_RESHAPE_H_
#define BESS_PACKET_RESHAPE_H_

#include <cstddef>
#include <cstdint>
#include <expected>

#include "packet_mutation.h"

namespace bess::packet {

enum class ReshapeError : uint8_t {
  kNullPacket,
  kLengthOutOfRange,
  kAllocationFailed,
  kInsufficientContiguousCapacity,
  kMalformedChain,
};

// Ensures packet payload storage is writable with a semantic deep copy. The
// caller must exclusively own the descriptor chain; packet may be replaced on
// success. Copying may change segment topology.
std::expected<void, ReshapeError> EnsureWritable(
    ::bess::PacketHandle &packet) noexcept;

namespace detail {

// Internal COW variant for operations whose offsets depend on the existing
// segment topology. Fails unchanged if a source segment cannot fit in one
// direct mbuf.
std::expected<void, ReshapeError> EnsureWritablePreservingTopology(
    ::bess::PacketHandle &packet) noexcept;

}  // namespace detail

// Ensures that packet has one segment. The caller must exclusively own the
// descriptor chain; payload backing may remain shared when packet is already
// linear.
std::expected<void, ReshapeError> EnsureLinear(
    ::bess::PacketHandle &packet) noexcept;

// Ensures that [offset, offset + bytes) is contiguous and writable. A zero
// length range is valid at any offset through pkt_len and never changes packet.
std::expected<MutableBytes, ReshapeError> EnsureContiguous(
    ::bess::PacketHandle &packet, size_t offset, size_t bytes) noexcept;

// Removes a prefix across the descriptor chain without copying payload bytes
// or requiring exclusive payload backing. The caller must exclusively own
// the descriptor chain. Removing the full packet retains its existing head
// as a valid zero-length, single-segment packet.
std::expected<void, ReshapeError> RemovePrefix(::bess::PacketHandle &packet,
                                               size_t bytes) noexcept;

// Trims a suffix across the descriptor chain without copying payload bytes or
// requiring exclusive payload backing. The caller must exclusively own the
// descriptor chain. Removing the full packet retains its existing head as a
// valid zero-length, single-segment packet.
std::expected<void, ReshapeError> TrimSuffix(::bess::PacketHandle &packet,
                                             size_t bytes) noexcept;

}  // namespace bess::packet

#endif  // BESS_PACKET_RESHAPE_H_
