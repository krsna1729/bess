// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_BUFFER_H_
#define BESS_MODULES_BUFFER_H_

#include "module.h"

/* TODO: timer-triggered flush */
class Buffer final : public Module {
 public:
  Buffer() : Module(), buf_() {}

  CommandResponse Init(const bess::pb::BufferArg &) { return CommandSuccess(); }

  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  bess::PacketBatch buf_;
};

#endif  // BESS_MODULES_BUFFER_H_
