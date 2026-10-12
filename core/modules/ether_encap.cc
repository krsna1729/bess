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

void EtherEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const auto src_off = attr_offset(ATTR_R_ETHER_SRC);
  const auto dst_off = attr_offset(ATTR_R_ETHER_DST);
  const auto type_off = attr_offset(ATTR_R_ETHER_TYPE);

  for (bess::PacketRef pkt : batch->packets()) {
    Ethernet::Address ether_src;
    Ethernet::Address ether_dst;
    bess::utils::be16_t ether_type;

    ether_src = get_attr_with_offset<Ethernet::Address>(src_off, pkt);
    ether_dst = get_attr_with_offset<Ethernet::Address>(dst_off, pkt);
    ether_type = get_attr_with_offset<bess::utils::be16_t>(type_off, pkt);

    Ethernet *eth = static_cast<Ethernet *>(pkt.prepend(sizeof(*eth)));

    // not enough headroom?
    if (unlikely(!eth)) {
      continue;
    }

    eth->dst_addr = ether_dst;
    eth->src_addr = ether_src;
    eth->ether_type = ether_type;
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(EtherEncap, "ether_encap",
           "encapsulates packets with an Ethernet header")
