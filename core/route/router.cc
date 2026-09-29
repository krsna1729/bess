// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "route/router.h"

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

#include <glog/logging.h>

#include "dataplane/slot_resource.h"
#include "dataplane/transaction_engine.h"

namespace bess::route {

std::expected<std::unique_ptr<Router>, RouteError> Router::Create(
    std::string name, const Config &config, size_t max_next_hops,
    rcu::RcuDomain &domain) {
  if (max_next_hops == 0 || max_next_hops > LpmRouteTable::kMaxValue) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  auto routes = RouteTable<NextHopId>::Create(name, config, domain);
  if (!routes) {
    return std::unexpected(routes.error());
  }
  return std::unique_ptr<Router>(
      new Router(name, std::move(*routes), max_next_hops, domain));
}

Router::Router(std::string name, std::unique_ptr<RouteTable<NextHopId>> routes,
               size_t max_next_hops, rcu::RcuDomain &domain)
    : routes_(std::move(routes)),
      domain_(domain),
      next_hops_(max_next_hops),
      references_(max_next_hops + 1, 0),
      next_hops_name_(name + "/next_hops"),
      routes_name_(name + "/routes") {}

// The SlotTable frees what is still published (including retiring next hops)
// when it goes: the owner destroys a Router only once no reader can use it.
Router::~Router() {
  if (engine_ != nullptr) {
    // Routes reference only next hops, so the two leave together with their
    // contents; with readers gone, pending removals complete in the call.
    const std::string names[] = {routes_name_, next_hops_name_};
    auto unregistered = engine_->Unregister(names);
    CHECK(unregistered) << unregistered.error();
  }
}

// "<router>/routes" (D-023): key RouteKey(prefix), value NextHopId; each
// route references its next hop in "<router>/next_hops". A root: erasing a
// route takes effect at once (rte_lpm deletes in place), so nothing may
// reference a route.
//
// rte_lpm cannot promise capacity (rules and tbl8 groups) for a set of
// prefixes in advance, so a new route is placed during Reserve() with a
// placeholder value: what its addresses resolve to today (the covering
// route, else the default, else id 0, a miss -- LpmRouteTable::
// CoveringValue). Readers see no change until Publish() stores the real
// next hop -- an in-place update of an existing rule, which cannot fail.
// A prefix that does not fit rejects the transaction with nothing visible;
// Abort() deletes the placeholder (deletes cannot fail: the tbl8 defer
// queue holds every group, pinned by RouteTest). Placed prefixes are
// "pending" to Contains()/ReferencesOf() until the transaction ends.
class Router::RouteResource final : public dataplane::Resource {
 public:
  explicit RouteResource(Router &router)
      : Resource(router.routes_name_, {router.next_hops_name_}),
        router_(router) {}

  size_t LiveCount() const override { return router_.routes_->size(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    const auto prefix = Decode(key);
    return prefix && Find(*prefix).has_value();
  }

  std::vector<dataplane::Reference> ReferencesOf(
      const dataplane::ResourceKey &key) const override {
    const auto prefix = Decode(key);
    const auto hop = prefix ? Find(*prefix) : std::nullopt;
    return hop ? References(*hop) : std::vector<dataplane::Reference>{};
  }
  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &visit) const
      override {
    router_.routes_->ForEach([&](Ipv4Prefix, NextHopId hop) {
      for (const auto &ref : References(hop)) {
        visit(ref);
      }
    });
  }


  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    const auto prefix = Decode(op.key);
    if (!prefix) {
      return std::unexpected("not a route key (RouteKey) of a valid prefix");
    }
    const std::optional<NextHopId> previous = Find(*prefix);
    if (op.kind == dataplane::OpKind::kErase) {
      if (!previous) {
        return std::unexpected("not found");
      }
      Reservation erase{std::make_unique<EraseOp>(router_, *prefix), {}};
      erase.existed = true;
      erase.previous_references = References(*previous);
      return erase;
    }
    const NextHopId *hop = std::any_cast<NextHopId>(&op.value);
    if (hop == nullptr) {
      return std::unexpected("wrong value type (want NextHopId)");
    }
    if (!router_.ValidId(*hop)) {
      return std::unexpected(RouteErrorName(RouteError::kInvalidId));
    }
    // Everything that can throw first; placing the prefix is the last step.
    auto upsert = std::make_unique<UpsertOp>(router_, *prefix, *hop);
    Reservation reservation{nullptr, References(*hop)};
    reservation.existed = previous.has_value();
    if (previous) {
      reservation.previous_references = References(*previous);
    } else if (prefix->length() != 0) {
      pending_.insert(*prefix);  // may throw: before anything is placed
      const uint32_t placeholder =
          router_.routes_->untyped().CoveringValue(*prefix).value_or(0);
      if (auto placed = router_.routes_->untyped().Upsert(*prefix, placeholder);
          !placed) {
        pending_.erase(*prefix);
        return std::unexpected(RouteErrorName(placed.error()));
      }
      upsert->MarkPlaced();  // Abort() now deletes it
    }
    reservation.staged = std::move(upsert);
    return reservation;
  }

  void EndTransaction() noexcept override { pending_.clear(); }

 private:
  static std::optional<Ipv4Prefix> Decode(const dataplane::ResourceKey &key) {
    uint64_t raw = 0;
    if (!dataplane::DecodeKey(key, &raw) || raw >> 40 != 0) {
      return std::nullopt;
    }
    const auto prefix = Ipv4Prefix::Make(static_cast<uint32_t>(raw >> 8),
                                         static_cast<uint8_t>(raw & 0xff));
    return prefix ? std::optional<Ipv4Prefix>(*prefix) : std::nullopt;
  }

  std::optional<NextHopId> Find(Ipv4Prefix prefix) const {
    if (pending_.contains(prefix)) {
      return std::nullopt;
    }
    return router_.routes_->Find(prefix);
  }

  std::vector<dataplane::Reference> References(NextHopId hop) const {
    return {{router_.next_hops_name_, dataplane::EncodeKey(hop)}};
  }

  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(Router &router, Ipv4Prefix prefix, NextHopId hop)
        : router_(router), prefix_(prefix), hop_(hop) {}
    void MarkPlaced() noexcept { placed_ = true; }
    void Publish(dataplane::Retirer &) noexcept override {
      // The prefix is present (placed in Reserve(), or already a route): an
      // in-place value store, which cannot fail.
      auto set = router_.routes_->Upsert(prefix_, hop_);
      CHECK(set) << RouteErrorName(set.error());
    }
    void Abort() noexcept override {
      if (placed_) {
        auto erased = router_.routes_->Erase(prefix_);
        CHECK(erased) << RouteErrorName(erased.error());
      }
    }

   private:
    Router &router_;
    Ipv4Prefix prefix_;
    NextHopId hop_;
    bool placed_ = false;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(Router &router, Ipv4Prefix prefix)
        : router_(router), prefix_(prefix) {}
    void Publish(dataplane::Retirer &) noexcept override {
      auto erased = router_.routes_->Erase(prefix_);
      CHECK(erased) << RouteErrorName(erased.error());
    }

   private:
    Router &router_;
    Ipv4Prefix prefix_;
  };

  Router &router_;
  std::set<Ipv4Prefix> pending_;  // placed in this transaction
};

std::expected<void, std::string> Router::Enroll(
    dataplane::TransactionEngine &engine) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected("already enrolled");
  }
  if (CompleteRetirementsLocked() != 0) {
    return std::unexpected("removed next hops are still retiring");
  }
  if (routes_->size() != 0) {
    return std::unexpected("enroll before adding routes");
  }
  auto hops = std::make_unique<dataplane::SlotResource<NextHopId, NextHop>>(
      next_hops_name_, next_hops_);
  auto routes = std::make_unique<RouteResource>(*this);
  if (auto r = engine.Register(hops.get()); !r) {
    return r;
  }
  if (auto r = engine.Register(routes.get()); !r) {
    CHECK(engine.Unregister(next_hops_name_));
    return r;
  }
  engine_ = &engine;
  next_hops_res_ = std::move(hops);
  routes_res_ = std::move(routes);
  return {};
}

std::expected<void, std::string> Router::Release() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ == nullptr) {
    return {};  // not enrolled, or already released
  }
  const std::string names[] = {routes_name_, next_hops_name_};
  auto released = engine_->ReleaseForTeardown(names);
  if (!released) {
    return released;
  }
  engine_ = nullptr;
  next_hops_res_.reset();
  routes_res_.reset();
  return {};
}

size_t Router::CompleteRetirementsLocked() {
  std::vector<std::unique_ptr<const NextHop>> emptied;
  std::erase_if(retiring_, [&](const Retiring &r) {
    if (!domain_.IsComplete(r.token)) {
      return false;
    }
    // No reader can still obtain the id from a route; one may have just
    // looked the object up, so it is freed after one more grace period.
    emptied.push_back(next_hops_.Unpublish(r.id));
    return true;
  });
  if (!emptied.empty()) {
    const rcu::GracePeriod token = domain_.StartGracePeriod();
    for (auto &hop : emptied) {
      domain_.Retire(token, std::move(hop));
    }
    domain_.ReclaimReady();
  }
  return retiring_.size();
}

size_t Router::ReclaimRetired() {
  std::lock_guard<std::mutex> lock(mutex_);
  return CompleteRetirementsLocked();
}

std::expected<void, RouteError> Router::SetNextHop(NextHopId id,
                                                   const NextHop &hop) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  if (!ValidId(id)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!next_hops_.CanPublish(id)) {
    return std::unexpected(RouteError::kNextHopRetiring);
  }
  // One pointer store; a reader holding the previous object keeps it until
  // its next quiescent state.
  auto replaced = next_hops_.Publish(id, std::make_unique<const NextHop>(hop));
  if (replaced) {
    domain_.Retire(domain_.StartGracePeriod(), std::move(replaced));
    domain_.ReclaimReady();
  }
  return {};
}

std::expected<void, RouteError> Router::RemoveNextHop(NextHopId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  if (!ValidId(id)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!next_hops_.Contains(id)) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  if (references_[id.value()] != 0) {
    return std::unexpected(RouteError::kNextHopInUse);
  }
  // No route names `id` any more, but a reader may have looked one up just
  // before the last such route went and not yet loaded the next hop. Keep it
  // published until every reader has passed a quiescent state; a later
  // control call empties the slot. No waiting here (a stalled worker must not
  // stall the command path, and a transaction must not hold a blocking wait).
  next_hops_.Retire(id);
  retiring_.push_back({id, domain_.StartGracePeriod()});
  return {};
}

std::expected<void, RouteError> Router::SetRoute(Ipv4Prefix prefix,
                                                 NextHopId hop) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  if (!ValidId(hop)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!next_hops_.Contains(hop)) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  const std::optional<NextHopId> previous = routes_->Find(prefix);
  if (auto set = routes_->Upsert(prefix, hop); !set) {
    return set;
  }
  references_[hop.value()]++;
  if (previous) {
    references_[previous->value()]--;
  }
  return {};
}

std::expected<void, RouteError> Router::RemoveRoute(Ipv4Prefix prefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return std::unexpected(RouteError::kEnrolled);
  }
  CompleteRetirementsLocked();
  const std::optional<NextHopId> previous = routes_->Find(prefix);
  if (!previous) {
    return std::unexpected(RouteError::kNotFound);
  }
  if (auto erased = routes_->Erase(prefix); !erased) {
    return erased;
  }
  references_[previous->value()]--;
  return {};
}

size_t Router::next_hop_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_hops_.size();  // live, not counting retiring ones
}

size_t Router::RouteReferences(NextHopId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_ != nullptr) {
    return engine_->ReferenceCount(next_hops_name_, dataplane::EncodeKey(id));
  }
  return ValidId(id) ? references_[id.value()] : 0;
}

}  // namespace bess::route
