// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "track.h"

#include "message.h"
#include "stats/current_worker.h"

// Ethernet overhead in bytes
static const size_t kEthernetOverhead = 24;

const std::string Track::kName = bess::kTrackGateHookName;

Track::Track()
    : bess::GateHook(Track::kName, "track", Track::kPriority),
      track_bytes_(),
      counters_({"batches", "packets", "bytes"}) {}

CommandResponse Track::Init(const bess::Gate *, const bess::pb::TrackArg &arg) {
  track_bytes_ = arg.bits();
  return CommandSuccess();
}

Track::Totals Track::totals() const {
  const bess::stats::CounterSnapshot snap = counters_.Snapshot();
  return Totals{snap.totals[kBatches], snap.totals[kPackets],
                snap.totals[kBytes]};
}

void Track::ProcessBatch(const bess::PacketBatch *batch) {
  const size_t cnt = batch->cnt();
  uint64_t bytes = 0;
  if (track_bytes_) {
    for (bess::PacketRef pkt : batch->packets()) {
      bytes += pkt.data_len() + kEthernetOverhead;
    }
  }

  const auto update = counters_.Updating(bess::stats::CurrentWorkerId());
  update.Add(kBatches, 1);
  update.Add(kPackets, cnt);
  if (track_bytes_) {
    update.Add(kBytes, bytes);
  }
}

ADD_GATE_HOOK(Track, "track", "count the packets and batches")
