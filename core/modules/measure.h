// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_MEASURE_H_
#define BESS_MODULES_MEASURE_H_

#include "framework/memory_scope.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "utils/histogram.h"
#include "utils/mcslock.h"
#include "utils/random.h"

class Measure final : public Module {
 public:
  // The histograms hold one (above-range) bucket until Init admits and
  // sizes them (D-198).
  Measure()
      : Module(),
        rtt_hist_(0, kDefaultNsPerBucket),
        jitter_hist_(0, kDefaultNsPerBucket),
        rand_(),
        jitter_sample_prob_(),
        last_rtt_ns_(),
        offset_(),
        attr_id_(-1),
        pkt_cnt_(),
        bytes_cnt_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::MeasureArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandGetSummary(
      const bess::pb::MeasureCommandGetSummaryArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

  static const Commands cmds;
  // Every worker updates one sampler (RNG, last RTT, histograms) under lock_.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kShared,
                                                    ModuleCapabilities::FlowAffinity::kNone};

 private:
  static const uint64_t kDefaultNsPerBucket = 100;
  static const uint64_t kDefaultMaxNs = 100'000'000;  // 100 ms
  static constexpr double kDefaultIpDvSampleProb = 0.05;

  // Replaces both histograms with empty ones of Init's size, admitted before
  // they are built (throws MemoryRefused, nothing changed, when refused).
  void Clear();

  // The admission of both histograms' buckets ("histograms", glibc, D-198):
  // declared before them, so it is released after they are freed.
  bess::framework::MemoryLease hist_lease_;
  Histogram<uint64_t> rtt_hist_;
  Histogram<uint64_t> jitter_hist_;
  // Init's size of each histogram: latency_ns_max / latency_ns_resolution
  // buckets (plus the above-range one) of latency_ns_resolution ns.
  size_t num_buckets_ = 0;
  uint64_t bucket_width_ = kDefaultNsPerBucket;

  Random rand_;
  double jitter_sample_prob_;
  uint64_t last_rtt_ns_;

  size_t offset_;  // in bytes; Init bounds offset_ + the stamp by the data room
  int attr_id_;

  uint64_t pkt_cnt_;
  uint64_t bytes_cnt_;

  mcslock lock_;
};

#endif  // BESS_MODULES_MEASURE_H_
