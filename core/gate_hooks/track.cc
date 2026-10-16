// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "track.h"

#include "message.h"
#include "stats/current_worker.h"

const std::string Track::kName = bess::kTrackGateHookName;

Track::Track()
    : bess::GateHook(Track::kName, "track", Track::kPriority),
      counters_({"batches", "packets"}) {}

CommandResponse Track::Init(const bess::Gate *, const bess::pb::TrackArg &) {
  return CommandSuccess();
}

Track::Totals Track::totals() const {
  const bess::stats::CounterSnapshot snap = counters_.Snapshot();
  return Totals{snap.totals[kBatches], snap.totals[kPackets]};
}

void Track::ProcessBatch(const bess::PacketBatch *batch) {
  const size_t cnt = batch->cnt();

  const auto update = counters_.Updating(bess::stats::CurrentWorkerId());
  update.Add(kBatches, 1);
  update.Add(kPackets, cnt);
}

ADD_GATE_HOOK(Track, "track", "count the packets and batches")
