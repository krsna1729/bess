// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vlan_pop.h"

#include "arch/vlan.h"
#include "utils/ether.h"

void VLANPop::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;

  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    // The EtherType read is at offset 12: a shorter frame has no header to
    // read, so it is dropped rather than read past its first segment.
    if (unlikely(pkt.head_len() < sizeof(Ethernet))) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    char *old_head = pkt.head_data<char *>();
    const be16_t tpid =
        reinterpret_cast<const Ethernet *>(old_head)->ether_type;

    bool tagged = (tpid == be16_t(Ethernet::Type::kVlan)) ||
                  (tpid == be16_t(Ethernet::Type::kQinQ));

    if (tagged && pkt.adj(4)) {
      bess::arch::RemoveVlanTag(old_head);
    }
    batch->handles()[out_cnt++] = pkt.handle();
  }
  batch->set_cnt(out_cnt);

  RunNextModule(ctx, batch);
}

ADD_MODULE(VLANPop, "vlan_pop", "removes 802.1Q/802.11ad VLAN tag")
