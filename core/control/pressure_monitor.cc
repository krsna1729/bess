// SPDX-License-Identifier: BSD-3-Clause

#include "control/pressure_monitor.h"

#include <set>

#include "port.h"
#include "rcu/rcu_domain.h"
#include "runtime/runtime_state.h"
#include "stats/event_hub.h"

namespace bess::control {

PressureMonitor::Sample PressureMonitor::Take() {
  Sample s;
  rcu::RcuDomain &rcu = runtime::runtime().rcu();
  s.rcu_pending = rcu.pending_retired();
  s.rcu_high_water = rcu.retire_high_water();
  for (const auto &[name, port] : runtime::runtime().ports().All()) {
    const Port::PortStats stats = port->GetPortStats();
    const Port::LinkStatus link = port->GetLinkStatus();
    s.ports.push_back({name, stats.inc.dropped, stats.out.dropped, link.link_up, link.speed});
  }
  return s;
}

void PressureMonitor::Observe(const Sample &s) {
  const uint64_t high = s.rcu_high_water / 2;
  const uint64_t low = s.rcu_high_water / 8;
  const bool was_high = rcu_high_;
  if (!rcu_high_ && high != 0 && s.rcu_pending >= high) {
    rcu_high_ = true;
  } else if (rcu_high_ && s.rcu_pending < low) {
    rcu_high_ = false;
  }
  if (rcu_high_ != was_high) {
    events_.Emit("bess.rcu_backlog", "bessd",
                 {{"state", rcu_high_ ? "high" : "cleared"},
                  {"pending", std::to_string(s.rcu_pending)},
                  {"high_water", std::to_string(s.rcu_high_water)}});
  }
  std::set<std::string> present;
  for (const PortSample &p : s.ports) {
    present.insert(p.name);
    PortState &st = ports_[p.name];
    if (!st.seen) {  // the baseline
      st = PortState{{p.rx_dropped, 0, false}, {p.tx_dropped, 0, false}, p.link_up, true};
      continue;
    }
    ObserveDrops(p.name, "rx", st.rx, p.rx_dropped);
    ObserveDrops(p.name, "tx", st.tx, p.tx_dropped);
    if (p.link_up != st.link_up) {
      st.link_up = p.link_up;
      events_.Emit("bess.port_link", p.name,
                   {{"state", p.link_up ? "up" : "down"},
                    {"speed_mbps", std::to_string(p.speed_mbps)}});
    }
  }
  std::erase_if(ports_, [&](const auto &kv) { return present.count(kv.first) == 0; });
}

void PressureMonitor::ObserveDrops(const std::string &port, const char *direction, Direction &d,
                                   uint64_t now) {
  // A counter that went down was reset (or the port recreated): a new baseline.
  const uint64_t delta = now >= d.last ? now - d.last : 0;
  d.last = now;
  if (delta != 0) {
    d.episode += delta;
    if (!d.dropping) {
      d.dropping = true;
      events_.Emit("bess.port_drops", port,
                   {{"direction", direction}, {"state", "dropping"},
                    {"dropped", std::to_string(delta)}});
    }
  } else if (d.dropping) {
    d.dropping = false;
    events_.Emit("bess.port_drops", port,
                 {{"direction", direction}, {"state", "cleared"},
                  {"dropped", std::to_string(d.episode)}});
    d.episode = 0;
  }
}

}  // namespace bess::control
