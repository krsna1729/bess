// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vlan_split.h"

#include "arch/vlan.h"
#include "utils/ether.h"

void VLANSplit::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;
  using bess::utils::Vlan;

  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    char *old_head = pkt.head_data<char *>();
    const auto *eth = reinterpret_cast<const Ethernet *>(old_head);
    const be16_t tpid = eth->ether_type;

    bool tagged = (tpid == be16_t(Ethernet::Type::kVlan)) ||
                  (tpid == be16_t(Ethernet::Type::kQinQ));

    if (tagged && pkt.adj(4)) {
      const be16_t tci = reinterpret_cast<const Vlan *>(eth + 1)->tci;
      bess::arch::RemoveVlanTag(old_head);
      EmitPacket(ctx, pkt, tci.value() & 0x0fff);
    } else {
      EmitPacket(ctx, pkt, 0); /* untagged packets go to gate 0 */
    }
  }
}

ADD_MODULE(VLANSplit, "vlan_split", "split packets depending on their VID")
