// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "timestamp.h"

#include <cerrno>
#include <cstring>

#include "packet_mutation.h"
#include "packet_pool.h"
#include "packet_reshape.h"
#include "utils/time.h"

namespace {

// Whether the stamp at [offset, required) can be placed in `pkt`'s first
// segment: it is there already, or the packet is one segment whose data
// reaches the stamp's start, so an append makes room. An append grows a
// chain's last segment, not its first; and growing a packet whose data ends
// before the stamp would put the stale bytes between them on the wire.
inline bool CanPlace(bess::PacketRef pkt, size_t offset, size_t required) {
  return pkt.head_len() >= required || (pkt.nb_segs() == 1 && pkt.head_len() >= offset);
}

// Writes the stamp (the marker, then the time) at `offset` of the packet's
// first segment; false when the packet is to be dropped (D-172: a packet too
// short for the write is dropped, never forwarded without it -- a reader of
// the stamp would find whatever its buffer held there): one the stamp cannot
// be placed in, one whose single segment has no tailroom for the stamp's end,
// and a shared one whose copy fails. A shared payload is copied first
// (D-153), which may replace the packet.
inline bool StampPacket(bess::PacketHandle &handle, size_t offset, uint64_t time) {
  const size_t required = offset + Timestamp::kStampSize;
  if (!CanPlace(bess::PacketRef(handle), offset, required)) {
    return false;
  }
  if (bess::packet::PayloadWriteabilityOf(bess::PacketRef(handle)) !=
      bess::packet::PayloadWriteability::kWritable) {
    if (!bess::packet::EnsureWritable(handle) ||
        bess::packet::PayloadWriteabilityOf(bess::PacketRef(handle)) !=
            bess::packet::PayloadWriteability::kWritable ||
        !CanPlace(bess::PacketRef(handle), offset, required)) {  // the copy was resegmented
      return false;
    }
  }
  bess::PacketRef pkt(handle);
  if (pkt.head_len() < required &&
      pkt.append(static_cast<uint16_t>(required - pkt.head_len())) == nullptr) {
    return false;  // not enough tailroom
  }
  // Init bounded offset + kStampSize by the data room: it fits 16 bits.
  auto *stamp = pkt.head_data<char *>(static_cast<uint16_t>(offset));
  const Timestamp::MarkerType marker = Timestamp::kMarker;
  std::memcpy(stamp, &marker, sizeof(marker));
  std::memcpy(stamp + sizeof(marker), &time, sizeof(time));
  return true;
}

}  // namespace

CommandResponse Timestamp::Init(const bess::pb::TimestampArg &arg) {
  if (arg.offset()) {
    // The stamp must fit the packet data room (the default pool's): the
    // offset is then also a 16-bit one.
    const size_t data_room = bess::PacketPool::DefaultDataRoomSize();
    if (data_room < kStampSize || arg.offset() > data_room - kStampSize) {
      return CommandFailure(EINVAL, "offset %llu leaves no room for the %zu-byte stamp in the %zu-byte packet data room",
                            static_cast<unsigned long long>(arg.offset()), kStampSize, data_room);
    }
    offset_ = arg.offset();
  } else {
    std::string attr_name = "timestamp";
    if (arg.attr_name() != "")
      attr_name = arg.attr_name();

    using AccessMode = bess::metadata::Attribute::AccessMode;
    attr_id_ = AddMetadataAttr(attr_name, sizeof(uint64_t), AccessMode::kWrite);
  }
  return CommandSuccess();
}

void Timestamp::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  // We don't use ctx->current_ns here for better accuracy
  uint64_t now_ns = tsc_to_ns(rdtsc());

  if (attr_id_ != -1) {
    const bess::metadata::mt_offset_t off = attr_offset(attr_id_);
    if (bess::metadata::IsValidOffset(off)) {
      for (bess::PacketRef pkt : batch->packets()) {
        _set_attr_with_offset<uint64_t>(off, pkt, now_ns);
      }
    }
  } else {
    const size_t offset = offset_;
    int out_cnt = 0;
    for (bess::PacketRef pkt : batch->packets()) {
      bess::PacketHandle handle = pkt.handle();
      if (!StampPacket(handle, offset, now_ns)) {
        EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
        continue;
      }
      batch->handles()[out_cnt++] = handle;
    }
    batch->set_cnt(out_cnt);
  }
  RunNextModule(ctx, batch);
}

ADD_MODULE(Timestamp, "timestamp",
           "marks current time to packets (paired with Measure module)")
