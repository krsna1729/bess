// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "macswap.h"

#include "utils/ether.h"

void MACSwap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::Ethernet;

  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    Ethernet *eth = batch->packet(i).head_data<Ethernet *>();
    Ethernet::Address tmp;

    tmp = eth->dst_addr;
    eth->dst_addr = eth->src_addr;
    eth->src_addr = tmp;
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(MACSwap, "macswap", "swaps source/destination MAC addresses")
