// The connected route an interface's address implies (D-164): the subnet the
// address sits on is reachable on that interface. The route's hop is the
// interface itself with no neighbor resolved -- the hop names the interface,
// not a neighbor, so nothing resolves it: reaching a host on the subnet is a
// /32 route and hop the application writes when resolution learns the host
// (`NeighborTable::Bind` is what makes a learned neighbor update a hop, and
// binding this one to a host would point the whole subnet at that host's MAC)
// -- so the route exists from the moment the interface does.
//
// Two applications must not disagree about the arithmetic: the mask is the
// address's length, the prefix is the address with its host bits cleared (the
// table's own `Ipv4Prefix::Make` refuses host bits, so clearing them is this
// helper's job), and the hop is the interface with an unresolved neighbor.

#pragma once

#include <cstdint>
#include <optional>

#include "dataplane/interface_id.h"
#include "route/router.h"
#include "route/route_table.h"

namespace bess::route {

struct ConnectedRoute {
  // The interface's subnet: the address with its host bits cleared.
  Ipv4Prefix prefix;
  // The hop to install for it: the interface, no neighbor resolved yet.
  NextHop hop;
};

// The connected route for the interface `egress` whose own address is
// `address` (host order) with prefix length `length`. `nullopt` when `length`
// is not a prefix length (above 32): the configuration is wrong, not the
// arithmetic.
inline std::optional<ConnectedRoute> ConnectedRouteFor(uint32_t address, uint8_t length,
                                                       dataplane::InterfaceId egress) {
  if (length > 32) {
    return std::nullopt;
  }
  const uint32_t mask = length == 0 ? 0 : ~uint32_t{0} << (32u - length);
  const auto prefix = Ipv4Prefix::Make(address & mask, length);
  if (!prefix) {
    return std::nullopt;
  }
  return ConnectedRoute{.prefix = *prefix,
                        .hop = NextHop{.egress = egress,
                                       .neighbor = NeighborState::kIncomplete}};
}

}  // namespace bess::route
