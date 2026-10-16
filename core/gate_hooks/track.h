// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_GATE_HOOKS_TRACK_
#define BESS_GATE_HOOKS_TRACK_

#include "message.h"
#include "module.h"
#include "stats/counter_set.h"

// TrackGate counts the number of packets and batches seen by a gate.
//
// The counts are K6 worker-local counters: each worker adds to its own slot
// with no lock, and a batch's two counts are one grouped update (a reading
// never sees a batch's packets without its batches).
class Track final : public bess::GateHook {
 public:
  Track();

  CommandResponse Init(const bess::Gate *, const bess::pb::TrackArg &);

  struct Totals {
    uint64_t cnt;  // batches
    uint64_t pkts;
  };

  // One consistent reading of both counts.
  Totals totals() const;

  uint64_t cnt() const { return totals().cnt; }
  uint64_t pkts() const { return totals().pkts; }

  void ProcessBatch(const bess::PacketBatch *batch);

  static constexpr uint16_t kPriority = 0;
  static const std::string kName;

 private:
  enum Counter : size_t { kBatches, kPackets };

  bess::stats::CounterSet counters_;
};

#endif  // BESS_GATE_HOOKS_TRACK_
