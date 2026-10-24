// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vxlan_encap.h"

#include <limits>
#include <span>

#include "packet_reshape.h"
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
// the metadata written here), through the tunnel library (M19, D-069). The
// headers go into the packet's headroom with packet::PrependWritable: a
// shared payload (a clone, an indirect mbuf) is copied first, as its
// headroom is the other holders' buffer too. A packet is dropped, never sent
// on unencapsulated, when it has no headroom, when the copy fails, or when it
// is too long for the UDP length field (more than 65,535 bytes with the UDP
// and VXLAN headers).
void VXLANEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  constexpr size_t kHeaderBytes = bess::tunnel::kUdpBytes + bess::tunnel::kVxlanBytes;
  constexpr size_t kMaxInner = std::numeric_limits<uint16_t>::max() - kHeaderBytes;
  const auto tun_src = attr_offset(ATTR_R_TUN_IP_SRC);
  const auto tun_dst = attr_offset(ATTR_R_TUN_IP_DST);
  const auto tun_id = attr_offset(ATTR_R_TUN_ID);
  const auto out_src = attr_offset(ATTR_W_IP_SRC);
  const auto out_dst = attr_offset(ATTR_W_IP_DST);
  const auto out_proto = attr_offset(ATTR_W_IP_PROTO);
  const int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::PacketHandle handle = batch->handles()[i];
    const bess::PacketRef inner(handle);
    const be32_t ip_src = get_attr_with_offset<be32_t>(tun_src, inner);
    const be32_t ip_dst = get_attr_with_offset<be32_t>(tun_dst, inner);
    const be32_t vni = get_attr_with_offset<be32_t>(tun_id, inner);

    const uint16_t src_port = bess::tunnel::FlowEntropyPort(
        std::span<const uint8_t>(inner.head_data<const uint8_t *>(), inner.head_len()));
    const size_t inner_len = inner.total_len();
    if (unlikely(inner_len > kMaxInner)) {
      DropPacket(ctx, inner);
      continue;
    }
    const auto hdr = bess::packet::PrependWritable(handle, kHeaderBytes);
    const bess::PacketRef pkt(handle);
    if (unlikely(!hdr)) {
      DropPacket(ctx, pkt);
      continue;
    }
    bess::tunnel::WriteVxlanUdp(reinterpret_cast<uint8_t *>(hdr->data()), vni.value(), src_port,
                                dstport_.value(), inner_len);

    set_attr_with_offset<be32_t>(out_src, pkt, ip_src);
    set_attr_with_offset<be32_t>(out_dst, pkt, ip_dst);
    set_attr_with_offset<uint8_t>(out_proto, pkt, bess::utils::Ipv4::Proto::kUdp);
    EmitPacket(ctx, pkt, 0);
  }
}

ADD_MODULE(VXLANEncap, "vxlan_encap",
           "encapsulates packets with UDP/VXLAN headers")
