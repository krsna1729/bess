// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_ROUTE_DOMAIN_H_
#define BESS_ROUTE_ROUTE_DOMAIN_H_

#include <algorithm>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/strong_id.h"
#include "route/route_table.h"
#include "route/router.h"

namespace bess::route {

// Identifies a virtual routing and forwarding (VRF) or network instance domain
// (K7.1, e.g. N3 access vs N6 core vs N9 roaming in OMEC UPF).
struct RouteDomainIdTag;
using RouteDomainId = dataplane::StrongId<RouteDomainIdTag, uint32_t>;
inline constexpr RouteDomainId kDefaultRouteDomainId{0};

// A route definition within a domain.
struct RouteEntry {
  Ipv4Prefix prefix;
  NextHopId hop;

  constexpr bool operator==(const RouteEntry &) const = default;
};

using RouteSet = std::vector<RouteEntry>;

// Manages multiple independent route domains (VRFs), each with its own
// LpmRouteTable, allowing overlapping private subscriber subnets across
// separate network instances (e.g. N3/N6/N9).
class MultiDomainRouter {
 public:
  using Config = LpmRouteTable::Config;

  static std::expected<std::unique_ptr<MultiDomainRouter>, RouteError> Create(
      std::string name, const Config &default_config, size_t max_next_hops,
      rcu::RcuDomain &domain) {
    auto router = std::make_unique<MultiDomainRouter>(std::move(name),
                                                      default_config, domain);
    // Create the default domain (Domain 0)
    auto default_table = LpmRouteTable::Create(
        router->name_ + "_d0", default_config, domain);
    if (!default_table) {
      return std::unexpected(default_table.error());
    }
    router->domains_[kDefaultRouteDomainId] = std::move(*default_table);
    return router;
  }

  MultiDomainRouter(std::string name, Config default_config,
                    rcu::RcuDomain &domain)
      : name_(std::move(name)),
        default_config_(default_config),
        rcu_domain_(domain) {}

  ~MultiDomainRouter() = default;

  // -- Domain management ------------------------------------------------------

  std::expected<void, RouteError> CreateDomain(RouteDomainId domain_id,
                                              const Config &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (domains_.contains(domain_id)) {
      return std::unexpected(RouteError::kEnrolled);
    }
    std::string table_name = name_ + "_d" + std::to_string(domain_id.value());
    auto table = LpmRouteTable::Create(table_name, config, rcu_domain_);
    if (!table) {
      return std::unexpected(table.error());
    }
    domains_[domain_id] = std::move(*table);
    return {};
  }

  bool HasDomain(RouteDomainId domain_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return domains_.contains(domain_id);
  }

  size_t domain_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return domains_.size();
  }

  // -- Route management per domain --------------------------------------------

  std::expected<void, RouteError> SetRoute(RouteDomainId domain_id,
                                          Ipv4Prefix prefix, NextHopId hop) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = domains_.find(domain_id);
    if (it == domains_.end()) {
      return std::unexpected(RouteError::kNotFound);
    }
    return it->second->Upsert(prefix, hop.value());
  }

  std::expected<void, RouteError> RemoveRoute(RouteDomainId domain_id,
                                             Ipv4Prefix prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = domains_.find(domain_id);
    if (it == domains_.end()) {
      return std::unexpected(RouteError::kNotFound);
    }
    return it->second->Erase(prefix);
  }

  // Atomically replaces or populates an entire route set in a domain.
  std::expected<void, RouteError> ApplyRouteSet(RouteDomainId domain_id,
                                               const RouteSet &route_set) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = domains_.find(domain_id);
    if (it == domains_.end()) {
      return std::unexpected(RouteError::kNotFound);
    }
    for (const auto &entry : route_set) {
      auto res = it->second->Upsert(entry.prefix, entry.hop.value());
      if (!res) {
        return res;
      }
    }
    return {};
  }

  // -- Dataplane reader lookup ------------------------------------------------

  [[nodiscard]] NextHopId Lookup(RouteDomainId domain_id,
                                 uint32_t ipv4) const noexcept {
    // Lock-free domain lookup: in steady state, domains are established at config
    auto it = domains_.find(domain_id);
    if (unlikely(it == domains_.end())) {
      return kInvalidNextHopId;
    }
    auto view = it->second->Read();
    auto val = view.Lookup(ipv4);
    if (val.has_value()) {
      return NextHopId(*val);
    }
    return kInvalidNextHopId;
  }

  // 4-wide batch lookup within a domain.
  void LookupBatchX4(RouteDomainId domain_id, const uint32_t ips[4],
                     NextHopId hops[4]) const noexcept {
    auto it = domains_.find(domain_id);
    if (unlikely(it == domains_.end())) {
      for (int i = 0; i < 4; i++) {
        hops[i] = kInvalidNextHopId;
      }
      return;
    }
    uint32_t vals[4] = {0, 0, 0, 0};
    auto view = it->second->Read();
    uint64_t hit_mask = view.LookupBatch(std::span<const uint32_t>(ips, 4),
                                         std::span<uint32_t>(vals, 4));
    for (int i = 0; i < 4; i++) {
      hops[i] = (hit_mask & (1ull << i)) ? NextHopId(vals[i]) : kInvalidNextHopId;
    }
  }

  const std::string &name() const noexcept { return name_; }

 private:
  const std::string name_;
  const Config default_config_;
  rcu::RcuDomain &rcu_domain_;

  mutable std::mutex mutex_;
  std::map<RouteDomainId, std::unique_ptr<LpmRouteTable>> domains_;
};

}  // namespace bess::route

#endif  // BESS_ROUTE_ROUTE_DOMAIN_H_
