// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "dump.h"

#include <cmath>
#include <iostream>

#include <rte_hexdump.h>

#define NS_PER_SEC 1000000000ull

static const uint64_t DEFAULT_INTERVAL_NS = 1 * NS_PER_SEC; /* 1 sec */

const Commands Dump::cmds = {
    {"set_interval", "DumpArg", MODULE_CMD_FUNC(&Dump::CommandSetInterval),
     Command::THREAD_UNSAFE},
};

CommandResponse Dump::Init(const bess::pb::DumpArg &arg) {
  // cannot get a context-based current nanoseconds
  min_interval_ns_ = DEFAULT_INTERVAL_NS;
  next_ns_ = tsc_to_ns(rdtsc());
  if (arg.interval())
    return CommandSetInterval(arg);
  return CommandSuccess();
}

void Dump::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (unlikely(ctx->current_ns >= next_ns_)) {
    bess::PacketRef pkt = batch->packet(0);

    printf("----------------------------------------\n");
    printf("%s: packet dump\n", name().c_str());
    // PacketRef owns the native-mbuf debug formatting.
    std::cout << pkt.Dump();
    rte_hexdump(stdout, "Metadata buffer", pkt.metadata<const char *>(),
                SNBUF_METADATA);
    next_ns_ = ctx->current_ns + min_interval_ns_;
  }

  RunChooseModule(ctx, ctx->current_igate, batch);
}

CommandResponse Dump::CommandSetInterval(const bess::pb::DumpArg &arg) {
  double sec = arg.interval();

  if (std::isnan(sec) || sec <= 0.0) {
    return CommandFailure(EINVAL, "invalid interval");
  }

  min_interval_ns_ = static_cast<uint64_t>(sec * NS_PER_SEC);
  return CommandSuccess();
}

ADD_MODULE(Dump, "dump", "Dump packet data and metadata attributes")
