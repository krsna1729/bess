// Copyright (c) 2026, The Regents of the University of California.
// SPDX-License-Identifier: BSD-3-Clause

#include "usage_report.h"

#include <algorithm>
#include <chrono>
#include <expected>
#include <string>
#include <utility>

#include "framework/plugin.h"
#include "utils/endian.h"

namespace {
// The cells a key carries: octets first, then packets. A quota watches octets,
// the cell a per-packet compare is about; the packet count is metered beside it,
// for the report a deployment bills from.
constexpr size_t kOctets = 0;
constexpr size_t kPackets = 1;
constexpr size_t kCells = 2;
// The packet path's "no quota": a key with no quota is metered and compared
// against zero, which is one load and one compare.
constexpr uint64_t kNoQuota = 0;
// The sweep's period when the creation argument does not state one.
constexpr uint64_t kDefaultTickPeriodMs = 1000;
}  // namespace

// D-140: every legacy command is gone. "<name>/quotas" holds the quota set (the
// whole set, as UsageQuotas); the creation argument is GetPipeline's Module.arg.
const Commands UsageReport::cmds = {};

// requires_paused_workers=false: applying a quota set stores relaxed atomics
// the packet path reads and rebuilds the tick's own table, both under the
// control-plane lock, so workers run through it (docs/plugin-api.md). The
// Register below states the same thing by passing no worker_running condition;
// the daemon refuses a table and a codec that disagree.
const ResourceTables UsageReport::resource_tables = {
    ResourceTable::Singleton<sample::usage::pb::UsageQuotas>(
        "quotas", /*requires_paused_workers=*/false)};

std::expected<UsageReport::Staged, std::string> UsageReport::StageQuotas(
    const sample::usage::pb::UsageQuotas &v) {
  Staged staged;
  staged.quotas.reserve(v.quotas_size());
  for (const sample::usage::pb::UsageQuota &q : v.quotas()) {
    if (q.octets() == 0) {
      return std::unexpected("quota for key " + std::to_string(q.key()) +
                             " is zero: leave the key out to disarm it");
    }
    staged.quotas.emplace_back(q.key(), q.octets());
  }
  staged.set = v;
  return staged;
}

CommandResponse UsageReport::Init(const sample::usage::pb::UsageReportArg &arg) {
  if (arg.keys() == 0) {
    return CommandFailure(EINVAL, "keys must be at least 1");
  }
  if (arg.key_attr().empty()) {
    return CommandFailure(EINVAL,
                          "key_attr is required: it is where a packet's key "
                          "arrives, and the module has no other way to know "
                          "whose volume a packet is");
  }
  keys_ = arg.keys();
  mode_ = arg.mode();

  key_attr_ =
      AddMetadataAttr(arg.key_attr(), sizeof(uint64_t),
                      bess::metadata::Attribute::AccessMode::kRead);
  if (key_attr_ < 0) {
    return CommandFailure(EINVAL, "key_attr '%s' is not a valid attribute",
                          arg.key_attr().c_str());
  }

  // One plane per worker. The daemon launches workers after the pipeline is
  // built, so the count cannot be read here; the controller states it, and 0
  // means every slot. A count that is too small is a constraint failure before
  // traffic runs (module.cc CheckModuleConstraints), not a write past the
  // planes.
  const size_t workers =
      arg.workers() == 0 ? bess::dataplane::kMaxWorkers : arg.workers();
  max_allowed_workers_ = static_cast<int>(workers);
  counters_ =
      std::make_unique<bess::stats::KeyedCounters>(keys_, kCells, workers);
  quotas_ = std::make_unique<std::atomic<uint64_t>[]>(keys_);
  for (size_t key = 0; key < keys_; key++) {
    quotas_[key].store(kNoQuota, std::memory_order_relaxed);
  }

  // The packet path's reporter, and the control side's tick when the mode
  // sweeps.
  notifier_ = bess::stats::CrossingNotifier(init_context().events(), name());
  if (mode_ != sample::usage::pb::USAGE_REPORT_EVENTS) {
    const uint64_t period_ms =
        arg.tick_period_ms() == 0 ? kDefaultTickPeriodMs : arg.tick_period_ms();
    tick_ = bess::framework::TickEndpoint(
        init_context().ticks(), std::chrono::milliseconds(period_ms),
        [this](bess::framework::TickContext &ctx) { Sweep(ctx); });
  }

  auto registered = quotas_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/quotas",
      [](const sample::usage::pb::UsageQuotas &v)
          -> std::expected<Staged, std::string> { return StageQuotas(v); },
      [this](Staged &s) {
        // A key the previous set armed and this one leaves out is disarmed; a
        // key in both keeps its limit, so it never reads zero in between. The
        // packet path is not held by the control-plane lock, so that window
        // would be a missed compare (the sweep is the backstop for one).
        std::vector<uint64_t> keep;
        keep.reserve(s.quotas.size());
        for (const auto &[key, limit] : s.quotas) {
          keep.push_back(key);
        }
        std::sort(keep.begin(), keep.end());
        for (const sample::usage::pb::UsageQuota &q : arg_.quotas()) {
          if (q.key() < keys_ &&
              !std::binary_search(keep.begin(), keep.end(), q.key())) {
            quotas_[q.key()].store(kNoQuota, std::memory_order_relaxed);
          }
        }
        watches_.Clear();
        for (const auto &[key, limit] : s.quotas) {
          Arm(key, limit);
        }
        arg_.Swap(&s.set);
      },
      [this] { return arg_; }, nullptr);
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void UsageReport::DeInit() {
  quotas_resource_.Release();
}

void UsageReport::Arm(uint64_t key, uint64_t limit) {
  if (key >= keys_) {
    return;  // outside the key space: nothing to watch
  }
  quotas_[key].store(limit, std::memory_order_relaxed);
  watches_.Arm(key, limit);
}

void UsageReport::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const bess::metadata::mt_offset_t key_off =
      key_attr_ != -1 ? attr_offset(key_attr_)
                      : bess::metadata::kMetadataOffsetNoRead;
  if (key_off == bess::metadata::kMetadataOffsetNoRead) [[unlikely]] {
    RunNextModule(ctx, batch);
    return;
  }

  bess::stats::KeyedCounters::Plane plane = counters_->ForWorker(ctx->wid);
  const size_t cnt = batch->cnt();
  for (size_t i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    // The key is a 64-bit value in network byte order, which is what the
    // graph's writers use (SetMetadata's value_int, the match modules' values:
    // utils::uint64_to_bin with big_endian true).
    const uint64_t key =
        get_attr_with_offset<bess::utils::be64_t>(key_off, pkt).value();
    if (key >= keys_) [[unlikely]] {
      continue;  // a key outside the space: metered nowhere
    }
    const uint64_t len = pkt.data_len();
    const uint64_t limit = QuotaOf(key);
    if (limit == kNoQuota) [[likely]] {
      plane.Add(key, kOctets, len);
      plane.Add(key, kPackets, 1);
      continue;
    }
    // Armed: the worker's own plane is what it compares, and the compare is
    // what tells it the key crossed. One event per arming, not one per packet.
    const uint64_t before = counters_->Read(ctx->wid, key, kOctets);
    plane.Add(key, kOctets, len);
    plane.Add(key, kPackets, 1);
    if (bess::stats::CrossingNotifier::Crossed(before, before + len, limit))
        [[unlikely]] {
      notifier_.Note(ctx->wid, key, before + len, limit);
    }
  }
  RunNextModule(ctx, batch);
}

void UsageReport::Sweep(bess::framework::TickContext &ctx) {
  const bess::framework::ThresholdTable::Result result = watches_.Sweep(
      [this](uint64_t key) -> std::optional<uint64_t> {
        // The total over every plane: the answer a per-worker compare cannot
        // give when a key's traffic is spread across workers.
        return counters_->Total(key, kOctets);
      },
      [&ctx] { return ctx.Expired(); });
  bess::framework::ReportCrossings(ctx, name(), result);
  // result.gone is empty here: the key space is fixed at Init and no key leaves
  // it, so every armed key still has a count.
}

std::string UsageReport::GetDesc() const {
  return "UsageReport: " + std::to_string(keys_) + " keys, " +
         std::to_string(counters_ ? counters_->workers() : 0) + " planes, " +
         sample::usage::pb::UsageReportMode_Name(mode_);
}

ADD_MODULE(UsageReport, "usage_report",
           "meters octets and packets per key and reports quota crossings")
