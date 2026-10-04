// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vxlan_decap.h"

#include <cstring>
#include <span>

#include "tunnel/tunnel.h"
#include "utils/endian.h"

enum {
  ATTR_W_TUN_IP_SRC,
  ATTR_W_TUN_IP_DST,
  ATTR_W_TUN_ID,
};

CommandResponse VXLANDecap::Init(
    const bess::pb::VXLANDecapArg &arg [[maybe_unused]]) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  AddMetadataAttr("tun_ip_src", 4, AccessMode::kWrite);
  AddMetadataAttr("tun_ip_dst", 4, AccessMode::kWrite);
  AddMetadataAttr("tun_id", 4, AccessMode::kWrite);

  return CommandSuccess();
}

// Strips the outer Ethernet/IPv4/UDP/VXLAN headers after checking them
// through the tunnel library (M19, D-069): any VLAN tags, IPv4 options and
// lengths, the VXLAN I flag, an inner Ethernet header. Any UDP port (the
// classification upstream decided this is VXLAN). A packet that does not
// check, or has an IPv6 outer header (the metadata holds IPv4 addresses), is
// dropped; before, it was decapsulated at fixed offsets regardless.
void VXLANDecap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be32_t;
  const int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    bess::conntrack::ParsedFlowPacket outer;
    bess::tunnel::Decapsulated d;
    const auto status = bess::tunnel::DecapVxlan(
        std::span<const uint8_t>(pkt.head_data<const uint8_t *>(), pkt.head_len()), outer, d,
        std::nullopt, pkt.total_len());
    if (unlikely(status != bess::tunnel::DecapError::kOk ||
                 outer.l3 != bess::conntrack::L3Kind::kIpv4)) {
      DropPacket(ctx, pkt);
      continue;
    }
    be32_t src, dst;
    std::memcpy(&src, outer.src.data(), 4);
    std::memcpy(&dst, outer.dst.data(), 4);
    set_attr<be32_t>(this, ATTR_W_TUN_IP_SRC, pkt, src);
    set_attr<be32_t>(this, ATTR_W_TUN_IP_DST, pkt, dst);
    set_attr<be32_t>(this, ATTR_W_TUN_ID, pkt, be32_t(d.id));
    pkt.adj(static_cast<uint16_t>(d.inner_offset));
    EmitPacket(ctx, pkt, 0);
  }
}

ADD_MODULE(VXLANDecap, "vxlan_decap",
           "decapsulates the outer Ethernet/IP/UDP/VXLAN headers")
