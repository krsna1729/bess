// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vlan_split.h"

#include <cstdint>

#include "arch/vlan.h"
#include "packet_mutation.h"
#include "packet_reshape.h"
#include "utils/ether.h"

void VLANSplit::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;
  using bess::utils::Vlan;

  // A tagged frame's tag and the EtherType after it: the 16-byte tag removal
  // reads and writes the first 16 of these 18 bytes.
  constexpr size_t kTagged = sizeof(Ethernet) + sizeof(Vlan);

  for (bess::PacketRef pkt : batch->packets()) {
    // As VLANPop: a frame with no EtherType, or a tagged one whose first
    // segment ends inside the tag or the EtherType after it, is dropped
    // rather than read (or popped) past its data; a shared payload is copied
    // before the tag removal writes it (D-153).
    if (unlikely(pkt.head_len() < sizeof(Ethernet))) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    const be16_t tpid = pkt.head_data<const Ethernet *>()->ether_type;
    const bool tagged = (tpid == be16_t(Ethernet::Type::kVlan)) ||
                        (tpid == be16_t(Ethernet::Type::kQinQ));
    if (!tagged) {
      EmitPacket(ctx, pkt, 0); /* untagged packets go to gate 0 */
      continue;
    }

    if (unlikely(pkt.head_len() < kTagged)) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    if (unlikely(bess::packet::PayloadWriteabilityOf(pkt) !=
                 bess::packet::PayloadWriteability::kWritable)) {
      bess::PacketHandle copy = pkt.handle();
      if (!bess::packet::EnsureWritable(copy) ||
          bess::PacketRef(copy).head_len() < kTagged) {
        EmitPacket(ctx, bess::PacketRef(copy), DROP_GATE);
        continue;
      }
      pkt = bess::PacketRef(copy);
    }
    char *old_head = pkt.head_data<char *>();
    const be16_t tci =
        reinterpret_cast<const Vlan *>(old_head + sizeof(Ethernet))->tci;
    // Cannot fail: the first segment holds at least kTagged bytes.
    pkt.adj(4);
    bess::arch::RemoveVlanTag(old_head);
    EmitPacket(ctx, pkt,
               static_cast<gate_idx_t>(static_cast<uint32_t>(tci.value()) & 0x0fffU));
  }
}

ADD_MODULE(VLANSplit, "vlan_split", "split packets depending on their VID")
