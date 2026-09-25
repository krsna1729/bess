// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_GATE_HOOKS_TRACK_
#define BESS_GATE_HOOKS_TRACK_

#include "../message.h"
#include "../module.h"
#include "../stats/counter_set.h"

// TrackGate counts the number of packets, batches and bytes seen by a gate.
//
// The counts are K6 worker-local counters: each worker adds to its own slot
// with no lock, a batch's three counts are one grouped update (a reading
// never sees a batch's packets without its bytes), and reset is a
// controller-side baseline, so it needs no worker pause.
class Track final : public bess::GateHook {
 public:
  Track();

  static const GateHookCommands cmds;

  CommandResponse Init(const bess::Gate *, const bess::pb::TrackArg &);

  struct Totals {
    uint64_t cnt;    // batches
    uint64_t pkts;
    uint64_t bytes;  // zero unless byte tracking is on
  };

  // One consistent reading of all three counts.
  Totals totals() const;

  uint64_t cnt() const { return totals().cnt; }
  uint64_t pkts() const { return totals().pkts; }
  uint64_t bytes() const { return totals().bytes; }

  void set_track_bytes(bool track) { track_bytes_ = track; }

  void ProcessBatch(const bess::PacketBatch *batch);

  CommandResponse CommandReset(const bess::pb::EmptyArg &);

  static constexpr uint16_t kPriority = 0;
  static const std::string kName;

 private:
  enum Counter : size_t { kBatches, kPackets, kBytes };

  bool track_bytes_;
  bess::stats::CounterSet counters_;
};

#endif  // BESS_GATE_HOOKS_TRACK_
