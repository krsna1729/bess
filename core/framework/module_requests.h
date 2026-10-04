// SPDX-License-Identifier: BSD-3-Clause
// Worker-to-control requests for modules (table policy TP4, D-077).
// Experimental API.
//
// A worker notices something it must not handle on the packet path -- a table
// nearly full, a final usage record to hand off -- and asks the control side to
// act. The packet path only posts; the control side does the work (allocate,
// publish, copy) under the control-plane lock, in the daemon's maintenance
// loop, and calls the module's handler.
//
//   // Init(), once (control side):
//   grow_ = framework::RequestEndpoint<GrowRequest>(
//       init_context().requests(), [this](const GrowRequest &r) { Grow(r); });
//   // ProcessBatch (any worker):
//   if (load_high) grow_.Post({capacity, size});
//   // the handler, later, under the control lock:
//   void Grow(const GrowRequest &r) { ...; grow_.Done(); }
//
// One request at a time per endpoint. Post() sets the endpoint's pending flag
// and stores the request; while it is pending, further posts return false at
// the cost of one relaxed load (a hot path posts at most once per condition).
// The handler runs once per accepted post; the endpoint stays pending until
// Done() -- called by the handler, or later when the work it started finishes
// (an incremental migration) -- so a condition is not re-posted while it is
// being dealt with. A module that needs independent kinds of request opens one
// endpoint per kind.
//
// Posting never blocks and never allocates; any worker may post. The handler
// runs on the maintenance thread with the control-plane lock held, like a
// module command. An endpoint is opened and destroyed under that lock (Init,
// module destruction); destroying it discards a request not yet delivered.

#ifndef BESS_FRAMEWORK_MODULE_REQUESTS_H_
#define BESS_FRAMEWORK_MODULE_REQUESTS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace bess::framework {

class RequestHub;

namespace detail {

// What the hub sees of an endpoint: a slot it can poll and deliver.
class RequestSlot {
 public:
  RequestSlot(const RequestSlot &) = delete;
  RequestSlot &operator=(const RequestSlot &) = delete;

  // Delivers the request if one is ready; true if it did.
  virtual bool DeliverIfReady() = 0;

 protected:
  RequestSlot() = default;
  ~RequestSlot() = default;
};

}  // namespace detail

// The control side's set of open endpoints. One per daemon (the runtime owns
// it); the maintenance loop calls Deliver() with the control-plane lock held.
class RequestHub {
 public:
  RequestHub() = default;
  RequestHub(const RequestHub &) = delete;
  RequestHub &operator=(const RequestHub &) = delete;

  // Calls the handler of every endpoint with a request ready; returns how many.
  size_t Deliver();
  size_t size() const noexcept { return slots_.size(); }

 private:
  template <typename Request>
  friend class RequestEndpoint;
  void Add(detail::RequestSlot *slot);
  void Remove(detail::RequestSlot *slot) noexcept;

  std::vector<detail::RequestSlot *> slots_;
};

template <typename Request>
class RequestEndpoint final : private detail::RequestSlot {
  static_assert(std::is_trivially_copyable_v<Request>,
                "a request is copied from the worker to the control side");
  static_assert(sizeof(Request) <= 56, "a request fits one cache line with its flags");

 public:
  using Handler = std::function<void(const Request &)>;

  RequestEndpoint() = default;
  RequestEndpoint(RequestHub &hub, Handler handler)
      : hub_(&hub), handler_(std::move(handler)) {
    hub_->Add(this);
  }
  ~RequestEndpoint() {
    if (hub_ != nullptr) {
      hub_->Remove(this);
    }
  }
  RequestEndpoint(const RequestEndpoint &) = delete;
  RequestEndpoint &operator=(const RequestEndpoint &) = delete;
  // Assigning an opened endpoint to a default-constructed member (in Init).
  RequestEndpoint &operator=(RequestEndpoint &&other) noexcept {
    if (this != &other) {
      if (hub_ != nullptr) {
        hub_->Remove(this);
      }
      hub_ = std::exchange(other.hub_, nullptr);
      handler_ = std::move(other.handler_);
      // Moved in Init, before any worker posts; the state moves with it.
      pending_.store(other.pending_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      ready_.store(other.ready_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      request_ = other.request_;
      if (hub_ != nullptr) {
        hub_->Remove(&other);
        hub_->Add(this);
      }
    }
    return *this;
  }
  RequestEndpoint(RequestEndpoint &&other) noexcept { *this = std::move(other); }

  // Any worker. False if a request is already pending (nothing is stored).
  bool Post(const Request &request) noexcept {
    if (pending_.load(std::memory_order_relaxed) ||
        pending_.exchange(true, std::memory_order_acquire)) {
      return false;
    }
    request_ = request;
    ready_.store(true, std::memory_order_release);
    return true;
  }

  // Control side: the condition has been dealt with; the next Post() goes
  // through.
  void Done() noexcept { pending_.store(false, std::memory_order_release); }
  bool pending() const noexcept { return pending_.load(std::memory_order_acquire); }
  bool open() const noexcept { return hub_ != nullptr; }

 private:
  bool DeliverIfReady() override {
    if (!ready_.load(std::memory_order_acquire)) {
      return false;
    }
    const Request request = request_;
    ready_.store(false, std::memory_order_relaxed);
    handler_(request);
    return true;
  }

  RequestHub *hub_ = nullptr;
  Handler handler_;
  // Written by workers; read by the maintenance thread.
  alignas(64) std::atomic<bool> pending_{false};
  std::atomic<bool> ready_{false};
  Request request_{};
};

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_MODULE_REQUESTS_H_
