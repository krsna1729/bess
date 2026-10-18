// SPDX-License-Identifier: BSD-3-Clause
// Periodic control-side work for modules (D-150). Experimental API.
//
// A module sometimes has work that belongs on the control side and on a clock,
// not on a packet or on a worker's post: reading its own counters against
// limits a controller armed, aging entries out of a table it owns, checking
// how full a structure is. The daemon's maintenance loop already visits the
// control side every tick under the control-plane lock; a TickEndpoint is how
// a module asks to be called from it.
//
//   // Init(), once (control side):
//   tick_ = framework::TickEndpoint(
//       init_context().ticks(), std::chrono::seconds(1),
//       [this](framework::TickContext &ctx) { Sweep(ctx); });
//   // the callback, later, under the control lock, on the maintenance thread:
//   void Sweep(framework::TickContext &ctx) {
//     while (!ctx.Expired() && more_) { ...; }
//     ctx.Emit("my.event", name(), {{"key", "7"}});
//   }
//
// The framework knows nothing about what the callback does. It owns three
// things, so the module need not:
//
//  * When: the callback runs on the first maintenance tick and then no sooner
//    than `period` after the start of its previous run (zero: every tick).
//  * How long: each call gets a time slice (the daemon's choice, a few hundred
//    microseconds) and sees it as ctx.Expired(). The callback is trusted to
//    stop and resume next tick, since only it knows what a sensible stopping
//    point is; the hub measures each call and counts the ones that ran past
//    the slice (overruns()), because the control-plane lock is held for the
//    whole call and a module command waits for it.
//  * Events: ctx.Emit() queues an operational event that the maintenance loop
//    appends to the log after the lock is released, so a burst of events costs
//    the callback a queue push, not an allocation-heavy append under the lock.
//
// The callback runs with the control-plane lock held, like a module command and
// like a RequestEndpoint handler. It must not block, sleep or wait on a
// worker. An endpoint is opened in Init and destroyed with the module, both
// under that lock; destruction takes the endpoint out of the hub, so the hub
// never calls into a module that is gone. A callback may open endpoints and
// close other endpoints, but not its own: the call is still running inside it.

#ifndef BESS_FRAMEWORK_MODULE_TICK_H_
#define BESS_FRAMEWORK_MODULE_TICK_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bess::framework {

class TickHub;
class TickEndpoint;

// What one call of a tick callback may use. Valid only during the call.
class TickContext {
 public:
  using Clock = std::chrono::steady_clock;

  // True once this call's time slice is used up: stop at the next point that
  // is safe to resume from.
  bool Expired() const noexcept { return Clock::now() >= deadline_; }
  Clock::time_point deadline() const noexcept { return deadline_; }

  // Counts maintenance ticks since the daemon started (every tick, not only
  // those that called this endpoint).
  uint64_t tick() const noexcept { return tick_; }

  // Queues an event for the daemon's log, delivered after the control-plane
  // lock is released. `type` and `source` are the event's names (the same
  // strings the log shows); fields are its values, as text.
  void Emit(std::string type, std::string source, std::map<std::string, std::string> fields);

 private:
  friend class TickHub;
  TickContext(TickHub &hub, Clock::time_point deadline, uint64_t tick) noexcept
      : hub_(hub), deadline_(deadline), tick_(tick) {}

  TickHub &hub_;
  const Clock::time_point deadline_;
  const uint64_t tick_;
};

// An event a tick queued, for the maintenance loop to append to the log.
struct PendingEvent {
  std::string type;
  std::string source;
  std::map<std::string, std::string> fields;
};

namespace detail {

// What the hub sees of an endpoint: the callback and when it is next due.
struct TickSlot {
  std::function<void(TickContext &)> callback;
  std::chrono::steady_clock::duration period{};
  // Control side only; time_point{} is due at once.
  std::chrono::steady_clock::time_point next_due{};
};

}  // namespace detail

// The control side's set of open endpoints. One per daemon (the runtime owns
// it); the maintenance loop calls Run() with the control-plane lock held and
// then TakeEvents() after releasing it. Single-threaded: only the maintenance
// thread (or a test standing in for it) calls these.
class TickHub {
 public:
  using Clock = std::chrono::steady_clock;

  TickHub() = default;
  TickHub(const TickHub &) = delete;
  TickHub &operator=(const TickHub &) = delete;

  // Calls every endpoint that is due, each with its own `slice`; returns how
  // many it called. `tick` is the maintenance loop's tick count.
  size_t Run(Clock::duration slice, uint64_t tick);

  // The events the ticks queued since the last call. Call it after releasing
  // the control-plane lock.
  std::vector<PendingEvent> TakeEvents();

  size_t size() const noexcept { return slots_.size(); }
  // Calls that ran past their slice, since the hub was created.
  uint64_t overruns() const noexcept { return overruns_; }
  // Calls made, since the hub was created.
  uint64_t calls() const noexcept { return calls_; }

 private:
  friend class TickEndpoint;
  friend class TickContext;
  void Add(detail::TickSlot *slot);
  void Remove(detail::TickSlot *slot) noexcept;
  void Queue(PendingEvent event) { events_.push_back(std::move(event)); }

  std::vector<detail::TickSlot *> slots_;
  std::vector<PendingEvent> events_;
  uint64_t overruns_ = 0;
  uint64_t calls_ = 0;
};

inline void TickContext::Emit(std::string type, std::string source,
                              std::map<std::string, std::string> fields) {
  hub_.Queue({std::move(type), std::move(source), std::move(fields)});
}

class TickEndpoint final {
 public:
  using Callback = std::function<void(TickContext &)>;

  TickEndpoint() = default;
  // `period` is the least time between the starts of two calls; zero calls on
  // every maintenance tick.
  TickEndpoint(TickHub &hub, std::chrono::steady_clock::duration period, Callback callback)
      : hub_(&hub), slot_{std::move(callback), period, {}} {
    hub_->Add(&slot_);
  }
  ~TickEndpoint() {
    if (hub_ != nullptr) {
      hub_->Remove(&slot_);
    }
  }
  TickEndpoint(const TickEndpoint &) = delete;
  TickEndpoint &operator=(const TickEndpoint &) = delete;
  // Assigning an opened endpoint to a default-constructed member (in Init).
  TickEndpoint &operator=(TickEndpoint &&other) noexcept {
    if (this != &other) {
      if (hub_ != nullptr) {
        hub_->Remove(&slot_);
      }
      hub_ = std::exchange(other.hub_, nullptr);
      slot_ = std::move(other.slot_);
      if (hub_ != nullptr) {
        hub_->Remove(&other.slot_);
        hub_->Add(&slot_);
      }
    }
    return *this;
  }
  TickEndpoint(TickEndpoint &&other) noexcept { *this = std::move(other); }

  bool open() const noexcept { return hub_ != nullptr; }

 private:
  TickHub *hub_ = nullptr;
  detail::TickSlot slot_;
};

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_MODULE_TICK_H_
