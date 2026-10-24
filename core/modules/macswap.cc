// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "macswap.h"

#include "packet_reshape.h"
#include "utils/ether.h"

// The swap rewrites the first 12 bytes in place. A frame whose first segment
// does not hold an Ethernet header (a runt, or a chain split inside it) is
// dropped, never partly swapped (D-121, D-172); a shared payload is copied
// first, so the payload's other holders keep their frame (D-153), and a frame
// whose copy fails is dropped.
void MACSwap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::Ethernet;

  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    bess::PacketHandle handle = pkt.handle();
    const auto head = bess::packet::WritableHead(handle, sizeof(Ethernet));
    if (unlikely(!head)) {
      EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
      continue;
    }
    auto *eth = reinterpret_cast<Ethernet *>(head->data());
    const Ethernet::Address tmp = eth->dst_addr;
    eth->dst_addr = eth->src_addr;
    eth->src_addr = tmp;
    batch->handles()[out_cnt++] = handle;
  }
  batch->set_cnt(out_cnt);

  RunNextModule(ctx, batch);
}

ADD_MODULE(MACSwap, "macswap", "swaps source/destination MAC addresses")
