// SPDX-License-Identifier: BSD-3-Clause
// Bounded-backpressure conditions the control side can see, as operational
// events (M25 phase 3, D-089). The maintenance loop takes a sample about once
// a second (under the control-plane lock: the port list) and the monitor
// emits only transitions:
//
//   bess.rcu_backlog  source "bessd": state "high" when objects waiting for a
//                     grace period reach half the retire high-water mark (past
//                     it, retiring waits), "cleared" below an eighth of it;
//                     with pending and high_water.
//   bess.port_drops   source the port: per direction ("rx", "tx"), state
//                     "dropping" when a sample sees drops after one that saw
//                     none (dropped: in that sample), "cleared" at the first
//                     sample without (dropped: the whole episode).
//   bess.port_link    source the port: state "up" or "down" when the link
//                     changes (with speed_mbps); the first sample of a port
//                     is its baseline, not an event.
//
// Packet-path conditions (a full table, a full queue) are posted by the
// modules that meet them (stats/event_throttle.h).

#ifndef BESS_CONTROL_PRESSURE_MONITOR_H_
#define BESS_CONTROL_PRESSURE_MONITOR_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bess::stats {
class EventHub;
}  // namespace bess::stats

namespace bess::control {

class PressureMonitor {
 public:
  struct PortSample {
    std::string name;
    uint64_t rx_dropped = 0;
    uint64_t tx_dropped = 0;
    bool link_up = true;
    uint32_t speed_mbps = 0;
  };
  struct Sample {
    uint64_t rcu_pending = 0;
    uint64_t rcu_high_water = 0;
    std::vector<PortSample> ports;
  };

  explicit PressureMonitor(stats::EventHub &events) : events_(events) {}

  // The runtime's state now. The caller holds the control-plane lock (the
  // port list); port counters are read as GetPortStats reads them.
  static Sample Take();

  // Emits the transitions from the previous sample to `s`.
  void Observe(const Sample &s);

 private:
  struct Direction {
    uint64_t last = 0;
    uint64_t episode = 0;
    bool dropping = false;
  };
  struct PortState {
    Direction rx, tx;
    bool link_up = true;
    bool seen = false;
  };

  void ObserveDrops(const std::string &port, const char *direction, Direction &d, uint64_t now);

  stats::EventHub &events_;
  bool rcu_high_ = false;
  std::map<std::string, PortState> ports_;
};

}  // namespace bess::control

#endif  // BESS_CONTROL_PRESSURE_MONITOR_H_
