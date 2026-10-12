// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vxlan_encap.h"

#include <span>

#include "tunnel/tunnel.h"
#include "utils/ip.h"

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

const uint16_t VXLANEncap::kDefaultDstPort = bess::tunnel::kVxlanPort;

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

// Prepends the UDP and VXLAN headers (the outer IP header is IPEncap's, from
// the metadata written here), through the tunnel library (M19, D-069). A
// packet without the headroom is dropped (before, it went on unencapsulated).
void VXLANEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const auto tun_src = attr_offset(ATTR_R_TUN_IP_SRC);
  const auto tun_dst = attr_offset(ATTR_R_TUN_IP_DST);
  const auto tun_id = attr_offset(ATTR_R_TUN_ID);
  const auto out_src = attr_offset(ATTR_W_IP_SRC);
  const auto out_dst = attr_offset(ATTR_W_IP_DST);
  const auto out_proto = attr_offset(ATTR_W_IP_PROTO);
  for (bess::PacketRef pkt : batch->packets()) {
    const be32_t ip_src = get_attr_with_offset<be32_t>(tun_src, pkt);
    const be32_t ip_dst = get_attr_with_offset<be32_t>(tun_dst, pkt);
    const be32_t vni = get_attr_with_offset<be32_t>(tun_id, pkt);

    const uint16_t src_port = bess::tunnel::FlowEntropyPort(
        std::span<const uint8_t>(pkt.head_data<const uint8_t *>(), pkt.head_len()));
    const size_t inner_len = pkt.total_len();
    auto *hdr = static_cast<uint8_t *>(
        pkt.prepend(bess::tunnel::kUdpBytes + bess::tunnel::kVxlanBytes));
    if (unlikely(hdr == nullptr)) {
      DropPacket(ctx, pkt);
      continue;
    }
    bess::tunnel::WriteVxlanUdp(hdr, vni.value(), src_port, dstport_.value(), inner_len);

    set_attr_with_offset<be32_t>(out_src, pkt, ip_src);
    set_attr_with_offset<be32_t>(out_dst, pkt, ip_dst);
    set_attr_with_offset<uint8_t>(out_proto, pkt, bess::utils::Ipv4::Proto::kUdp);
    EmitPacket(ctx, pkt, 0);
  }
}

ADD_MODULE(VXLANEncap, "vxlan_encap",
           "encapsulates packets with UDP/VXLAN headers")
