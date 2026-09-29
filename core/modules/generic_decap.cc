// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "generic_decap.h"

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

void GenericDecap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();

  int decap_size = decap_size_;

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    pkt.adj(decap_size);
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(GenericDecap, "generic_decap",
           "remove specified bytes from the beginning of packets")
