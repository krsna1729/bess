// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "generic_decap.h"

#include "packet_mutation.h"

CommandResponse GenericDecap::Init(const bess::pb::GenericDecapArg &arg) {
  if (arg.bytes() == 0) {
    return CommandSuccess();
  }
  decap_size_ = arg.bytes();
  if (decap_size_ <= 0 || decap_size_ > 1024) {
    return CommandFailure(EINVAL, "invalid decap size");
  }
  return CommandSuccess();
}

// Strips the first `bytes` of each packet in its first segment
// (packet::RemovePrefixInPlace; no byte is written, so a shared payload is
// stripped as it is). A packet whose first segment holds fewer bytes (a
// short packet, or a chain whose first segment ends inside them, D-121) is
// dropped (D-172), never sent on with the header still on.
void GenericDecap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const auto decap_size = static_cast<size_t>(decap_size_);

  const int cnt = batch->cnt();
  int out_cnt = 0;
  for (int i = 0; i < cnt; i++) {
    const bess::PacketRef pkt = batch->packet(i);
    if (unlikely(!bess::packet::RemovePrefixInPlace(pkt, decap_size))) {
      DropPacket(ctx, pkt);
      continue;
    }
    batch->handles()[out_cnt++] = pkt.handle();
  }
  batch->set_cnt(out_cnt);

  RunNextModule(ctx, batch);
}

ADD_MODULE(GenericDecap, "generic_decap",
           "remove specified bytes from the beginning of packets")
