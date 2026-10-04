// SPDX-License-Identifier: BSD-3-Clause

#include "control/maintenance_loop.h"

#include <algorithm>
#include <optional>

#include "control/control_plane.h"
#include "control/pressure_monitor.h"
#include "framework/module_requests.h"
#include "stats/event_hub.h"

namespace bess::control {

MaintenanceLoop::MaintenanceLoop(ControlPlane &control, framework::RequestHub &requests,
                                 std::chrono::microseconds interval, stats::EventHub *events,
                                 std::chrono::microseconds pressure_period)
    : control_(control), requests_(requests), events_(events), interval_(interval) {
  if (events_ != nullptr && pressure_period.count() > 0) {
    pressure_ = std::make_unique<PressureMonitor>(*events_);
    pressure_every_ = std::max<uint64_t>(
        1, static_cast<uint64_t>(pressure_period.count() / std::max<int64_t>(1, interval.count())));
  }
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
    std::optional<PressureMonitor::Sample> sample;
    {
      auto lock = control_.AcquireLock();
      delivered_.fetch_add(requests_.Deliver(), std::memory_order_relaxed);
      if (pressure_ != nullptr && ticks_.load(std::memory_order_relaxed) % pressure_every_ == 0) {
        sample = PressureMonitor::Take();
      }
    }
    if (sample) {
      pressure_->Observe(*sample);
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
