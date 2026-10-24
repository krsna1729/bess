// Copyright (c) 2018, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "static_nat.h"

#include <cstdint>
#include <cstring>
#include <span>

#include "packet_parse.h"
#include "packet_reshape.h"
#include "utils/checksum.h"
#include "utils/ip.h"
#include "utils/tcp.h"
#include "utils/udp.h"

// No resources and no actions: the pairs are the init argument, which
// GetPipeline's Module.arg carries back (D-129).
const Commands StaticNAT::cmds = {};

CommandResponse StaticNAT::Init(const bess::pb::StaticNATArg &arg) {
  using bess::utils::ParseIpv4Address;

  for (const auto &pb_pair : arg.pairs()) {
    be32_t int_start, int_end;
    if (!ParseIpv4Address(pb_pair.int_range().start(), &int_start)) {
      return CommandFailure(EINVAL, "invalid IP address %s",
                            pb_pair.int_range().start().c_str());
    }
    if (!ParseIpv4Address(pb_pair.int_range().end(), &int_end)) {
      return CommandFailure(EINVAL, "invalid IP address %s",
                            pb_pair.int_range().end().c_str());
    }
    if (int_start > int_end) {
      return CommandFailure(EINVAL, "invalid internal IP address range");
    }

    be32_t ext_start, ext_end;
    if (!ParseIpv4Address(pb_pair.ext_range().start(), &ext_start)) {
      return CommandFailure(EINVAL, "invalid IP address %s",
                            pb_pair.ext_range().start().c_str());
    }
    if (!ParseIpv4Address(pb_pair.ext_range().end(), &ext_end)) {
      return CommandFailure(EINVAL, "invalid IP address %s",
                            pb_pair.ext_range().end().c_str());
    }
    if (ext_start > ext_end) {
      return CommandFailure(EINVAL, "invalid external IP address range");
    }

    if (int_end.value() == 0xffffffff || ext_end.value() == 0xffffffff) {
      return CommandFailure(EINVAL, "cannot map broadcast address");
    }

    if (int_end - int_start != ext_end - ext_start) {
      return CommandFailure(EINVAL, "internal/external address ranges differ");
    }

    pairs_.push_back({.int_addr = int_start.value(),
                      .ext_addr = ext_start.value(),
                      .size = int_end.value() - int_start.value() + 1});
  }

  return CommandSuccess();
}

// Updates the L3 checksum, and the L4 checksum when the packet carries its
// L4 header (`l4`, nullptr for a non-first fragment).
static inline void UpdateChecksum(bess::utils::Ipv4 *ip, uint8_t *l4,
                                  uint32_t incr) {
  using IpProto = bess::utils::Ipv4::Proto;

  ip->checksum = bess::utils::UpdateChecksumWithIncrement(ip->checksum, incr);
  if (l4 == nullptr) {
    return;
  }

  const auto proto = static_cast<IpProto>(ip->protocol);
  if (proto == IpProto::kTcp) {
    auto *tcp = reinterpret_cast<bess::utils::Tcp *>(l4);
    tcp->checksum =
        bess::utils::UpdateChecksumWithIncrement(tcp->checksum, incr);
  } else if (proto == IpProto::kUdp) {
    // NOTE: UDP checksum is tricky in two ways:
    // 1. if the old checksum field was 0 (not set), no need to update
    // 2. if the updated value is 0, use 0xffff (rfc768)
    auto *udp = reinterpret_cast<bess::utils::Udp *>(l4);
    if (udp->checksum != 0) {
      udp->checksum =
          bess::utils::UpdateChecksumWithIncrement(udp->checksum, incr)
              ?: 0xffff;
    }
  }
}

// Where the headers are comes from the checked parse (packet_parse.h), on the
// first segment, before anything is written:
//  - IPv4 (untagged or behind up to two VLAN tags) whose address is in a
//    range is translated: the address, the IP checksum, and the TCP or UDP
//    checksum. A non-first fragment has no L4 header: its address and IP
//    checksum only, never its payload read as a checksum (the first fragment
//    carries the datagram's L4 checksum).
//  - A frame that is not IPv4 (ARP, IPv6, ...) leaves untranslated, as an
//    address outside every range does.
//  - A frame whose IP headers do not fit its first segment or lie about their
//    lengths (ParseFrame's kMalformed) is dropped (D-172).
// The translation is in place: a shared payload is copied first (D-153), and
// a frame whose copy fails is dropped. One left untranslated is not copied.
template <StaticNAT::Direction dir>
inline void StaticNAT::DoProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::packet::ParseStatus;
  gate_idx_t ogate_idx = dir == kForward ? 1 : 0;

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
        EmitPacket(ctx, pkt, ogate_idx);
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

    // The parse found the whole IPv4 header in `frame`.
    const size_t addr_offset = l3 + (dir == kForward ? 12 : 16);
    be32_t addr_be;
    std::memcpy(&addr_be, frame.data() + addr_offset, sizeof(addr_be));
    const uint32_t addr = addr_be.value();

    const NatPair *match = nullptr;
    for (const auto &pair : pairs_) {
      uint32_t start_addr = (dir == kForward) ? pair.int_addr : pair.ext_addr;
      if (start_addr <= addr && addr < start_addr + pair.size) {
        match = &pair;
        break;
      }
    }
    // If there is no matching address pair, forward without NAT.
    // TODO: add an option to redirect unmatched traffic to a specified ogate
    if (match == nullptr) {
      EmitPacket(ctx, pkt, ogate_idx);
      continue;
    }

    // The L4 checksum is in the bytes the parse checked: TCP's at 16 of its
    // 20-byte header, UDP's at 6 of 8.
    const bool has_l4 = status == ParseStatus::kOk;
    size_t required = l3 + sizeof(bess::utils::Ipv4);
    if (has_l4 && protocol == static_cast<uint8_t>(bess::utils::Ipv4::kTcp)) {
      required = l4 + sizeof(bess::utils::Tcp);
    } else if (has_l4 &&
               protocol == static_cast<uint8_t>(bess::utils::Ipv4::kUdp)) {
      required = l4 + sizeof(bess::utils::Udp);
    }
    bess::PacketHandle handle = pkt.handle();
    const auto head = bess::packet::WritableHead(handle, required);
    if (unlikely(!head)) {
      EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
      continue;
    }
    auto *bytes = reinterpret_cast<uint8_t *>(head->data());

    const uint32_t diff = (dir == kForward)
                              ? (match->ext_addr - match->int_addr)
                              : (match->int_addr - match->ext_addr);
    const be32_t new_addr_be = be32_t(addr + diff);
    UpdateChecksum(reinterpret_cast<bess::utils::Ipv4 *>(bytes + l3),
                   has_l4 ? bytes + l4 : nullptr,
                   bess::utils::ChecksumIncrement32(addr_be.raw_value(),
                                                    new_addr_be.raw_value()));
    std::memcpy(bytes + addr_offset, &new_addr_be, sizeof(new_addr_be));
    EmitPacket(ctx, bess::PacketRef(handle), ogate_idx);
  }
}

void StaticNAT::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  gate_idx_t incoming_gate = ctx->current_igate;

  if (incoming_gate == 0) {
    DoProcessBatch<kForward>(ctx, batch);
  } else {
    DoProcessBatch<kReverse>(ctx, batch);
  }
}

ADD_MODULE(StaticNAT, "static_nat", "Static network address translator")
