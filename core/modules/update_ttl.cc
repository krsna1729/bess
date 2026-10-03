// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "update_ttl.h"

#include "utils/checksum.h"
#include "utils/ether.h"
#include "utils/ip.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;

void UpdateTTL::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);

    Ethernet *eth = pkt.head_data<Ethernet *>();
    Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);

    if (ip->ttl > 1) {
      // N to N-1 and 2 to 1 are identical for checksum purpose
      // We use constant numbers here for efficiency.
      ip->checksum = bess::utils::UpdateChecksum16(ip->checksum, 2, 1);
      ip->ttl -= 1;
      EmitPacket(ctx, pkt);
    } else {
      DropPacket(ctx, pkt);
    }
  }
}

ADD_MODULE(UpdateTTL, "update_ttl", "decreases the IP TTL field by 1")
