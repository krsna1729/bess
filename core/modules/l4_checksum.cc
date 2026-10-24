// Copyright (c) 2017, The Regents of the University of California.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "l4_checksum.h"

#include <cstdint>
#include <cstring>
#include <span>

#include "packet_checksum.h"
#include "packet_parse.h"
#include "packet_reshape.h"
#include "utils/checksum.h"
#include "utils/ip.h"
#include "utils/tcp.h"
#include "utils/udp.h"

namespace {

enum : gate_idx_t { kForwardGate = 0, kFailGate = 1 };

using bess::packet::ParseStatus;

constexpr uint8_t kTcp = 6;
constexpr uint8_t kUdp = 17;
constexpr size_t kUdpChecksumOffset = 6;
constexpr size_t kTcpChecksumOffset = 16;

// A TCP or UDP datagram over IPv4 whose checksum the module handles.
struct Datagram {
  size_t l3 = 0;
  size_t l4 = 0;
  size_t l4_length = 0;  // L4 header + payload, as the IP header states
  // The bytes the checksum covers: the UDP length for UDP (RFC 768; the
  // parse checked it is 8 to l4_length), l4_length for TCP.
  size_t covered = 0;
  bool udp = false;
};

enum class Verdict : uint8_t { kChecksum, kPass, kMalformed };

// The checked parse's verdict on `frame` (a packet of `total_len` bytes):
// CheckFastIpv4 for the common frame (no ParsedFrame to fill), ParseFrame
// otherwise. kChecksum fills `d`: an unfragmented IPv4 TCP or UDP datagram.
Verdict Classify(std::span<const uint8_t> frame, size_t total_len, Datagram &d) {
  ParseStatus status;
  uint8_t protocol;
  bool first_fragment;
  const bess::packet::FastIpv4 fast = bess::packet::CheckFastIpv4(frame, total_len);
  if (likely(fast.taken)) {
    status = fast.status;
    protocol = fast.protocol;
    first_fragment = fast.first_fragment;
    d.l3 = 14;
    d.l4 = bess::packet::kFastIpv4L4;
    d.l4_length = size_t{fast.total} - 20;
  } else {
    bess::packet::ParsedFrame parsed;
    status = bess::packet::ParseFrame(frame, parsed, total_len);
    if (parsed.l3 != bess::packet::L3Kind::kIpv4 && status != ParseStatus::kMalformed) {
      return Verdict::kPass;
    }
    protocol = parsed.protocol;
    first_fragment = parsed.first_fragment;
    d.l3 = parsed.l3_offset;
    d.l4 = parsed.l4_offset;
    d.l4_length = parsed.l4_length;
  }
  if (unlikely(status == ParseStatus::kMalformed)) {
    return Verdict::kMalformed;
  }
  if (status != ParseStatus::kOk || (protocol != kTcp && protocol != kUdp) || first_fragment) {
    return Verdict::kPass;
  }
  d.udp = protocol == kUdp;
  // The UDP header is in the first segment: the parse checked it.
  d.covered = d.udp ? (size_t{frame[d.l4 + 4]} << 8u | frame[d.l4 + 5]) : d.l4_length;
  return Verdict::kChecksum;
}

// The checksum `d` should carry, as stored on the wire, summed over the
// first segment `bytes` that holds the whole datagram.
uint16_t Sum(const uint8_t *bytes, const Datagram &d) {
  const auto *ip = reinterpret_cast<const bess::utils::Ipv4 *>(bytes + d.l3);
  const auto len = static_cast<uint16_t>(d.covered);
  if (d.udp) {
    return bess::utils::CalculateIpv4UdpChecksum(
        *reinterpret_cast<const bess::utils::Udp *>(bytes + d.l4), ip->src, ip->dst, len);
  }
  return bess::utils::CalculateIpv4TcpChecksum(
      *reinterpret_cast<const bess::utils::Tcp *>(bytes + d.l4), ip->src, ip->dst, len);
}

// Whether the checksum `d` carries is right, summed over the first segment
// `bytes` with the stored field, as RFC 1071 verifies: a checksum stored as
// 0xffff where 0x0000 was computed (-0, RFC 1624) is right too, and a UDP
// checksum of 0 is "none".
bool Verify(const uint8_t *bytes, const Datagram &d) {
  const auto *ip = reinterpret_cast<const bess::utils::Ipv4 *>(bytes + d.l3);
  const auto len = static_cast<uint16_t>(d.covered);
  if (d.udp) {
    return bess::utils::VerifyIpv4UdpChecksum(
        *reinterpret_cast<const bess::utils::Udp *>(bytes + d.l4), ip->src, ip->dst, len);
  }
  return bess::utils::VerifyIpv4TcpChecksum(
      *reinterpret_cast<const bess::utils::Tcp *>(bytes + d.l4), ip->src, ip->dst, len);
}

}  // namespace

// Where the headers are comes from the checked parse (packet_parse.h) on the
// first segment, its lengths against the packet:
//  - TCP and UDP over IPv4, untagged or behind up to two VLAN tags, with IP
//    options: the transport checksum is recomputed, or with `verify` checked
//    (gate 0 when right, gate 1 when wrong). TCP's covers the datagram the
//    IP header states; UDP's the UDP length (RFC 768: IP payload past it is
//    trailing data, as Linux treats it). A UDP checksum of 0 means "none":
//    verify passes it; a recomputed one of 0 is written as 0xffff. A stored
//    -0 (0xffff for a computed 0x0000) is right.
//  - Every other frame passes unchanged on gate 0: not IPv4 (IPv6 included),
//    another IP protocol, or a fragment (a non-first one has no L4 header, a
//    first one not the whole datagram to sum).
//  - A frame the parse calls malformed (headers that do not fit the first
//    segment, lengths that lie, a UDP length under 8 or past the IP payload)
//    is dropped, never read past or partly written (D-121, D-172).
// A datagram wholly in the first segment (the common case) is summed there
// with the checksum kernels (utils/checksum.h) and written through
// WritableHead; one a chain splits is summed and written by packet_checksum.h,
// which walks the segments. Either way a shared payload is copied before the
// write (D-153), and a frame whose copy fails is dropped. packet_checksum.h
// sums UDP only when its length is the IP payload's: a chain that splits a
// UDP datagram shorter than its IP payload passes unchanged on gate 0,
// neither summed nor verified.
void L4Checksum::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  for (bess::PacketRef pkt : batch->packets()) {
    const std::span<const uint8_t> frame(pkt.head_data<const uint8_t *>(), pkt.head_len());
    Datagram d;
    const Verdict verdict = Classify(frame, pkt.total_len(), d);
    if (verdict == Verdict::kPass) {
      EmitPacket(ctx, pkt, kForwardGate);
      continue;
    }
    if (unlikely(verdict == Verdict::kMalformed)) {
      DropPacket(ctx, pkt);
      continue;
    }

    const size_t field = d.l4 + (d.udp ? kUdpChecksumOffset : kTcpChecksumOffset);
    const size_t end = d.l4 + d.covered;
    if (likely(end <= frame.size())) {
      if (verify_) {
        EmitPacket(ctx, pkt, Verify(frame.data(), d) ? kForwardGate : kFailGate);
        continue;
      }
      bess::PacketHandle handle = pkt.handle();
      // The whole datagram: the sum reads it all, from the copy if there is one.
      const auto head = bess::packet::WritableHead(handle, end);
      if (unlikely(!head)) {
        DropPacket(ctx, bess::PacketRef(handle));
        continue;
      }
      auto *bytes = reinterpret_cast<uint8_t *>(head->data());
      const uint16_t sum = Sum(bytes, d);
      std::memcpy(bytes + field, &sum, sizeof(sum));
      EmitPacket(ctx, bess::PacketRef(handle), kForwardGate);
      continue;
    }

    // A chain splits the datagram: packet_checksum.h sums across segments,
    // over the IP payload, which must then be the UDP datagram.
    if (d.covered != d.l4_length) {
      EmitPacket(ctx, pkt, kForwardGate);
      continue;
    }
    bess::packet::ChecksumPlan plan;
    plan.network_offset = d.l3;
    plan.transport_offset = d.l4;
    plan.transport =
        d.udp ? bess::packet::TransportChecksum::kUdp : bess::packet::TransportChecksum::kTcp;
    if (verify_) {
      bess::utils::be16_t stored;
      std::memcpy(&stored, frame.data() + field, sizeof(stored));
      const auto sums = bess::packet::ComputeChecksums(pkt, plan);
      if (unlikely(!sums)) {
        DropPacket(ctx, pkt);
        continue;
      }
      // UDP 0 is "none"; 0xffff is -0 for a computed 0x0000 (the library
      // already writes a UDP 0x0000 as 0xffff).
      const bool ok = (d.udp && stored == bess::utils::be16_t(0)) || sums->transport == stored ||
                      (sums->transport == bess::utils::be16_t(0) &&
                       stored == bess::utils::be16_t(0xffff));
      EmitPacket(ctx, pkt, ok ? kForwardGate : kFailGate);
      continue;
    }
    bess::PacketHandle handle = pkt.handle();
    if (unlikely(!bess::packet::ApplySoftwareChecksums(handle, plan))) {
      DropPacket(ctx, bess::PacketRef(handle));
      continue;
    }
    EmitPacket(ctx, bess::PacketRef(handle), kForwardGate);
  }
}

CommandResponse L4Checksum::Init(const bess::pb::L4ChecksumArg &arg) {
  verify_ = arg.verify();
  return CommandSuccess();
}

ADD_MODULE(L4Checksum, "l4_checksum",
           "recomputes the TCP/IPv4 and UDP/IPv4 checksums")
