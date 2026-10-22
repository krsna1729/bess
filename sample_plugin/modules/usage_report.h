// Copyright (c) 2026, The Regents of the University of California.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_USAGEREPORT_H_
#define BESS_MODULES_USAGEREPORT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "framework/module_tick.h"
#include "framework/singleton_resource.h"
#include "framework/threshold_table.h"
#include "module.h"
#include "stats/crossing_notifier.h"
#include "stats/keyed_counters.h"

#include "pb/usage_msg.pb.h"

// The worked example of the counter-watch pattern (D-150, D-151, D-152, D-154),
// and its use case is the one that drove the pattern: a UPF-like deployment
// meters each subscriber's volume and wants a report when a subscriber reaches
// a quota, without reading every counter every second.
//
// Volume. `KeyedCounters`: one plane per worker, sized to the workers this
// pipeline will run on, so a module on 8 workers pays for 8 planes and not for
// the 64 worker slots the daemon reserves. A packet's key is the dense slot
// index its metadata carries; the worker that saw the packet adds its length to
// the key's octets cell and 1 to its packets cell, in its own plane, so no two
// workers write one line.
//
// Quotas. One relaxed atomic per key, written by the control side when a
// controller arms and read by the packet path. That is what keeps the compare
// proportional to what was asked for: an unarmed key pays a load and a compare
// against zero, an armed key pays CrossingNotifier::Crossed (about 0.16 ns a
// packet, docs/benchmarking.md), and USAGE_REPORT_SWEEP pays neither.
//
// The three ways. The packet path posts the report when an armed key crosses
// (USAGE_REPORT_EVENTS), the tick sweeps the armed keys and reports what
// crossed (USAGE_REPORT_SWEEP), or both (USAGE_REPORT_BOTH, the default: the
// report exact, the sweep the backstop for a key whose traffic is spread across
// workers). The mode is configuration, fixed at Init: the module does not
// change what it decides from because a graph's wiring changed underneath it
// (D-155), and it cannot see the quotas, so an automatic choice would be a
// guess.
//
// What the module does not know: who a key is, what a quota means, and when to
// arm. The graph writes the key, the controller arms the quota, and the report
// leaves as the shared bess.counter_threshold event.
class UsageReport final : public Module {
 public:
  static const Commands cmds;
  static const ResourceTables resource_tables;
  // Capabilities: each worker meters into its own counter plane and compares it
  // against the quota; a key on one worker gets an exact report, not the sweep's.
  static constexpr ModuleCapabilities kCapabilities{
      ModuleCapabilities::State::kPerWorker, ModuleCapabilities::FlowAffinity::kPreferred};

  UsageReport() = default;

  CommandResponse Init(const sample::usage::pb::UsageReportArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  // Control side, from the "<name>/quotas" resource: arm `key` at `limit`
  // octets, for the packet path to compare against and the tick to sweep. The
  // callers run under the control-plane lock; the packet path reads only the
  // atomic, so both are safe with workers running.
  void Arm(uint64_t key, uint64_t limit);

  // The tick's work: sweep the armed keys, report the crossings. Runs under the
  // control-plane lock, so it stops at ctx.Expired() and resumes next tick.
  void Sweep(bess::framework::TickContext &ctx);

  // What the packet path reads: 0 when the key has no quota.
  uint64_t QuotaOf(size_t key) const noexcept {
    return quotas_[key].load(std::memory_order_relaxed);
  }

 private:
  // What one apply stages: the quota set, converted, and the set itself, for
  // the read that serves desired state.
  struct Staged {
    std::vector<std::pair<uint64_t, uint64_t>> quotas;  // key, octet limit
    sample::usage::pb::UsageQuotas set;
  };

  // The quota set a controller wrote, checked and converted. The whole set is
  // one value: setting it replaces every quota (D-129's singleton shape, the
  // one the sample plugin already uses).
  static std::expected<Staged, std::string> StageQuotas(
      const sample::usage::pb::UsageQuotas &v);

  // The key space: what the module was built for.
  size_t keys_ = 0;
  // The attribute a packet's key arrives in, resolved once at Init.
  int key_attr_ = -1;
  sample::usage::pb::UsageReportMode mode_ = sample::usage::pb::USAGE_REPORT_BOTH;

  // The counters: one plane per worker, two cells per key (octets, packets).
  // Null until Init.
  std::unique_ptr<bess::stats::KeyedCounters> counters_;

  // The packet path's half: one relaxed quota per key (0: no quota) and the
  // notifier that posts the report. Sized to the key space at Init.
  std::unique_ptr<std::atomic<uint64_t>[]> quotas_;
  bess::stats::CrossingNotifier notifier_;

  // The control side's half: the armed keys the tick sweeps and the endpoint
  // that calls it. The endpoint is open only when the mode sweeps.
  bess::framework::ThresholdTable watches_;
  bess::framework::TickEndpoint tick_;

  // "<name>/quotas" (D-129): the whole quota set. The apply writes the atomics
  // and the table, both safe with workers running, so it states no
  // worker_running condition.
  bess::framework::Singleton<sample::usage::pb::UsageQuotas, Staged>
      quotas_resource_;
  // The last applied set: what the read returns, and what the next apply
  // compares against to know which keys to disarm.
  sample::usage::pb::UsageQuotas arg_;
};

#endif  // BESS_MODULES_USAGEREPORT_H_
