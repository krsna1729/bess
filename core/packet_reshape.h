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
  kInsufficientHeadroom,
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

// WritableHead's out-of-line part: everything its inline test does not
// decide.
std::expected<MutableBytes, ReshapeError> WritableHeadSlow(
    ::bess::PacketHandle &packet, size_t required) noexcept;

// PrependWritable's out-of-line part: everything its inline test does not
// decide.
std::expected<MutableBytes, ReshapeError> PrependWritableSlow(
    ::bess::PacketHandle &packet, size_t bytes) noexcept;

}  // namespace detail

// True when `packet`'s first segment holds at least `required` bytes and is
// a direct mbuf held once: the common case WritableHead decides inline, with
// no branch, so a batch can be tested in one straight pass (one length
// compare, one flag test and one reference-count compare per packet). False
// does not mean the packet cannot be written; WritableHead decides the rest.
inline bool HeadWritableInPlace(::bess::PacketRef packet,
                                size_t required) noexcept {
  const ::rte_mbuf *m = packet.handle();
  return static_cast<bool>(
      static_cast<unsigned>(m->data_len >= required) &
      static_cast<unsigned>(RTE_MBUF_DIRECT(m)) &  // NOLINT(bugprone-signed-bitwise)
      static_cast<unsigned>(rte_mbuf_refcnt_read(m) == 1));
}

// The checked in-place header write (D-121, D-153, D-172): `packet`'s first
// segment as writable bytes, all of them (at least `required`), for a module
// that rewrites bytes at fixed offsets of the frame. Only the first segment
// counts: its bytes must reach `required`. A payload this packet holds alone
// is written as it is. A shared one (a clone, an indirect mbuf or an external
// buffer another mbuf holds) is copied first with EnsureWritable; the copy
// replaces `packet` and may resegment it, so the length is checked again on
// the copy. Only the first segment's sharing is tested, as only it is
// written.
//
// On failure the caller drops the packet `packet` names now (the original or
// its copy), never forwards it partly written or unwritten:
//  - kLengthOutOfRange: the first segment, or the copy's, is shorter than
//    `required`;
//  - EnsureWritable's errors: the copy failed and `packet` is unchanged.
//
// `packet` must not be null (a batch's packets never are). The common case,
// HeadWritableInPlace, is inline; the rest is out of line.
inline std::expected<MutableBytes, ReshapeError> WritableHead(
    ::bess::PacketHandle &packet, size_t required) noexcept {
  if (likely(HeadWritableInPlace(::bess::PacketRef(packet), required))) {
    return MutableBytes(rte_pktmbuf_mtod(packet, std::byte *),
                        packet->data_len);
  }
  // Only this branch takes a handle's address, so the caller's handle stays
  // in a register on the common path.
  ::bess::PacketHandle slow = packet;
  auto head = detail::WritableHeadSlow(slow, required);
  packet = slow;
  return head;
}

// True when `packet`'s first segment has at least `bytes` of headroom and is
// a direct mbuf held once: the common case PrependWritable decides inline,
// with no branch (one compare, one flag test and one reference-count compare
// per packet). False does not mean the packet cannot take the bytes;
// PrependWritable decides the rest.
inline bool PrependableInPlace(::bess::PacketRef packet,
                               size_t bytes) noexcept {
  const ::rte_mbuf *m = packet.handle();
  return static_cast<bool>(
      static_cast<unsigned>(rte_pktmbuf_headroom(m) >= bytes) &
      static_cast<unsigned>(RTE_MBUF_DIRECT(m)) &  // NOLINT(bugprone-signed-bitwise)
      static_cast<unsigned>(rte_mbuf_refcnt_read(m) == 1));
}

// The checked prepend (D-153, D-172): `bytes` new bytes in front of
// `packet`'s data, writable, for a module that prepends a header. The bytes
// come from the first segment's headroom. Headroom this packet holds alone
// is used as it is. The headroom of a shared payload (a clone, an indirect
// mbuf or an external buffer another mbuf holds) is the other holders'
// buffer too, so that payload is copied first with EnsureWritable; the copy
// replaces `packet`, and the bytes come from the copy's headroom.
//
// On failure the caller drops the packet `packet` names now (the original or
// its copy), never forwards it without its header:
//  - kInsufficientHeadroom: the first segment, or the copy's, has less than
//    `bytes` of headroom (a shared packet without the headroom is not
//    copied);
//  - kLengthOutOfRange: `bytes` would take the packet past its length
//    fields (PrependInPlace's limits);
//  - EnsureWritable's errors: the copy failed and `packet` is unchanged.
//
// The packet's length is never at risk on the inline path: a chain holds at
// most 65,535 segments of at most 65,535 bytes, so pkt_len plus a headroom
// (at most 65,535) fits its 32 bits. `packet` must not be null (a batch's
// packets never are). The common case, PrependableInPlace, is inline; the
// rest is out of line.
inline std::expected<MutableBytes, ReshapeError> PrependWritable(
    ::bess::PacketHandle &packet, size_t bytes) noexcept {
  if (likely(PrependableInPlace(::bess::PacketRef(packet), bytes))) {
    const auto n = static_cast<uint16_t>(bytes);
    packet->data_off = static_cast<uint16_t>(packet->data_off - n);
    packet->data_len = static_cast<uint16_t>(packet->data_len + n);
    packet->pkt_len += n;
    return MutableBytes(rte_pktmbuf_mtod(packet, std::byte *), bytes);
  }
  // Only this branch takes a handle's address, so the caller's handle stays
  // in a register on the common path.
  ::bess::PacketHandle slow = packet;
  auto prepended = detail::PrependWritableSlow(slow, bytes);
  packet = slow;
  return prepended;
}

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
