// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "split.h"

#include "../utils/bits.h"
#include "../utils/endian.h"

CommandResponse Split::Init(const bess::pb::SplitArg &arg) {
  size_ = arg.size();
  if (size_ < 1 || size_ > sizeof(uint64_t)) {
    return CommandFailure(EINVAL, "'size' must be 1-%zu", sizeof(uint64_t));
  }

  mask_ = bess::utils::SetBitsHigh<uint64_t>(size_ * 8);

  // We read a be64_t value regardless of the actual size,
  // hence the read value needs bit shift to the right.
  shift_ = 64 - (size_ * 8);

  if (arg.type_case() == bess::pb::SplitArg::kAttribute) {
    attr_id_ = AddMetadataAttr(arg.attribute().c_str(), size_,
                               bess::metadata::Attribute::AccessMode::kRead);
    if (attr_id_ < 0) {
      return CommandFailure(-attr_id_, "add_metadata_attr() failed");
    }
  } else {
    attr_id_ = -1;
    offset_ = arg.offset();
    if (offset_ > 1024) {
      return CommandFailure(EINVAL, "invalid 'offset'");
    }
  }
  return CommandSuccess();
}

void Split::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be64_t;

  int cnt = batch->cnt();

  if (attr_id_ >= 0) {
    bess::metadata::mt_offset_t offset = attr_offset(attr_id_);
    for (int i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      uint64_t val = get_attr_with_offset<be64_t>(offset, pkt).value();
      val = (val >> shift_) & mask_;
      EmitPacket(ctx, pkt, val);
    }
  } else {
    for (int i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      uint64_t val = (pkt.head_data<be64_t *>(offset_))->value();
      val = (val >> shift_) & mask_;
      EmitPacket(ctx, pkt, val);
    }
  }
}

ADD_MODULE(Split, "split",
           "split packets depending on packet data or metadata attributes")
