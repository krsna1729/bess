// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vxlan_encap.h"

#include <rte_hash_crc.h>

#include "../utils/ether.h"
#include "../utils/ip.h"
#include "../utils/udp.h"
#include "../utils/vxlan.h"

using bess::utils::be16_t;
using bess::utils::be32_t;

enum {
  ATTR_R_TUN_IP_SRC,
  ATTR_R_TUN_IP_DST,
  ATTR_R_TUN_ID,
  ATTR_W_IP_SRC,
  ATTR_W_IP_DST,
  ATTR_W_IP_PROTO,
};

// NOTE: UDP port 4789 is the official port number assigned by IANA,
// but some systems (including Linux) uses 8472 for legacy reasons.
const uint16_t VXLANEncap::kDefaultDstPort = 4789;

CommandResponse VXLANEncap::Init(const bess::pb::VXLANEncapArg &arg) {
  auto dstport = arg.dstport();
  if (dstport == 0) {
    dstport_ = be16_t(kDefaultDstPort);
  } else {
    if (dstport >= 65536) {
      return CommandFailure(EINVAL, "invalid 'dstport' field");
    }
    dstport_ = be16_t(dstport);
  }

  using AccessMode = bess::metadata::Attribute::AccessMode;

  AddMetadataAttr("tun_ip_src", 4, AccessMode::kRead);
  AddMetadataAttr("tun_ip_dst", 4, AccessMode::kRead);
  AddMetadataAttr("tun_id", 4, AccessMode::kRead);
  AddMetadataAttr("ip_src", 4, AccessMode::kWrite);
  AddMetadataAttr("ip_dst", 4, AccessMode::kWrite);
  AddMetadataAttr("ip_proto", 1, AccessMode::kWrite);

  return CommandSuccess();
}

void VXLANEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Udp;
  using bess::utils::Vxlan;

  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);

    be32_t ip_src = get_attr<be32_t>(this, ATTR_R_TUN_IP_SRC, pkt);
    be32_t ip_dst = get_attr<be32_t>(this, ATTR_R_TUN_IP_DST, pkt);
    be32_t vni = get_attr<be32_t>(this, ATTR_R_TUN_ID, pkt);

    Ethernet *inner_eth;
    Udp *udp;
    Vxlan *vh;

    size_t inner_frame_len = pkt.total_len() + sizeof(*udp);

    inner_eth = pkt.head_data<Ethernet *>();
    udp = static_cast<Udp *>(pkt.prepend(sizeof(*udp) + sizeof(*vh)));
    if (unlikely(!udp)) {
      continue;
    }

    vh = reinterpret_cast<Vxlan *>(udp + 1);
    vh->vx_flags = be32_t(0x08000000);
    vh->vx_vni = vni << 8;

    uint32_t h = 0;
    if (inner_eth->ether_type.value() != Ethernet::Type::kIpv4) {
      h = rte_hash_crc(inner_eth, sizeof(Ethernet::Address) * 2, UINT32_MAX);
    } else {
      Ipv4 *inner_ip = reinterpret_cast<Ipv4 *>(inner_eth + 1);
      size_t ip_len = inner_ip->header_length;
      h = inner_ip->protocol;
      if (inner_ip->protocol == Ipv4::Proto::kTcp ||
          inner_ip->protocol == Ipv4::Proto::kUdp) {
        Udp *inner_l4 = reinterpret_cast<Udp *>(
            reinterpret_cast<uint8_t *>(inner_ip) + ip_len);
        h = rte_hash_crc(&inner_l4->src_port, sizeof(be32_t), h);
      }
      h = rte_hash_crc(&inner_ip->src, sizeof(be32_t) * 2, h);
    }
    udp->src_port = be16_t(h | 0xc000);  // Force into private range (RFC6335)
    udp->dst_port = dstport_;
    udp->length = be16_t(sizeof(*udp) + inner_frame_len);
    udp->checksum = 0;

    set_attr<be32_t>(this, ATTR_W_IP_SRC, pkt, ip_src);
    set_attr<be32_t>(this, ATTR_W_IP_DST, pkt, ip_dst);
    set_attr<uint8_t>(this, ATTR_W_IP_PROTO, pkt, Ipv4::Proto::kUdp);
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(VXLANEncap, "vxlan_encap",
           "encapsulates packets with UDP/VXLAN headers")
