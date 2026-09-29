// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "bypass.h"

CommandResponse Bypass::Init(const bess::pb::BypassArg &arg) {
  cycles_per_batch_ = arg.cycles_per_batch();
  cycles_per_packet_ = arg.cycles_per_packet();
  cycles_per_byte_ = arg.cycles_per_byte();

  return CommandSuccess();
}

void Bypass::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  uint64_t start_tsc = rdtsc();
  uint64_t cycles = cycles_per_batch_ + cycles_per_packet_ * batch->cnt();

  if (cycles_per_byte_) {
    uint64_t total_bytes = 0;
    int cnt = batch->cnt();
    for (int i = 0; i < cnt; i++) {
      total_bytes = batch->packet(i).total_len();
    }
    cycles += cycles_per_byte_ * total_bytes;
  }

  if (cycles) {
    uint64_t target_tsc = start_tsc + cycles;
    // burn cycles until it comsumes target cycles
    while (rdtsc() < target_tsc) {
      _mm_pause();
    }
  }
  RunChooseModule(ctx, ctx->current_igate, batch);
}

ADD_MODULE(Bypass, "bypass", "bypasses packets without any processing")
