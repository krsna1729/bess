// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "timestamp.h"

#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/time.h"
#include "utils/udp.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;

static inline void timestamp_packet(bess::PacketRef pkt, size_t offset,
                                    uint64_t time) {
  Timestamp::MarkerType *marker;
  uint64_t *ts;

  const size_t kStampSize = sizeof(*marker) + sizeof(*ts);
  size_t room = pkt.data_len() - offset;

  if (room < kStampSize) {
    void *ret = pkt.append(kStampSize - room);
    if (!ret) {
      // not enough tailroom for timestamp. give up
      return;
    }
  }

  marker = pkt.head_data<Timestamp::MarkerType *>(offset);
  *marker = Timestamp::kMarker;
  ts = reinterpret_cast<uint64_t *>(marker + 1);
  *ts = time;
}

CommandResponse Timestamp::Init(const bess::pb::TimestampArg &arg) {
  if (arg.offset()) {
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

  int cnt = batch->cnt();
  if (attr_id_ != -1) {
    const bess::metadata::mt_offset_t off = attr_offset(attr_id_);
    if (bess::metadata::IsValidOffset(off)) {
      for (int i = 0; i < cnt; i++) {
        _set_attr_with_offset<uint64_t>(off, batch->packet(i), now_ns);
      }
    }
  } else {
    const size_t offset = offset_;
    for (int i = 0; i < cnt; i++) {
      timestamp_packet(batch->packet(i), offset, now_ns);
    }
  }
  RunNextModule(ctx, batch);
}

ADD_MODULE(Timestamp, "timestamp",
           "marks current time to packets (paired with Measure module)")
