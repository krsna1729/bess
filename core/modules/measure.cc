// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "measure.h"

#include <cstring>
#include <iterator>
#include <utility>

#include "packet_pool.h"
#include "timestamp.h"
#include "utils/common.h"
#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/time.h"
#include "utils/udp.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;

// Whether `pkt` carries Timestamp's in-packet stamp at `offset`, and its
// time. The stamp is read only from the first segment (D-121): a packet
// whose first segment ends before the stamp's end has none.
static bool IsTimestamped(bess::PacketRef pkt, size_t offset, uint64_t *time) {
  if (pkt.head_len() < offset + Timestamp::kStampSize) {
    return false;
  }
  const auto *stamp = pkt.head_data<const char *>() + offset;
  Timestamp::MarkerType marker;
  std::memcpy(&marker, stamp, sizeof(marker));
  if (marker == Timestamp::kMarker) {
    std::memcpy(time, stamp + sizeof(marker), sizeof(*time));
    return true;
  }
  return false;
}

const Commands Measure::cmds = {
    {"get_summary", "MeasureCommandGetSummaryArg",
     MODULE_CMD_FUNC(&Measure::CommandGetSummary), Command::THREAD_SAFE,
     "MeasureCommandGetSummaryResponse"},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&Measure::CommandClear),
     Command::THREAD_SAFE},
};

CommandResponse Measure::Init(const bess::pb::MeasureArg &arg) {
  uint64_t latency_ns_max = arg.latency_ns_max();
  uint64_t latency_ns_resolution = arg.latency_ns_resolution();
  if (latency_ns_max == 0) {
    latency_ns_max = kDefaultMaxNs;
  }
  if (latency_ns_resolution == 0) {
    latency_ns_resolution = kDefaultNsPerBucket;
  }
  uint64_t quotient = latency_ns_max / latency_ns_resolution;
  if ((latency_ns_max % latency_ns_resolution) != 0) {
    quotient += 1;  // absorb any remainder
  }
  if (quotient > rtt_hist_.max_num_buckets() / 2) {
    return CommandFailure(E2BIG,
                          "excessive latency_ns_max / latency_ns_resolution");
  }

  // Both histograms admitted before either is sized (D-198).
  num_buckets_ = quotient;
  bucket_width_ = latency_ns_resolution;
  const size_t bytes =
      2 * Histogram<uint64_t>::Bytes(num_buckets_, bucket_width_);
  hist_lease_ = init_context().memory().Lease("histograms", bytes, bytes);
  rtt_hist_.Resize(num_buckets_, bucket_width_);
  jitter_hist_.Resize(num_buckets_, bucket_width_);

  if (arg.offset()) {
    // The stamp must fit the packet data room (the default pool's), as
    // Timestamp requires: no packet could carry one past it. A DPDK data
    // room fits 16 bits, so the offset does too.
    const size_t data_room = bess::PacketPool::DefaultDataRoomSize();
    if (data_room < Timestamp::kStampSize || arg.offset() > data_room - Timestamp::kStampSize) {
      return CommandFailure(EINVAL, "offset %llu leaves no room for the %zu-byte stamp in the %zu-byte packet data room",
                            static_cast<unsigned long long>(arg.offset()), Timestamp::kStampSize,
                            data_room);
    }
    offset_ = arg.offset();
  } else {
    std::string attr_name = "timestamp";
    if (arg.attr_name() != "")
      attr_name = arg.attr_name();

    using AccessMode = bess::metadata::Attribute::AccessMode;
    // An absent metadata stamp falls back to the checked in-packet stamp.
    attr_id_ =
        AddMetadataAttr(attr_name, sizeof(uint64_t), AccessMode::kRead,
                        bess::metadata::Attribute::Contract::kBestEffort);
    if (attr_id_ < 0) {
      return CommandFailure(-attr_id_, "add_metadata_attr(%s) failed",
                            attr_name.c_str());
    }
  }

  if (arg.jitter_sample_prob()) {
    jitter_sample_prob_ = arg.jitter_sample_prob();
  } else {
    jitter_sample_prob_ = kDefaultIpDvSampleProb;
  }

  mcs_lock_init(&lock_);

  return CommandSuccess();
}

void Measure::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  // We don't use ctx->current_ns here for better accuracy
  uint64_t now_ns = tsc_to_ns(rdtsc());
  size_t offset = offset_;

  mcslock_node_t mynode;
  mcs_lock(&lock_, &mynode);

  pkt_cnt_ += batch->cnt();

  const bess::metadata::mt_offset_t ts_off =
      attr_id_ != -1 ? attr_offset(attr_id_) : bess::metadata::kMetadataOffsetNoRead;
  for (bess::PacketRef pkt : batch->packets()) {
    uint64_t pkt_time = get_attr_with_offset<uint64_t>(ts_off, pkt);
    if (pkt_time || IsTimestamped(pkt, offset, &pkt_time)) {
      uint64_t diff;

      if (now_ns >= pkt_time) {
        diff = now_ns - pkt_time;
      } else {
        // The magic number matched, but timestamp doesn't seem correct
        continue;
      }

      bytes_cnt_ += pkt.total_len();

      rtt_hist_.Insert(diff);
      if (rand_.GetRealNonzero() <= jitter_sample_prob_) {
        if (unlikely(!last_rtt_ns_)) {
          last_rtt_ns_ = diff;
          continue;
        }
        uint64_t jitter = absdiff(diff, last_rtt_ns_);
        jitter_hist_.Insert(jitter);
        last_rtt_ns_ = diff;
      }
    }
  }

  mcs_unlock(&lock_, &mynode);

  RunNextModule(ctx, batch);
}

template <typename T>
static void SetHistogram(
    bess::pb::MeasureCommandGetSummaryResponse::Histogram *r, const T &hist,
    uint64_t bucket_width) {
  r->set_count(hist.count);
  r->set_above_range(hist.above_range);
  r->set_resolution_ns(bucket_width);
  r->set_min_ns(hist.min);
  r->set_max_ns(hist.max);
  r->set_avg_ns(hist.avg);
  r->set_total_ns(hist.total);
  for (const auto &val : hist.percentile_values) {
    r->add_percentile_values_ns(val);
  }
}

void Measure::Clear() {
  // The replacements are admitted first; the old histograms' admission is
  // released after them (`lease` is declared first, so it goes last).
  const size_t bytes =
      2 * Histogram<uint64_t>::Bytes(num_buckets_, bucket_width_);
  bess::framework::MemoryLease lease =
      init_context().memory().Lease("histograms", bytes, bytes);
  // vector initialization is expensive thus should be out of critical section
  decltype(rtt_hist_) new_rtt_hist(num_buckets_, bucket_width_);
  decltype(jitter_hist_) new_jitter_hist(num_buckets_, bucket_width_);

  // Swap to minimize critical section; the old buckets are freed after it.
  mcslock_node_t mynode;
  mcs_lock(&lock_, &mynode);
  pkt_cnt_ = 0;
  bytes_cnt_ = 0;
  rtt_hist_.swap(new_rtt_hist);
  jitter_hist_.swap(new_jitter_hist);
  std::swap(hist_lease_, lease);
  mcs_unlock(&lock_, &mynode);
}

static bool IsValidPercentiles(const std::vector<double> &percentiles) {
  if (percentiles.empty()) {
    return true;
  }

  return std::is_sorted(percentiles.cbegin(), percentiles.cend()) &&
         *std::min_element(percentiles.cbegin(), percentiles.cend()) >= 0.0 &&
         *std::max_element(percentiles.cbegin(), percentiles.cend()) <= 100.0;
}

CommandResponse Measure::CommandGetSummary(
    const bess::pb::MeasureCommandGetSummaryArg &arg) {
  bess::pb::MeasureCommandGetSummaryResponse r;

  std::vector<double> latency_percentiles;
  std::vector<double> jitter_percentiles;

  std::copy(arg.latency_percentiles().begin(), arg.latency_percentiles().end(),
            back_inserter(latency_percentiles));
  std::copy(arg.jitter_percentiles().begin(), arg.jitter_percentiles().end(),
            back_inserter(jitter_percentiles));

  if (!IsValidPercentiles(latency_percentiles)) {
    return CommandFailure(EINVAL, "invalid 'latency_percentiles'");
  }

  if (!IsValidPercentiles(jitter_percentiles)) {
    return CommandFailure(EINVAL, "invalid 'jitter_percentiles'");
  }

  r.set_timestamp(get_epoch_time());
  r.set_packets(pkt_cnt_);
  r.set_bits((bytes_cnt_ + pkt_cnt_ * 24) * 8);
  const auto &rtt = rtt_hist_.Summarize(latency_percentiles);
  const auto &jitter = jitter_hist_.Summarize(jitter_percentiles);

  SetHistogram(r.mutable_latency(), rtt, rtt_hist_.bucket_width());
  SetHistogram(r.mutable_jitter(), jitter, jitter_hist_.bucket_width());

  if (arg.clear()) {
    // Note that some samples might be lost due to the small gap between
    // Summarize() and the next mcs_lock... but we posit that smaller
    // critical section is more important.
    Clear();
  }

  return CommandSuccess(r);
}

CommandResponse Measure::CommandClear(const bess::pb::EmptyArg &) {
  Clear();
  return CommandResponse();
}

ADD_MODULE(Measure, "measure",
           "measures packet latency (paired with Timestamp module)")
