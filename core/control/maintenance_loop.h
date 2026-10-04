// SPDX-License-Identifier: BSD-3-Clause
// The daemon's maintenance loop (TP4, D-077): a control-side thread that wakes
// every interval and, under the control-plane lock, delivers the requests
// workers posted to module endpoints (framework/module_requests.h). Handlers
// therefore run exactly as a module command does: serialized with every RPC,
// never on a worker. With an event log, it also moves workers' events into it
// every tick and, about once a second, samples the control-visible pressure
// conditions (control/pressure_monitor.h).

#ifndef BESS_CONTROL_MAINTENANCE_LOOP_H_
#define BESS_CONTROL_MAINTENANCE_LOOP_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace bess::framework {
class RequestHub;
}  // namespace bess::framework
namespace bess::stats {
class EventHub;
}  // namespace bess::stats

namespace bess::control {

class ControlPlane;
class PressureMonitor;

class MaintenanceLoop {
 public:
  // Starts the thread. `interval` 0 disables the loop (no thread).
  // With `events`, also moves workers' events into its log every tick and
  // emits pressure transitions (sampled every `pressure_period`).
  MaintenanceLoop(ControlPlane &control, framework::RequestHub &requests,
                  std::chrono::microseconds interval, stats::EventHub *events = nullptr,
                  std::chrono::microseconds pressure_period = std::chrono::seconds(1));
  ~MaintenanceLoop();  // stops and joins: no handler runs after it returns
  MaintenanceLoop(const MaintenanceLoop &) = delete;
  MaintenanceLoop &operator=(const MaintenanceLoop &) = delete;

  uint64_t ticks() const noexcept { return ticks_.load(std::memory_order_relaxed); }
  uint64_t delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }

 private:
  void Run();

  ControlPlane &control_;
  framework::RequestHub &requests_;
  stats::EventHub *const events_;
  const std::chrono::microseconds interval_;
  std::unique_ptr<PressureMonitor> pressure_;
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
