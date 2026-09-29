// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vlan_split.h"

#include "../utils/ether.h"

void VLANSplit::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;

  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    char *old_head = pkt.head_data<char *>();
    __m128i eth;

    eth = _mm_loadu_si128(reinterpret_cast<__m128i *>(old_head));
    be16_t tpid(be16_t::swap(_mm_extract_epi16(eth, 6)));

    bool tagged = (tpid == be16_t(Ethernet::Type::kVlan)) ||
                  (tpid == be16_t(Ethernet::Type::kQinQ));

    if (tagged && pkt.adj(4)) {
      be16_t tci(be16_t::swap(_mm_extract_epi16(eth, 7)));
      eth = _mm_slli_si128(eth, 4);
      _mm_storeu_si128(reinterpret_cast<__m128i *>(old_head), eth);
      EmitPacket(ctx, pkt, tci.value() & 0x0fff);
    } else {
      EmitPacket(ctx, pkt, 0); /* untagged packets go to gate 0 */
    }
  }
}

ADD_MODULE(VLANSplit, "vlan_split", "split packets depending on their VID")
