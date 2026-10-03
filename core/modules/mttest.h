// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_MTTEST_H_
#define BESS_MODULES_MTTEST_H_

#include "module.h"
#include "pb/module_msg.pb.h"

using bess::metadata::Attribute;

class MetadataTest final : public Module {
 public:
  MetadataTest() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }
  static const gate_idx_t kNumIGates = MAX_GATES;
  static const gate_idx_t kNumOGates = MAX_GATES;

  CommandResponse Init(const bess::pb::MetadataTestArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  CommandResponse AddAttributes(
      const google::protobuf::Map<std::string, int64_t> &attrs,
      Attribute::AccessMode mode);
};

#endif  // BESS_MODULES_MTTEST_H_
