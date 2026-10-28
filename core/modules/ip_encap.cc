// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "ip_encap.h"

#include <limits>

#include "packet_reshape.h"
#include "utils/checksum.h"
#include "utils/ether.h"
#include "utils/ip.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::be16_t;
using bess::utils::be32_t;

enum {
  ATTR_R_IP_SRC,
  ATTR_R_IP_DST,
  ATTR_R_IP_PROTO,
  ATTR_W_IP_NEXTHOP,
  ATTR_W_ETHER_TYPE,
};

CommandResponse IPEncap::Init(const bess::pb::IPEncapArg &arg[[maybe_unused]]) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  if (const int id = AddMetadataAttr("ip_src", 4, AccessMode::kRead); id < 0) {
    return CommandFailure(-id, "add_metadata_attr(ip_src) failed");
  }
  if (const int id = AddMetadataAttr("ip_dst", 4, AccessMode::kRead); id < 0) {
    return CommandFailure(-id, "add_metadata_attr(ip_dst) failed");
  }
  if (const int id = AddMetadataAttr("ip_proto", 1, AccessMode::kRead);
      id < 0) {
    return CommandFailure(-id, "add_metadata_attr(ip_proto) failed");
  }
  if (const int id = AddMetadataAttr("ip_nexthop", 4, AccessMode::kWrite);
      id < 0) {
    return CommandFailure(-id, "add_metadata_attr(ip_nexthop) failed");
  }
  if (const int id = AddMetadataAttr("ether_type", 2, AccessMode::kWrite);
      id < 0) {
    return CommandFailure(-id, "add_metadata_attr(ether_type) failed");
  }

  return CommandSuccess();
}

// Prepends an IPv4 header (no options) into the packet's headroom with
// packet::PrependWritable: a shared payload (a clone, an indirect mbuf) is
// copied first, as its headroom is the other holders' buffer too. A packet is
// dropped, never sent on without its header, when it has no headroom, when
// the copy fails, or when it is too long for the IPv4 total length (more than
// 65,535 bytes with the header).
void IPEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  constexpr size_t kMaxInner = std::numeric_limits<uint16_t>::max() - sizeof(Ipv4);
  const auto src_off = attr_offset(ATTR_R_IP_SRC);
  const auto dst_off = attr_offset(ATTR_R_IP_DST);
  const auto proto_off = attr_offset(ATTR_R_IP_PROTO);
  const auto nexthop_off = attr_offset(ATTR_W_IP_NEXTHOP);
  const auto type_off = attr_offset(ATTR_W_ETHER_TYPE);

  batch->KeepIf([&](bess::PacketHandle &handle, int) {
    const bess::PacketRef inner(handle);
    const be32_t ip_src = get_attr_with_offset<be32_t>(src_off, inner);
    const be32_t ip_dst = get_attr_with_offset<be32_t>(dst_off, inner);
    const uint8_t ip_proto = get_attr_with_offset<uint8_t>(proto_off, inner);

    const size_t inner_len = inner.total_len();
    if (unlikely(inner_len > kMaxInner)) {
      DropPacket(ctx, inner);
      return false;
    }
    const auto header = bess::packet::PrependWritable(handle, sizeof(Ipv4));
    const bess::PacketRef pkt(handle);
    if (unlikely(!header)) {
      DropPacket(ctx, pkt);
      return false;
    }

    auto *iph = reinterpret_cast<Ipv4 *>(header->data());
    iph->version = 0x4;
    iph->header_length = sizeof(*iph) / 4;
    iph->type_of_service = 0;
    iph->length = be16_t(static_cast<uint16_t>(inner_len + sizeof(*iph)));
    iph->id = be16_t(0);
    iph->fragment_offset = be16_t(Ipv4::Flag::kDF);
    iph->ttl = 64;
    iph->protocol = ip_proto;
    iph->src = ip_src;
    iph->dst = ip_dst;

    iph->checksum = bess::utils::CalculateIpv4NoOptChecksum(*iph);

    set_attr_with_offset<be32_t>(nexthop_off, pkt, ip_dst);
    set_attr_with_offset<be16_t>(type_off, pkt, be16_t(Ethernet::Type::kIpv4));
    return true;
  });

  RunNextModule(ctx, batch);
}

ADD_MODULE(IPEncap, "ip_encap", "encapsulates packets with an IPv4 header")
