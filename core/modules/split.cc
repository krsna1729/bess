// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "split.h"

#include <cstdint>
#include <cstring>

#include "packet_pool.h"
#include "utils/bits.h"
#include "utils/endian.h"

namespace {

// The `size` bytes at `p` as a big-endian value, one byte at a time: for a
// field with fewer than 8 bytes from its start to the end of the first
// segment (`offset + 8 > head_len`), where the 8-byte load would read past it.
[[gnu::noinline]] uint64_t ReadNarrow(const uint8_t *p, size_t size) {
  uint64_t v = 0;
  for (size_t i = 0; i < size; i++) {
    v = v << 8u | uint64_t{p[i]};
  }
  return v;
}

}  // namespace

CommandResponse Split::Init(const bess::pb::SplitArg &arg) {
  size_ = arg.size();
  if (size_ < 1 || size_ > sizeof(uint64_t)) {
    return CommandFailure(EINVAL, "'size' must be 1-%zu", sizeof(uint64_t));
  }

  mask_ = bess::utils::SetBitsHigh<uint64_t>(size_ * 8);

  // The common case reads a be64_t value regardless of the actual size,
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
    // The value must fit the packet data room (the default pool's): no
    // packet could hold one past it.
    const size_t room = bess::PacketPool::DefaultDataRoomSize();
    if (offset_ + size_ > room) {
      return CommandFailure(EINVAL, "'offset' %zu + 'size' %zu is past the %zu-byte packet data room",
                            offset_, size_, room);
    }
  }
  return CommandSuccess();
}

void Split::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be64_t;

  if (attr_id_ >= 0) {
    bess::metadata::mt_offset_t offset = attr_offset(attr_id_);
    for (bess::PacketRef pkt : batch->packets()) {
      uint64_t val = get_attr_with_offset<be64_t>(offset, pkt).value();
      val = (val >> shift_) & mask_;
      EmitPacket(ctx, pkt, val);
    }
  } else {
    // The value is read from the first segment (D-121). A packet whose first
    // segment ends before offset + size is dropped: it has no value to pick
    // a gate by.
    const size_t end = offset_ + size_;
    const size_t wide_end = offset_ + sizeof(uint64_t);
    for (bess::PacketRef pkt : batch->packets()) {
      const size_t len = pkt.head_len();
      const auto *field = pkt.head_data<const uint8_t *>() + offset_;
      uint64_t val;
      if (likely(len >= wide_end)) {
        be64_t wide;
        std::memcpy(&wide, field, sizeof(wide));
        val = (wide.value() >> shift_) & mask_;
      } else if (len >= end) {
        val = ReadNarrow(field, size_);
      } else {
        DropPacket(ctx, pkt);
        continue;
      }
      EmitPacket(ctx, pkt, val);
    }
  }
}

ADD_MODULE(Split, "split",
           "split packets depending on packet data or metadata attributes")
