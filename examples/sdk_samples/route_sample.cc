// SPDX-License-Identifier: BSD-3-Clause

// Router (route/router.h), the smallest useful program: create a router, add a
// next hop and a route, resolve a batch, update and remove, and handle every
// failure as a value. Ownership: the router is a unique_ptr the caller owns;
// next hops are referenced by id, and a next hop still named by a route cannot
// be removed (kNextHopInUse) -- the caller removes the route first.

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

#include "rcu/rcu_domain.h"
#include "route/router.h"

namespace sample {

using bess::route::Ipv4Prefix;
using bess::route::NextHop;
using bess::route::NextHopId;
using bess::route::Router;
using bess::route::RouteErrorName;

// `domain` is the reclamation domain the router retires replaced tables to;
// in a module it comes from the init context.
bool RouteSample(bess::rcu::RcuDomain &domain) {
  auto router = Router::Create("sample", Router::Config{.max_routes = 64, .tbl8_groups = 4},
                               /*max_next_hops=*/16, domain);
  if (!router) {
    std::fprintf(stderr, "create: %s\n", RouteErrorName(router.error()));
    return false;
  }
  NextHop hop;
  hop.egress = bess::dataplane::InterfaceId{uint16_t{3}};
  if (auto set = (*router)->SetNextHop(NextHopId{1}, hop); !set) {
    std::fprintf(stderr, "next hop: %s\n", RouteErrorName(set.error()));
    return false;
  }
  auto prefix = Ipv4Prefix::Make(0x0a000000, 8);  // 10.0.0.0/8
  if (!prefix || !(*router)->SetRoute(*prefix, NextHopId{1})) {
    return false;
  }
  // Typed hot path: destinations in host order, next hops out.
  const std::array<uint32_t, 2> dst = {0x0a010203, 0xc0a80001};
  std::array<const NextHop *, 2> hops{};
  const uint64_t found = (*router)->ResolveBatch(dst, hops);
  // Bit 0: 10.1.2.3 matched; bit 1 unset: 192.168.0.1 has no route.
  if (found != 0b01 || hops[0]->egress != hop.egress) {
    return false;
  }
  // A next hop a route still names is refused; remove the route first.
  if ((*router)->RemoveNextHop(NextHopId{1})) {
    return false;
  }
  return (*router)->RemoveRoute(*prefix) && (*router)->RemoveNextHop(NextHopId{1});
}

}  // namespace sample
