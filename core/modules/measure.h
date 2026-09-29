// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_MEASURE_H_
#define BESS_MODULES_MEASURE_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../utils/histogram.h"
#include "../utils/mcslock.h"
#include "../utils/random.h"

class Measure final : public Module {
 public:
  Measure(uint64_t ns_per_bucket = 1, uint64_t max_ns = 0)
      : Module(),
        rtt_hist_(max_ns / ns_per_bucket, ns_per_bucket),
        jitter_hist_(max_ns / ns_per_bucket, ns_per_bucket),
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

 private:
  static const uint64_t kDefaultNsPerBucket = 100;
  static const uint64_t kDefaultMaxNs = 100'000'000;  // 100 ms
  static constexpr double kDefaultIpDvSampleProb = 0.05;

  void Clear();

  Histogram<uint64_t> rtt_hist_;
  Histogram<uint64_t> jitter_hist_;

  Random rand_;
  double jitter_sample_prob_;
  uint64_t last_rtt_ns_;

  size_t offset_;  // in bytes
  int attr_id_;

  uint64_t pkt_cnt_;
  uint64_t bytes_cnt_;

  mcslock lock_;
};

#endif  // BESS_MODULES_MEASURE_H_
