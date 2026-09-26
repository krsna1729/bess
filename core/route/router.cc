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

namespace bess::route {

std::expected<std::unique_ptr<Router>, RouteError> Router::Create(
    std::string name, const Config &config, size_t max_next_hops,
    rcu::RcuDomain &domain) {
  if (max_next_hops == 0 || max_next_hops > LpmRouteTable::kMaxValue) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  auto routes = RouteTable<NextHopId>::Create(std::move(name), config, domain);
  if (!routes) {
    return std::unexpected(routes.error());
  }
  std::unique_ptr<Router> router(
      new Router(std::move(*routes), max_next_hops, domain));
  router->PublishNextHops();
  return router;
}

Router::Router(std::unique_ptr<RouteTable<NextHopId>> routes,
               size_t max_next_hops, rcu::RcuDomain &domain)
    : routes_(std::move(routes)),
      domain_(domain),
      next_hops_(domain),
      desired_(max_next_hops + 1),
      references_(max_next_hops + 1, 0),
      is_retiring_(max_next_hops + 1, false) {}

Router::~Router() { next_hops_.ResetQuiesced(); }

void Router::PublishNextHops() {
  dataplane::ObjectTableBuilder<NextHopId, NextHop> builder(desired_.size() -
                                                            1);
  for (size_t i = 1; i < desired_.size(); i++) {
    if (desired_[i]) {
      builder.Set(NextHopId(static_cast<uint32_t>(i)), *desired_[i]);
    }
  }
  auto table = std::move(builder).Build();
  if (next_hops_.Read() == nullptr) {
    next_hops_.Initialize(std::move(table));
    return;
  }
  next_hops_.Publish(std::move(table));
  domain_.ReclaimReady();
}

size_t Router::CompleteRetirementsLocked() {
  const size_t before = retiring_.size();
  std::erase_if(retiring_, [&](const Retiring &r) {
    if (!domain_.IsComplete(r.token)) {
      return false;
    }
    desired_[r.id.value()].reset();
    is_retiring_[r.id.value()] = false;
    return true;
  });
  if (retiring_.size() != before) {
    PublishNextHops();
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
  CompleteRetirementsLocked();
  if (!ValidId(id)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (is_retiring_[id.value()]) {
    return std::unexpected(RouteError::kNextHopRetiring);
  }
  desired_[id.value()] = hop;
  PublishNextHops();
  return {};
}

std::expected<void, RouteError> Router::RemoveNextHop(NextHopId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  CompleteRetirementsLocked();
  if (!ValidId(id)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!desired_[id.value()] || is_retiring_[id.value()]) {
    return std::unexpected(RouteError::kUnknownNextHop);
  }
  if (references_[id.value()] != 0) {
    return std::unexpected(RouteError::kNextHopInUse);
  }
  // No route names `id` any more, but a reader may have looked one up just
  // before the last such route went and not yet loaded the next-hop table.
  // Keep it published until every reader has passed a quiescent state; a
  // later control call drops it. No waiting here (a stalled worker must not
  // stall the command path, and a transaction must not hold a blocking wait).
  retiring_.push_back({id, domain_.StartGracePeriod()});
  is_retiring_[id.value()] = true;
  return {};
}

std::expected<void, RouteError> Router::SetRoute(Ipv4Prefix prefix,
                                                 NextHopId hop) {
  std::lock_guard<std::mutex> lock(mutex_);
  CompleteRetirementsLocked();
  if (!ValidId(hop)) {
    return std::unexpected(RouteError::kInvalidId);
  }
  if (!desired_[hop.value()] || is_retiring_[hop.value()]) {
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
  size_t live = 0;
  for (size_t i = 1; i < desired_.size(); i++) {
    live += desired_[i] && !is_retiring_[i];
  }
  return live;
}

size_t Router::RouteReferences(NextHopId id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ValidId(id) ? references_[id.value()] : 0;
}

}  // namespace bess::route
