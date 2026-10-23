// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vlan_pop.h"

#include "arch/vlan.h"
#include "packet_mutation.h"
#include "packet_reshape.h"
#include "utils/ether.h"

void VLANPop::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;

  // A tagged frame's tag and the EtherType after it: the 16-byte tag removal
  // reads and writes the first 16 of these 18 bytes.
  constexpr size_t kTagged = sizeof(Ethernet) + 4;

  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    // The EtherType read is at offset 12: a shorter frame has no header to
    // read, so it is dropped rather than read past its first segment.
    if (unlikely(pkt.head_len() < sizeof(Ethernet))) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    const be16_t tpid = pkt.head_data<const Ethernet *>()->ether_type;
    const bool tagged = (tpid == be16_t(Ethernet::Type::kVlan)) ||
                        (tpid == be16_t(Ethernet::Type::kQinQ));
    if (!tagged) {
      batch->handles()[out_cnt++] = pkt.handle();
      continue;
    }

    // A tagged frame is popped or dropped, never forwarded still tagged: one
    // whose first segment ends inside the tag or its EtherType (a truncated
    // tag, or a chain split there) has no tag to remove, and the removal
    // would run past the segment's data. The removal moves the addresses in
    // place, so a shared payload is copied first (D-153); the copy may
    // replace and resegment the mbuf, so the length is checked on it again.
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
    // Cannot fail: the first segment holds at least kTagged bytes.
    pkt.adj(4);
    bess::arch::RemoveVlanTag(old_head);
    batch->handles()[out_cnt++] = pkt.handle();
  }
  batch->set_cnt(out_cnt);

  RunNextModule(ctx, batch);
}

ADD_MODULE(VLANPop, "vlan_pop", "removes 802.1Q/802.11ad VLAN tag")
