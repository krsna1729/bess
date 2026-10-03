// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_TIMESTAMP_H_
#define BESS_MODULES_TIMESTAMP_H_

#include "module.h"
#include "pb/module_msg.pb.h"

class Timestamp final : public Module {
 public:
  using MarkerType = uint32_t;
  static const MarkerType kMarker = 0x54C5BE55;

  Timestamp() : Module(), offset_(), attr_id_(-1) { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::TimestampArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  size_t offset_;
  int attr_id_;
};

#endif  // BESS_MODULES_TIMESTAMP_H_
