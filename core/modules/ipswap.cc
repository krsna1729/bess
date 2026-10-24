// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "ipswap.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "packet_parse.h"
#include "packet_reshape.h"

namespace {

using bess::packet::ParseStatus;

constexpr uint8_t kTcp = 6;
constexpr uint8_t kUdp = 17;

// Swaps the `n` bytes at `a` and `b`, which do not overlap.
template <size_t n>
inline void SwapBytes(uint8_t *a, uint8_t *b) {
  std::array<uint8_t, n> tmp;
  std::memcpy(tmp.data(), a, n);
  std::memcpy(a, b, n);
  std::memcpy(b, tmp.data(), n);
}

}  // namespace

// Where the headers are comes from the checked parse (packet_parse.h), on the
// first segment, before anything is written:
//  - IPv4 (untagged or behind up to two VLAN tags): the addresses are
//    swapped, and for TCP and UDP the ports too. A non-first fragment has no
//    L4 header, so only its addresses are swapped; its payload is never read
//    as ports.
//  - A frame that is not IPv4 (ARP, IPv6, ...) passes unchanged.
//  - A frame whose IP headers do not fit its first segment or lie about their
//    lengths (ParseFrame's kMalformed) is dropped, never partly swapped
//    (D-172).
// The swap is in place: a shared payload is copied first (D-153), and a frame
// whose copy fails is dropped. The checksums need no update: a swap leaves
// every 16-bit sum unchanged.
void IPSwap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    const std::span<const uint8_t> frame(pkt.head_data<const uint8_t *>(),
                                         pkt.head_len());
    ParseStatus status;
    size_t l3;
    size_t l4;
    uint8_t protocol;
    const bess::packet::FastIpv4 fast =
        bess::packet::CheckFastIpv4(frame, pkt.total_len());
    if (likely(fast.taken)) {
      status = fast.status;
      l3 = 14;
      l4 = bess::packet::kFastIpv4L4;
      protocol = fast.protocol;
    } else {
      bess::packet::ParsedFrame parsed;
      status = bess::packet::ParseFrame(frame, parsed, pkt.total_len());
      if (status != ParseStatus::kMalformed &&
          parsed.l3 != bess::packet::L3Kind::kIpv4) {
        batch->handles()[out_cnt++] = pkt.handle();
        continue;
      }
      l3 = parsed.l3_offset;
      l4 = parsed.l4_offset;
      protocol = parsed.protocol;
    }
    if (unlikely(status == ParseStatus::kMalformed)) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }

    const bool ports =
        status == ParseStatus::kOk && (protocol == kTcp || protocol == kUdp);
    bess::PacketHandle handle = pkt.handle();
    const auto head =
        bess::packet::WritableHead(handle, ports ? l4 + 4 : l3 + 20);
    if (unlikely(!head)) {
      EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
      continue;
    }
    auto *bytes = reinterpret_cast<uint8_t *>(head->data());
    SwapBytes<4>(bytes + l3 + 12, bytes + l3 + 16);
    if (ports) {
      // TCP and UDP share the same layout for ports.
      SwapBytes<2>(bytes + l4, bytes + l4 + 2);
    }
    batch->handles()[out_cnt++] = handle;
  }
  batch->set_cnt(out_cnt);

  RunNextModule(ctx, batch);
}

ADD_MODULE(IPSwap, "ipswap",
           "swaps source/destination IP addresses and L4 ports")
