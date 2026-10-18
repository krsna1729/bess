// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_CROSSING_NOTIFIER_H_
#define BESS_STATS_CROSSING_NOTIFIER_H_

#include <cstdint>
#include <string>

#include "stats/event_hub.h"

namespace bess::stats {

// The event a counter crossing is reported as, by every producer: a module's
// tick through framework::ReportCrossings (framework/threshold_table.h, D-151)
// and a worker through CrossingNotifier below. One type, so a consumer's
// handler serves both. Its source is the reporting module's name; its fields
// are "key", "value" (the total read) and "limit"; the worker path adds
// "worker", the plane that crossed.
inline constexpr const char *kCounterThresholdEvent = "bess.counter_threshold";

// A counter on a packet path that passed a limit, reported as an operational
// event (D-152). An opt-in for a module that knows its workload: a key's
// traffic always runs on ONE worker (sessions pinned by RSS on the key, say),
// so that worker's own count IS the key's total and the event is exact.
//
// The condition matters. A worker sees only its own plane. If a key's packets
// can arrive on several workers, no worker sees the total, and an event says
// only that one plane reached the limit: a hint (see WorkerLimit). For a key
// spread across workers, sweep the total from the module's tick with a
// framework::ThresholdTable, which reads every plane, instead.
//
// A lost event (a full ring: counted, and shown in the log as
// "bess.events_lost") is the other limit: the event is not acknowledged, so a
// consumer that cannot afford a missed report keeps polling as the backstop.
//
// Cost: the packet path pays one compare, `Crossed(before, after, limit)`,
// after the increment it already makes; Note() runs only on a crossing and
// never blocks or allocates. A plugin using it requires BESS_CAP_EVENT_SOURCES.
//
//   Init:    crossed_ = CrossingNotifier(init_context().events(), name());
//   Worker:  const uint64_t before = slot.bytes;
//            slot.bytes += len;
//            if (CrossingNotifier::Crossed(before, slot.bytes, slot.limit)) [[unlikely]] {
//              crossed_.Note(ctx->wid, key, slot.bytes, slot.limit);
//            }
//
// A limit is armed on the control side, in the module's own action, by storing
// it where the worker reads it. Arm a running counter at its current count plus
// the room the controller wants (a limit at or below the count never fires),
// and read the counter once after arming, since a crossing between reading it
// and arming is not reported.
class CrossingNotifier {
 public:
  CrossingNotifier() = default;
  // Reports as kCounterThresholdEvent from `source`.
  CrossingNotifier(EventHub &hub, const std::string &source)
      : CrossingNotifier(hub, kCounterThresholdEvent, source) {}
  // Registers `type` (values "key", "value", "limit") and `source` with the
  // hub, for a module that reports under a name of its own.
  CrossingNotifier(EventHub &hub, const std::string &type, const std::string &source)
      : hub_(&hub),
        type_(hub.RegisterType(type, {"key", "value", "limit"})),
        source_(hub.NamedSource(source)) {}

  // Whether a count that went from `before` to `after` passed `limit` on this
  // step: true exactly once as a growing count passes it, whatever the step
  // size, and false for a count already past it -- so a worker posts one
  // event per arming of a limit, not one per packet. A limit armed on a count
  // already past it never fires.
  static constexpr bool Crossed(uint64_t before, uint64_t after, uint64_t limit) noexcept {
    return before < limit && limit <= after;
  }

  // For a key whose traffic is spread across `workers` planes: the per-worker
  // limit that cannot miss a crossing of `total`. If the planes sum to
  // `total` or more, one of them is at least ceil(total / workers), so that
  // worker posts. The event can fire while the sum is still below `total`
  // (one plane ahead of the others), so on such an event the controller reads
  // the total before it acts; a hint, never the answer. `workers` 0 is read
  // as 1.
  static constexpr uint64_t WorkerLimit(uint64_t total, uint32_t workers) noexcept {
    const uint64_t n = workers == 0 ? 1 : workers;
    return total / n + (total % n != 0 ? 1 : 0);
  }

  // Worker `wid`'s count for `key` reached `limit` and read `value`. False
  // when the worker's ring refused the event (counted by the hub).
  bool Note(dataplane::WorkerId wid, uint64_t key, uint64_t value, uint64_t limit) noexcept {
    if (hub_ == nullptr) {
      return false;
    }
    WorkerEvent e;
    e.type = type_;
    e.source = source_;
    e.values[0] = key;
    e.values[1] = value;
    e.values[2] = limit;
    return hub_->Post(wid, e);
  }

 private:
  EventHub *hub_ = nullptr;
  uint32_t type_ = 0;
  uint32_t source_ = 0;
};

}  // namespace bess::stats

#endif  // BESS_STATS_CROSSING_NOTIFIER_H_
