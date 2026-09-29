// SPDX-License-Identifier: BSD-3-Clause

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
