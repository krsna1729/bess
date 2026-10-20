// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "ether_encap.h"

#include "utils/ether.h"

using bess::utils::Ethernet;

enum {
  ATTR_R_ETHER_SRC,
  ATTR_R_ETHER_DST,
  ATTR_R_ETHER_TYPE,
};

CommandResponse EtherEncap::Init(
    const bess::pb::EtherEncapArg &arg[[maybe_unused]]) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  AddMetadataAttr("ether_src", sizeof(Ethernet::Address), AccessMode::kRead);
  AddMetadataAttr("ether_dst", sizeof(Ethernet::Address), AccessMode::kRead);
  AddMetadataAttr("ether_type", 2, AccessMode::kRead);

  return CommandSuccess();
};

bool EtherEncap::Encap(bess::PacketRef pkt,
                       const Ethernet::Address &src,
                       const Ethernet::Address &dst,
                       bess::utils::be16_t ether_type) {
  Ethernet *eth = static_cast<Ethernet *>(pkt.prepend(sizeof(*eth)));
  if (unlikely(!eth)) {
    return false;
  }
  eth->dst_addr = dst;
  eth->src_addr = src;
  eth->ether_type = ether_type;
  return true;
}

void EtherEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const auto src_off = attr_offset(ATTR_R_ETHER_SRC);
  const auto dst_off = attr_offset(ATTR_R_ETHER_DST);
  const auto type_off = attr_offset(ATTR_R_ETHER_TYPE);

  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    const Ethernet::Address ether_src =
        get_attr_with_offset<Ethernet::Address>(src_off, pkt);
    const Ethernet::Address ether_dst =
        get_attr_with_offset<Ethernet::Address>(dst_off, pkt);
    const bess::utils::be16_t ether_type =
        get_attr_with_offset<bess::utils::be16_t>(type_off, pkt);

    if (unlikely(!Encap(pkt, ether_src, ether_dst, ether_type))) {
      // No headroom: forwarding the payload would put a frame on the wire
      // without the header the graph configured. Drop and keep the rest.
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    batch->handles()[out_cnt++] = pkt.handle();
  }
  batch->set_cnt(out_cnt);

  RunNextModule(ctx, batch);
}

ADD_MODULE(EtherEncap, "ether_encap",
           "encapsulates packets with an Ethernet header")
