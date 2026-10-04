// SPDX-License-Identifier: BSD-3-Clause
// The daemon's maintenance loop (TP4, D-077): a control-side thread that wakes
// every interval and, under the control-plane lock, delivers the requests
// workers posted to module endpoints (framework/module_requests.h). Handlers
// therefore run exactly as a module command does: serialized with every RPC,
// never on a worker.

#ifndef BESS_CONTROL_MAINTENANCE_LOOP_H_
#define BESS_CONTROL_MAINTENANCE_LOOP_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace bess::framework {
class RequestHub;
}  // namespace bess::framework

namespace bess::control {

class ControlPlane;

class MaintenanceLoop {
 public:
  // Starts the thread. `interval` 0 disables the loop (no thread).
  MaintenanceLoop(ControlPlane &control, framework::RequestHub &requests,
                  std::chrono::microseconds interval);
  ~MaintenanceLoop();  // stops and joins: no handler runs after it returns
  MaintenanceLoop(const MaintenanceLoop &) = delete;
  MaintenanceLoop &operator=(const MaintenanceLoop &) = delete;

  uint64_t ticks() const noexcept { return ticks_.load(std::memory_order_relaxed); }
  uint64_t delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }

 private:
  void Run();

  ControlPlane &control_;
  framework::RequestHub &requests_;
  const std::chrono::microseconds interval_;
  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;
  bool stop_ = false;
  std::atomic<uint64_t> ticks_{0};
  std::atomic<uint64_t> delivered_{0};
  std::thread thread_;
};

}  // namespace bess::control

#endif  // BESS_CONTROL_MAINTENANCE_LOOP_H_
