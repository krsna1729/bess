// SPDX-License-Identifier: BSD-3-Clause

#include "control/maintenance_loop.h"

#include <algorithm>
#include <optional>
#include <thread>

#include "control/control_plane.h"
#include "control/pressure_monitor.h"
#include "control/resource_fill.h"
#include "framework/module_requests.h"
#include "framework/module_tick.h"
#include "stats/event_hub.h"
#include "utils/dpdk_memory.h"

namespace bess::control {

MaintenanceLoop::MaintenanceLoop(ControlPlane &control, framework::RequestHub &requests,
                                 framework::TickHub &ticks, std::chrono::microseconds interval,
                                 stats::EventHub *events, std::chrono::microseconds pressure_period,
                                 std::chrono::microseconds tick_slice, ResourceFillWatch *fills)
    : control_(control),
      requests_(requests),
      tick_hub_(ticks),
      tick_slice_(tick_slice),
      fills_(fills),
      events_(events),
      interval_(interval) {
  if (events_ != nullptr && pressure_period.count() > 0) {
    pressure_ = std::make_unique<PressureMonitor>(*events_);
    pressure_every_ = std::max<uint64_t>(
        1, static_cast<uint64_t>(pressure_period.count() / std::max<int64_t>(1, interval.count())));
  }
  if (interval_.count() > 0) {
    tick_hub_.set_interval(interval_);
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
    tick_hub_.set_interval(std::chrono::microseconds(0));
  }
}

void MaintenanceLoop::Run() {
  std::unique_lock<std::mutex> wait(stop_mutex_);
  while (!stop_cv_.wait_for(wait, interval_, [this] { return stop_; })) {
    wait.unlock();
    std::optional<PressureMonitor::Sample> sample;
    ResourceFillWatch::Result fills_result;
    {
      auto lock = control_.AcquireLock();
      delivered_.fetch_add(requests_.Deliver(), std::memory_order_relaxed);
      tick_hub_.Run(tick_slice_, ticks_.load(std::memory_order_relaxed));
      if (fills_ != nullptr) {
        // The sweep's own slice, measured after the hub: the hub gives each
        // endpoint its own slice, so a deadline taken before it could already
        // be spent, and the sweep would then stop after its first batch every
        // tick and never reach the rest of the resources.
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + tick_slice_;
        fills_result =
            fills_->Sweep([deadline] { return std::chrono::steady_clock::now() >= deadline; });
      }
      if (pressure_ != nullptr && ticks_.load(std::memory_order_relaxed) % pressure_every_ == 0) {
        sample = PressureMonitor::Take();
      }
    }
    // Outside the lock: the ticks' events, then the monitor's transitions.
    std::vector<framework::PendingEvent> queued = tick_hub_.TakeEvents();
    if (events_ != nullptr) {
      for (framework::PendingEvent &e : queued) {
        events_->Emit(std::move(e.type), std::move(e.source), std::move(e.fields));
      }
      for (const ResourceFillWatch::Crossing &c : fills_result.crossed) {
        std::map<std::string, std::string> fields{
            {"resource", c.resource},
            {"count", std::to_string(c.count)},
            {"limit", std::to_string(c.limit)}};
        if (c.capacity) {
          fields["capacity"] = std::to_string(*c.capacity);
        }
        events_->Emit(kResourceFillEvent, ModuleOf(c.resource), std::move(fields));
      }
    }
    if (sample) {
      pressure_->Observe(*sample);
    }
    if (events_ != nullptr) {
      // The hub has its own lock: workers' events reach the log every tick.
      (void)events_->DrainWorkers();
    }
    // After a large teardown, or garbage that never went through admission,
    // glibc's free pages go back (RT-1); RPCs are not held up while it walks
    // the arenas. The second check reads mallinfo2 at most once a second.
    if (!runtime::TrimAfterRelease()) {
      (void)free_check_.Check(std::chrono::steady_clock::now(),
                              runtime::MallocFreeBound(utils::memory_policy().limit_bytes));
    }
    ticks_.fetch_add(1, std::memory_order_relaxed);
    wait.lock();
  }
}

}  // namespace bess::control
