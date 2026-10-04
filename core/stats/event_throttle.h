// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_EVENT_THROTTLE_H_
#define BESS_STATS_EVENT_THROTTLE_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "stats/event_hub.h"

namespace bess::stats {

// A recurring condition on a packet path -- a full table, a full queue --
// reported as operational events, at most one per `interval_ns` per worker
// (M25 phase 3, D-089). Called on the failure path only: a packet path that
// never meets the condition runs none of it. The first occurrence posts at
// once; later ones inside the interval are counted and carried by the next
// event ("count"), so the log shows how much happened without one event per
// packet. Each worker uses its own slot (no sharing, no atomics); a post the
// worker's ring refuses keeps the count for the next one.
//
//   Init:    full_ = EventThrottle(init_context().events(), "bess.table_full", name());
//   Worker:  if (refused != 0) [[unlikely]] full_.Note(ctx->wid, ctx->current_ns, refused);
class EventThrottle {
 public:
  static constexpr uint64_t kDefaultIntervalNs = 1'000'000'000;

  EventThrottle() = default;
  // Registers `type` (one value, "count") and `source` with the hub.
  EventThrottle(EventHub &hub, const std::string &type, const std::string &source,
                uint64_t interval_ns = kDefaultIntervalNs)
      : hub_(&hub),
        type_(hub.RegisterType(type, {"count"})),
        source_(hub.NamedSource(source)),
        interval_ns_(interval_ns) {}

  // Worker `wid` met the condition `count` times at `now_ns`.
  void Note(int wid, uint64_t now_ns, uint64_t count = 1) noexcept {
    if (hub_ == nullptr || wid < 0 || wid >= EventHub::kMaxWorkers) {
      return;
    }
    Slot &s = slots_[static_cast<size_t>(wid)];
    s.pending += count;
    if (s.posted && now_ns - s.last_ns < interval_ns_) {
      return;
    }
    WorkerEvent e;
    e.type = type_;
    e.source = source_;
    e.values[0] = s.pending;
    if (hub_->Post(wid, e)) {
      s.pending = 0;
      s.last_ns = now_ns;
      s.posted = true;
    }
  }

 private:
  struct alignas(64) Slot {
    uint64_t last_ns = 0;
    uint64_t pending = 0;
    bool posted = false;
  };

  EventHub *hub_ = nullptr;
  uint32_t type_ = 0;
  uint32_t source_ = 0;
  uint64_t interval_ns_ = kDefaultIntervalNs;
  std::array<Slot, EventHub::kMaxWorkers> slots_{};
};

}  // namespace bess::stats

#endif  // BESS_STATS_EVENT_THROTTLE_H_
