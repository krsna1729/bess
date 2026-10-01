// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_ROUTE_DOMAIN_H_
#define BESS_ROUTE_ROUTE_DOMAIN_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dataplane/strong_id.h"
#include "route/next_hop_id.h"
#include "route/route_table.h"

namespace bess::route {

// Identifies a virtual routing and forwarding (VRF) or network instance
// domain (K7.1, M6; e.g. N3 access vs N6 core vs N9 roaming in OMEC UPF).
// Domains are owned by a Router (route/router.h), which indexes them densely:
// the id is the domain's slot, so ids are small and a Router is created with
// the bound it will accept. Nothing here depends on gates, modules or
// metadata.
struct RouteDomainIdTag;
using RouteDomainId = dataplane::StrongId<RouteDomainIdTag, uint32_t>;
inline constexpr RouteDomainId kDefaultRouteDomainId{0};

// The domain is part of a route's transaction-resource key in bits 40..63
// (Router::RouteKey), so a Router holds at most this many domains.
inline constexpr size_t kMaxRouteDomains = size_t{1} << 24;

// A route's identity: the same prefix in two domains is two routes.
struct RouteKey {
  RouteDomainId domain;
  Ipv4Prefix prefix;

  friend constexpr bool operator==(const RouteKey &,
                                   const RouteKey &) = default;
  friend constexpr auto operator<=>(const RouteKey &,
                                    const RouteKey &) = default;
};

// A route definition within a domain.
struct RouteEntry {
  Ipv4Prefix prefix;
  NextHopId hop;

  constexpr bool operator==(const RouteEntry &) const = default;
};

using RouteSet = std::vector<RouteEntry>;

}  // namespace bess::route

#endif  // BESS_ROUTE_ROUTE_DOMAIN_H_
