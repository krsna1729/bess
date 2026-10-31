// SPDX-License-Identifier: BSD-3-Clause
// The daemon's maintenance loop (TP4, D-077): a control-side thread that wakes
// every interval and, under the control-plane lock, delivers the requests
// workers posted to module endpoints (framework/module_requests.h) and calls
// the modules' tick endpoints that are due (framework/module_tick.h, D-150).
// Handlers and ticks therefore run exactly as a module command does:
// serialized with every RPC, never on a worker. The events the ticks queued
// are appended to the log after the lock is released. With an event log, it
// also moves workers' events into it every tick and, about once a second,
// samples the control-visible pressure conditions (control/pressure_monitor.h).
// After a large teardown, or once glibc holds a bound's worth of new free
// bytes (checked once a second), it hands glibc's free pages back
// (runtime/malloc_policy.h).

#ifndef BESS_CONTROL_MAINTENANCE_LOOP_H_
#define BESS_CONTROL_MAINTENANCE_LOOP_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "runtime/malloc_policy.h"

namespace bess::framework {
class RequestHub;
class TickHub;
}  // namespace bess::framework
namespace bess::stats {
class EventHub;
}  // namespace bess::stats

namespace bess::control {

class ControlPlane;
class PressureMonitor;
class ResourceFillWatch;

class MaintenanceLoop {
 public:
  // Starts the thread. `interval` 0 disables the loop (no thread).
  // With `events`, also moves workers' events into its log every tick and
  // emits pressure transitions (sampled every `pressure_period`), and appends
  // the events the ticks queued. Each due tick endpoint is given `tick_slice`
  // (the hub gives each endpoint its own), and the fill watch
  // (control/resource_fill.h, D-165) is then given a slice of its own, so one
  // tick may spend more than one slice on control-side work. Its crossings
  // reach the log beside the ticks' events when `events` is set.
  MaintenanceLoop(ControlPlane &control, framework::RequestHub &requests,
                  framework::TickHub &ticks, std::chrono::microseconds interval,
                  stats::EventHub *events = nullptr,
                  std::chrono::microseconds pressure_period = std::chrono::seconds(1),
                  std::chrono::microseconds tick_slice = std::chrono::microseconds(250),
                  ResourceFillWatch *fills = nullptr);
  ~MaintenanceLoop();  // stops and joins: no handler runs after it returns
  MaintenanceLoop(const MaintenanceLoop &) = delete;
  MaintenanceLoop &operator=(const MaintenanceLoop &) = delete;

  uint64_t ticks() const noexcept { return ticks_.load(std::memory_order_relaxed); }
  uint64_t delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }

 private:
  void Run();

  ControlPlane &control_;
  framework::RequestHub &requests_;
  framework::TickHub &tick_hub_;
  const std::chrono::microseconds tick_slice_;
  ResourceFillWatch *const fills_;
  stats::EventHub *const events_;
  const std::chrono::microseconds interval_;
  std::unique_ptr<PressureMonitor> pressure_;
  runtime::MallocFreeCheck free_check_{runtime::kMallocFreeCheckPeriod};
  uint64_t pressure_every_ = 1;  // ticks
  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;
  bool stop_ = false;
  std::atomic<uint64_t> ticks_{0};
  std::atomic<uint64_t> delivered_{0};
  std::thread thread_;
};

}  // namespace bess::control

#endif  // BESS_CONTROL_MAINTENANCE_LOOP_H_
