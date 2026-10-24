// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "ip_checksum.h"

#include <cstdint>
#include <span>

#include "packet_parse.h"
#include "packet_reshape.h"
#include "utils/checksum.h"
#include "utils/ip.h"

namespace {

enum : gate_idx_t { kForwardGate = 0, kFailGate = 1 };

using bess::packet::ParseStatus;

// The IPv4 header's length, from its first byte at `ip`.
inline size_t HeaderLength(const uint8_t *ip) { return static_cast<size_t>(*ip & 0x0fu) * 4; }

// Where the IPv4 header of `frame` (a packet of `total_len` bytes) starts,
// or 0 when there is none: the checked parse's verdict, CheckFastIpv4 for the
// common frame (no ParsedFrame to fill) and ParseFrame otherwise. Malformed
// frames set `malformed`.
size_t Ipv4Offset(std::span<const uint8_t> frame, size_t total_len, bool &malformed) {
  const bess::packet::FastIpv4 fast = bess::packet::CheckFastIpv4(frame, total_len);
  if (likely(fast.taken)) {
    malformed = fast.status == ParseStatus::kMalformed;
    return 14;
  }
  bess::packet::ParsedFrame parsed;
  const ParseStatus status = bess::packet::ParseFrame(frame, parsed, total_len);
  malformed = status == ParseStatus::kMalformed;
  return parsed.l3 == bess::packet::L3Kind::kIpv4 ? parsed.l3_offset : 0;
}

}  // namespace

// Where the IPv4 header is comes from the checked parse (packet_parse.h) on
// the first segment, its lengths against the packet:
//  - IPv4, untagged or behind up to two VLAN tags, with options, fragments
//    included (each fragment carries its own header checksum): the header
//    checksum is recomputed, or with `verify` checked (forward on gate 0 when
//    right, gate 1 when wrong).
//  - A frame that is not IPv4 (ARP, IPv6, ...) passes unchanged on gate 0.
//  - A frame the parse calls malformed (headers that do not fit the first
//    segment, lengths that lie) is dropped, never read past or partly
//    written (D-121, D-172).
// The header is in the first segment (D-121), so the sum is the checksum
// kernels' over it there (utils/checksum.h), and the write is WritableHead's:
// a shared payload is copied first (D-153); a frame whose copy fails is
// dropped. (packet_checksum.h's chain walk computes the same sum; it is for
// ranges a chain may split, which the header never is here.)
void IPChecksum::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::Ipv4;

  for (bess::PacketRef pkt : batch->packets()) {
    const std::span<const uint8_t> frame(pkt.head_data<const uint8_t *>(), pkt.head_len());
    bool malformed = false;
    const size_t l3 = Ipv4Offset(frame, pkt.total_len(), malformed);
    if (unlikely(malformed)) {
      DropPacket(ctx, pkt);
      continue;
    }
    if (l3 == 0) {
      EmitPacket(ctx, pkt, kForwardGate);
      continue;
    }

    if (verify_) {
      // Summed with the stored field, as RFC 1071 verifies: a checksum stored
      // as 0xffff where 0x0000 was computed (-0, RFC 1624) is right too.
      const auto *ip = reinterpret_cast<const Ipv4 *>(frame.data() + l3);
      EmitPacket(ctx, pkt, bess::utils::VerifyIpv4Checksum(*ip) ? kForwardGate : kFailGate);
      continue;
    }
    bess::PacketHandle handle = pkt.handle();
    // The whole header: the sum reads it all, from the copy if there is one.
    const auto head = bess::packet::WritableHead(handle, l3 + HeaderLength(frame.data() + l3));
    if (unlikely(!head)) {
      DropPacket(ctx, bess::PacketRef(handle));
      continue;
    }
    auto *ip = reinterpret_cast<Ipv4 *>(reinterpret_cast<uint8_t *>(head->data()) + l3);
    ip->checksum = bess::utils::CalculateIpv4Checksum(*ip);
    EmitPacket(ctx, bess::PacketRef(handle), kForwardGate);
  }
}

CommandResponse IPChecksum::Init(const bess::pb::IPChecksumArg &arg) {
  verify_ = arg.verify();
  return CommandSuccess();
}

ADD_MODULE(IPChecksum, "ip_checksum", "recomputes the IPv4 checksum")
