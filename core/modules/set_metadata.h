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

  // Compiles `ops_` from the attributes and their offsets, which a resume
  // assigns just before.
  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  // One attribute write, as the packet path runs it: `fn` is specialized by
  // source (a value, or packet bytes with or without shift and mask) and, for
  // 1, 2, 4 and 8 bytes, by size.
  struct Op {
    void (*fn)(bess::PacketBatch *batch, const Op &op);
    mt_offset_t mt_offset;
    const Attr *attr;
  };

  template <size_t N>
  static void FromValue(bess::PacketBatch *batch, const Op &op);
  template <size_t N, bool do_shift, bool do_mask>
  static void FromPacket(bess::PacketBatch *batch, const Op &op);
  template <size_t N>
  static decltype(Op::fn) SelectSized(const Attr &attr);
  static decltype(Op::fn) Select(const Attr &attr);

  CommandResponse AddAttrOne(const bess::pb::SetMetadataArg_Attribute &attr);

  std::vector<struct Attr> attrs_;
  // Attributes without a valid offset (no reader downstream) are left out.
  std::vector<Op> ops_;

  bess::pb::SetMetadataArg init_arg_;
};

#endif  // BESS_MODULES_SETMETADATA_H_
