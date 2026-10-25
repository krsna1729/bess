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

// WritableHead's and PrependWritable's result before it becomes a
// std::expected: the bytes when `ok`, else the error. The inline fast path
// and the out-of-line slow path each give one, in registers (a std::expected
// of a span is returned through memory), and the two meet as these scalars,
// so the caller's use of the bytes on the fast path stays in registers.
struct BytesResult {
  std::byte *data;
  uint32_t size;
  ReshapeError error;
  bool ok;
};

inline std::expected<MutableBytes, ReshapeError> ToExpected(BytesResult r) noexcept {
  if (!r.ok) {
    return std::unexpected(r.error);
  }
  return MutableBytes(r.data, r.size);
}

// WritableHead's out-of-line part: everything its inline test does not
// decide.
BytesResult WritableHeadSlow(::bess::PacketHandle &packet,
                             size_t required) noexcept;

// PrependWritable's out-of-line part: everything its inline test does not
// decide.
BytesResult PrependWritableSlow(::bess::PacketHandle &packet,
                                size_t bytes) noexcept;

}  // namespace detail

// True when `packet`'s first segment holds at least `required` bytes and is
// a direct mbuf held once: the common case WritableHead decides inline. Three
// compares, each its own branch: in a per-packet loop that is fewer
// instructions than combining the three results, and each branch is
// predicted. False does not mean the packet cannot be written; WritableHead
// decides the rest.
inline bool HeadWritableInPlace(::bess::PacketRef packet,
                                size_t required) noexcept {
  const ::rte_mbuf *m = packet.handle();
  return m->data_len >= required &&
         RTE_MBUF_DIRECT(m) &&  // NOLINT(bugprone-signed-bitwise)
         rte_mbuf_refcnt_read(m) == 1;
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
  detail::BytesResult head;
  if (HeadWritableInPlace(::bess::PacketRef(packet), required)) [[likely]] {
    head = {rte_pktmbuf_mtod(packet, std::byte *), packet->data_len, ReshapeError{}, true};
  } else {
    // Only this branch takes a handle's address, so the caller's handle
    // stays in a register on the common path.
    ::bess::PacketHandle slow = packet;
    head = detail::WritableHeadSlow(slow, required);
    packet = slow;
  }
  return detail::ToExpected(head);
}

// True when `packet`'s first segment has at least `bytes` of headroom and is
// a direct mbuf held once: the common case PrependWritable decides inline,
// with three compares (as HeadWritableInPlace's). False does not mean the
// packet cannot take the bytes; PrependWritable decides the rest.
inline bool PrependableInPlace(::bess::PacketRef packet,
                               size_t bytes) noexcept {
  const ::rte_mbuf *m = packet.handle();
  return rte_pktmbuf_headroom(m) >= bytes &&
         RTE_MBUF_DIRECT(m) &&  // NOLINT(bugprone-signed-bitwise)
         rte_mbuf_refcnt_read(m) == 1;
}

// The checked prepend (D-153, D-172): `bytes` new bytes in front of
// `packet`'s data, writable, for a module that prepends a header. The bytes
// come from the first segment's headroom. Headroom this packet holds alone
// is used as it is. The headroom of a shared first segment (a clone, an
// indirect mbuf or an external buffer another mbuf holds) is the other
// holders' buffer too, so the whole packet is copied first, even when that
// segment is empty; the copy replaces `packet`, and the bytes come from the
// copy's headroom.
//
// On failure the caller drops the packet `packet` names now (the original or
// its copy), never forwards it without its header:
//  - kInsufficientHeadroom: the first segment, or the copy's, has less than
//    `bytes` of headroom (a shared packet without the headroom is not
//    copied);
//  - kLengthOutOfRange: `bytes` would take the packet past its length
//    fields (PrependInPlace's limits);
//  - kMalformedChain: the chain's segments disagree with its head, and
//    nothing is copied;
//  - kAllocationFailed: the copy failed and `packet` is unchanged.
//
// The packet's length is never at risk on the inline path: a chain holds at
// most 65,535 segments of at most 65,535 bytes, so pkt_len plus a headroom
// (at most 65,535) fits its 32 bits. `packet` must not be null (a batch's
// packets never are). The common case, PrependableInPlace, is inline; the
// rest is out of line.
inline std::expected<MutableBytes, ReshapeError> PrependWritable(
    ::bess::PacketHandle &packet, size_t bytes) noexcept {
  ::rte_mbuf *m = packet;
  detail::BytesResult prepended;
  if (PrependableInPlace(::bess::PacketRef(m), bytes)) [[likely]] {
    // The headroom bounds `bytes` by 65,535.
    const auto n = static_cast<uint16_t>(bytes);
    m->data_off = static_cast<uint16_t>(m->data_off - n);
    m->data_len = static_cast<uint16_t>(m->data_len + n);
    m->pkt_len += n;
    prepended = {rte_pktmbuf_mtod(m, std::byte *), n, ReshapeError{}, true};
  } else {
    // Only this branch takes a handle's address, so the caller's handle
    // stays in a register on the common path.
    ::bess::PacketHandle slow = packet;
    prepended = detail::PrependWritableSlow(slow, bytes);
    packet = slow;
  }
  return detail::ToExpected(prepended);
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
