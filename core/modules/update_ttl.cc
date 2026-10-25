// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "update_ttl.h"

#include <cstdint>
#include <span>

#include "packet_parse.h"
#include "packet_reshape.h"
#include "route/l3_packet.h"
#include "utils/ip.h"

using bess::packet::ParseStatus;

// Only IPv4 has its TTL decremented; where its header is comes from the
// checked parse (packet_parse.h) on the first segment:
//  - IPv4 (untagged or behind up to two VLAN tags, fragments included, as a
//    router decrements every fragment): TTL 0 and 1 are dropped, anything
//    else leaves one lower with the header checksum updated
//    (route::DecrementTtl).
//  - A frame that is not IPv4 (ARP, IPv6, ...) has no IPv4 TTL: it passes
//    unchanged.
//  - A frame whose IP headers do not fit its first segment or lie about their
//    lengths (ParseFrame's kMalformed) is dropped, as Router drops it.
// The decrement is in place: a shared payload is copied first (D-153), and a
// frame whose copy fails is dropped. An expiring frame is dropped before
// that, without a copy.
void UpdateTTL::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  // Every survivor leaves on gate 0: they stay in the batch and are passed
  // on at once, the drops going one by one.
  batch->KeepIf([&](bess::PacketHandle &handle, int) {
    const bess::PacketRef pkt(handle);
    const std::span<const uint8_t> frame(pkt.head_data<const uint8_t *>(),
                                         pkt.head_len());
    size_t l3;
    const bess::packet::FastIpv4 fast =
        bess::packet::CheckFastIpv4(frame, pkt.total_len());
    if (likely(fast.taken)) {
      if (unlikely(fast.status == ParseStatus::kMalformed)) {
        DropPacket(ctx, pkt);
        return false;
      }
      l3 = 14;
    } else {
      bess::packet::ParsedFrame parsed;
      const ParseStatus status =
          bess::packet::ParseFrame(frame, parsed, pkt.total_len());
      if (status == ParseStatus::kMalformed) {
        DropPacket(ctx, pkt);
        return false;
      }
      if (parsed.l3 != bess::packet::L3Kind::kIpv4) {
        return true;
      }
      l3 = parsed.l3_offset;
    }

    // The parse found the whole IPv4 header in `frame`.
    if (frame[l3 + 8] <= 1) {
      DropPacket(ctx, pkt);
      return false;
    }
    const auto head =
        bess::packet::WritableHead(handle, l3 + sizeof(bess::utils::Ipv4));
    if (unlikely(!head)) {
      DropPacket(ctx, bess::PacketRef(handle));
      return false;
    }
    // TTL 2 or more: kForward.
    bess::route::DecrementTtl(
        *reinterpret_cast<bess::utils::Ipv4 *>(head->data() + l3));
    return true;
  });

  RunNextModule(ctx, batch);
}

ADD_MODULE(UpdateTTL, "update_ttl", "decreases the IP TTL field by 1")
