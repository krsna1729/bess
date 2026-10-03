// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_SETMETADATA_H_
#define BESS_MODULES_SETMETADATA_H_

#include "module.h"
#include "pb/module_msg.pb.h"

using bess::metadata::kMetadataAttrMaxSize;
using bess::metadata::mt_offset_t;

typedef struct {
  uint8_t bytes[kMetadataAttrMaxSize];
} value_t;
typedef struct {
  uint8_t bytes[kMetadataAttrMaxSize];
} mask_t;

struct Attr {
  std::string name;
  value_t value;
  mask_t mask;
  int offset;
  size_t size;
  bool do_mask;
  int shift;  // in bytes for now
};

class SetMetadata final : public Module {
 public:
  static const Commands cmds;

  SetMetadata() : Module(), attrs_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::SetMetadataArg &arg);
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  enum class Mode { FromPacket, FromValue };

  template <Mode mode = Mode::FromValue>
  inline void DoProcessBatch(bess::PacketBatch *batch, const struct Attr *attr,
                             mt_offset_t mt_offset);

  CommandResponse AddAttrOne(const bess::pb::SetMetadataArg_Attribute &attr);

  std::vector<struct Attr> attrs_;

  bess::pb::SetMetadataArg init_arg_;
};

#endif  // BESS_MODULES_SETMETADATA_H_
