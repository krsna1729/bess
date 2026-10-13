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
     Command::THREAD_UNSAFE, "", Command::LEGACY_ONLY},
};

const ResourceTables Dump::resource_tables = {
    ResourceTable::Singleton<bess::pb::DumpArg>("settings"),
};

namespace {

// set_interval's validation: a positive number of seconds.
bool ValidInterval(double sec) {
  return !std::isnan(sec) && sec > 0.0;
}

}  // namespace

CommandResponse Dump::Init(const bess::pb::DumpArg &arg) {
  // cannot get a context-based current nanoseconds
  min_interval_ns_.store(DEFAULT_INTERVAL_NS, std::memory_order_relaxed);
  interval_sec_ = 1.0;
  next_ns_ = tsc_to_ns(rdtsc());
  if (arg.interval()) {
    CommandResponse err = CommandSetInterval(arg);
    if (err.error().code() != 0) {
      return err;
    }
  }
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [](const bess::pb::DumpArg &value) -> std::expected<double, std::string> {
        if (!ValidInterval(value.interval())) {
          return std::unexpected("invalid interval");
        }
        return value.interval();
      },
      [this](double &sec) {
        interval_sec_ = sec;
        min_interval_ns_.store(static_cast<uint64_t>(sec * NS_PER_SEC),
                               std::memory_order_relaxed);
      },
      [this] {
        bess::pb::DumpArg value;
        value.set_interval(interval_sec_);
        return value;
      },
      nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Dump::DeInit() {
  settings_.Release();
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
    next_ns_ = ctx->current_ns +
               min_interval_ns_.load(std::memory_order_relaxed);
  }

  RunChooseModule(ctx, ctx->current_igate, batch);
}

CommandResponse Dump::CommandSetInterval(const bess::pb::DumpArg &arg) {
  double sec = arg.interval();

  if (!ValidInterval(sec)) {
    return CommandFailure(EINVAL, "invalid interval");
  }

  interval_sec_ = sec;
  min_interval_ns_.store(static_cast<uint64_t>(sec * NS_PER_SEC),
                         std::memory_order_relaxed);
  return CommandSuccess();
}

ADD_MODULE(Dump, "dump", "Dump packet data and metadata attributes")
