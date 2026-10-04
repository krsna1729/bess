// SPDX-License-Identifier: BSD-3-Clause

#include "control/maintenance_loop.h"

#include "control/control_plane.h"
#include "framework/module_requests.h"
#include "stats/event_hub.h"

namespace bess::control {

MaintenanceLoop::MaintenanceLoop(ControlPlane &control, framework::RequestHub &requests,
                                 std::chrono::microseconds interval, stats::EventHub *events)
    : control_(control), requests_(requests), events_(events), interval_(interval) {
  if (interval_.count() > 0) {
    thread_ = std::thread([this] { Run(); });
  }
}

MaintenanceLoop::~MaintenanceLoop() {
  {
    std::lock_guard<std::mutex> guard(stop_mutex_);
    stop_ = true;
  }
  stop_cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void MaintenanceLoop::Run() {
  std::unique_lock<std::mutex> wait(stop_mutex_);
  while (!stop_cv_.wait_for(wait, interval_, [this] { return stop_; })) {
    wait.unlock();
    {
      auto lock = control_.AcquireLock();
      delivered_.fetch_add(requests_.Deliver(), std::memory_order_relaxed);
    }
    if (events_ != nullptr) {
      // The hub has its own lock: workers' events reach the log every tick.
      (void)events_->DrainWorkers();
    }
    ticks_.fetch_add(1, std::memory_order_relaxed);
    wait.lock();
  }
}

}  // namespace bess::control
