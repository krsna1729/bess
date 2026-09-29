// Copyright (c) 2014-2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_SEQUENTIALUPDATE_H_
#define BESS_MODULES_SEQUENTIALUPDATE_H_

#include "module.h"
#include "utils/endian.h"

#include "pb/supdate_msg.pb.h"

static const size_t kMaxVariable = 16;

class SequentialUpdate final : public Module {
public:
  static const Commands cmds;

  SequentialUpdate() : Module(), num_vars_(), vars_() {}

  CommandResponse Init(const sample::supdate::pb::SequentialUpdateArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse
  CommandAdd(const sample::supdate::pb::SequentialUpdateArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

private:
  size_t num_vars_;

  struct {
    bess::utils::be32_t mask; // bits with 1 won't be updated
    uint32_t min;
    uint32_t range; // max - min + 1
    uint32_t cur;
    size_t offset;
    size_t bit_shift;
  } vars_[kMaxVariable];
};

#endif // BESS_MODULES_SEQUENTIALUPDATE_H_
