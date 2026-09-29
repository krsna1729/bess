// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vlan_pop.h"

#include "../utils/ether.h"

void VLANPop::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;

  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    char *old_head = pkt.head_data<char *>();

    __m128i eth = _mm_loadu_si128(reinterpret_cast<__m128i *>(old_head));
    be16_t tpid(be16_t::swap(_mm_extract_epi16(eth, 6)));

    bool tagged = (tpid == be16_t(Ethernet::Type::kVlan)) ||
                  (tpid == be16_t(Ethernet::Type::kQinQ));

    if (tagged && pkt.adj(4)) {
      eth = _mm_slli_si128(eth, 4);
      _mm_storeu_si128(reinterpret_cast<__m128i *>(old_head), eth);
    }
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(VLANPop, "vlan_pop", "removes 802.1Q/802.11ad VLAN tag")
