// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "ether_encap.h"

#include "packet_reshape.h"
#include "utils/ether.h"

using bess::utils::Ethernet;

namespace {

// Fills the Ethernet header PrependWritable made room for.
void WriteHeader(bess::packet::MutableBytes header, const Ethernet::Address &src,
                 const Ethernet::Address &dst, bess::utils::be16_t ether_type) {
  auto *eth = reinterpret_cast<Ethernet *>(header.data());
  eth->dst_addr = dst;
  eth->src_addr = src;
  eth->ether_type = ether_type;
}

}  // namespace

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

bool EtherEncap::Encap(bess::PacketHandle &pkt,
                       const Ethernet::Address &src,
                       const Ethernet::Address &dst,
                       bess::utils::be16_t ether_type) {
  const auto header = bess::packet::PrependWritable(pkt, sizeof(Ethernet));
  if (unlikely(!header)) {
    return false;
  }
  WriteHeader(*header, src, dst, ether_type);
  return true;
}

void EtherEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const auto src_off = attr_offset(ATTR_R_ETHER_SRC);
  const auto dst_off = attr_offset(ATTR_R_ETHER_DST);
  const auto type_off = attr_offset(ATTR_R_ETHER_TYPE);

  const int cnt = batch->cnt();
  int out_cnt = 0;
  for (int i = 0; i < cnt; i++) {
    bess::PacketHandle pkt = batch->handles()[i];
    const bess::PacketRef ref(pkt);
    const Ethernet::Address ether_src =
        get_attr_with_offset<Ethernet::Address>(src_off, ref);
    const Ethernet::Address ether_dst =
        get_attr_with_offset<Ethernet::Address>(dst_off, ref);
    const bess::utils::be16_t ether_type =
        get_attr_with_offset<bess::utils::be16_t>(type_off, ref);

    // The prepend itself, not Encap: a call per packet would keep the
    // inline case out of this loop.
    const auto header = bess::packet::PrependWritable(pkt, sizeof(Ethernet));
    if (unlikely(!header)) {
      // No headroom, or a shared payload whose copy failed: forwarding the
      // payload would put a frame on the wire without the header the graph
      // configured. Drop and keep the rest.
      DropPacket(ctx, bess::PacketRef(pkt));
      continue;
    }
    WriteHeader(*header, ether_src, ether_dst, ether_type);
    batch->handles()[out_cnt++] = pkt;
  }
  batch->set_cnt(out_cnt);

  RunNextModule(ctx, batch);
}

ADD_MODULE(EtherEncap, "ether_encap",
           "encapsulates packets with an Ethernet header")
