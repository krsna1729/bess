// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "buffer.h"

void Buffer::DeInit() {
  bess::PacketBatch *buf = &buf_;
  bess::PacketFreeBatch(buf);
}

void Buffer::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  bess::PacketBatch *buf = &buf_;

  int free_slots = bess::PacketBatch::kMaxBurst - buf->cnt();
  int left = batch->cnt();

  bess::PacketHandle *p_buf = &buf->handles()[buf->cnt()];
  bess::PacketHandle *p_batch = &batch->handles()[0];

  if (left >= free_slots) {
    buf->set_cnt(bess::PacketBatch::kMaxBurst);
    bess::utils::CopyInlined(p_buf, p_batch,
                             free_slots * sizeof(bess::PacketHandle));

    p_buf = &buf->handles()[0];
    p_batch += free_slots;
    left -= free_slots;

    bess::PacketBatch *new_batch = ctx->task->AllocPacketBatch();
    new_batch->Copy(buf);
    buf->clear();
    RunNextModule(ctx, new_batch);
  }

  buf->incr_cnt(left);
  bess::utils::CopyInlined(p_buf, p_batch,
                           left * sizeof(bess::PacketHandle));
}

ADD_MODULE(Buffer, "buffer", "buffers packets into larger batches")
