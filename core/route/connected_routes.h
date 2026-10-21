// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_CONNECTED_ROUTES_H_
#define BESS_ROUTE_CONNECTED_ROUTES_H_

// Deriving the routes an interface's own addresses imply (D-178).
//
// Two routes come from one address+prefix:
//
//  - the **connected route**: the subnet the address sits on is reachable on
//    that interface (`ConnectedRouteFor`, D-164), its hop naming the interface
//    with no neighbor resolved;
//  - the **local route**: the address itself is this node's, so a packet to it
//    is delivered locally (`LocalNextHop`), never forwarded.
//
// The library derives and orders entries and builds the transaction operations
// that install them. It writes no table: the controller owns installation and
// submits the operations through the Router's resources and the transaction
// engine, so a failed transaction leaves no partial derived state and the same
// derivation can be retried.
//
// What the library decides, because two controllers must not disagree:
//
//  - the prefix arithmetic (`Ipv4Prefix::Make` refuses host bits, so masking is
//    the library's job);
//  - that a /32 address has no connected route distinct from its local route
//    (both would be the same key), while a /31, /30, ... has both;
//  - the deterministic order (domain, prefix address, prefix length, kind) and
//    the stable route key (domain + prefix);
//  - that two interfaces deriving the same route key in one domain is a
//    conflict, refused rather than resolved by guessing an interface;
//  - that duplicate inputs collapse (one address repeated, or the same address
//    twice on one interface).
//
// What it does not decide: next-hop ids (the controller's namespace, shared
// with the rest of its routes), route domains' validity for a particular
// Router (the resource or the engine refuses an out-of-range domain), and what
// a local packet becomes (a gate, a counter, a stack).

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "dataplane/interface_id.h"
#include "dataplane/resource.h"
#include "route/next_hop_id.h"
#include "route/route_domain.h"
#include "route/route_table.h"
#include "route/router.h"

namespace bess::route {

// Whether a derived entry is the interface's subnet or the node's own address.
// The integer order is the tie-break the entries use when two kinds share a
// prefix (they do not today; a future kind must still be deterministic).
enum class DerivedRouteKind : uint8_t {
  kConnected = 0,
  kLocal = 1,
};

// What to derive for one address. The defaults are both, which is what an
// interface address means; a caller that only wants one half says so here.
enum class DerivedRouteFlags : uint8_t {
  kNone = 0,
  kConnected = 1u << 0u,
  kLocal = 1u << 1u,
  kAll = kConnected | kLocal,
};

constexpr DerivedRouteFlags operator|(DerivedRouteFlags a, DerivedRouteFlags b) {
  return static_cast<DerivedRouteFlags>(static_cast<uint8_t>(a) |
                                        static_cast<uint8_t>(b));
}
constexpr DerivedRouteFlags operator&(DerivedRouteFlags a, DerivedRouteFlags b) {
  return static_cast<DerivedRouteFlags>(static_cast<uint8_t>(a) &
                                        static_cast<uint8_t>(b));
}
constexpr bool HasFlag(DerivedRouteFlags flags, DerivedRouteFlags flag) {
  return (flags & flag) != DerivedRouteFlags::kNone;
}

// One configured interface address. `address` is host order (the LPM's own
// order); `length` is the prefix length, and the mask that goes with it is
// derived, never stored.
struct InterfaceAddress {
  dataplane::InterfaceId interface;
  RouteDomainId domain = kDefaultRouteDomainId;
  uint32_t address = 0;
  uint8_t length = 0;
  DerivedRouteFlags flags = DerivedRouteFlags::kAll;
};

// Why a derivation or an installation plan was refused. These are
// configuration errors, not table-capacity errors: the router and the engine
// report those when the controller applies the plan.
enum class DerivationError : uint8_t {
  kInvalidInterface,     // InterfaceId 0 names no interface
  kInvalidPrefixLength,  // above 32
  kConflictingRoute,     // two interfaces derive the same (domain, prefix)
  kInvalidRoute,         // a plan entry's kind and egress disagree
  kHopCountMismatch,     // one next-hop id per entry is not one per entry
  kInvalidHopId,         // next-hop id 0
  kDuplicateHopId,       // one id assigned to two different hops
};

const char *DerivationErrorName(DerivationError error);

// One derived route. `prefix` and `domain` are its stable key; `kind` says
// whether `egress` forwards the subnet or `hop()` delivers locally.
struct DerivedRoute {
  RouteDomainId domain = kDefaultRouteDomainId;
  DerivedRouteKind kind = DerivedRouteKind::kConnected;
  Ipv4Prefix prefix;
  // The configured interface the address belongs to (both kinds): what makes
  // two interfaces deriving one key a conflict even though a local hop itself
  // has no egress.
  dataplane::InterfaceId interface = dataplane::kInvalidInterfaceId;
  // The connected route's forwarding interface; kInvalidInterfaceId for a
  // local route, which names no egress.
  dataplane::InterfaceId egress = dataplane::kInvalidInterfaceId;

  RouteKey key() const { return RouteKey{.domain = domain, .prefix = prefix}; }
  bool IsLocal() const { return kind == DerivedRouteKind::kLocal; }
  // The hop this entry installs: the interface with no neighbor resolved, or
  // the local-delivery hop.
  NextHop hop() const { return IsLocal() ? LocalNextHop() : ConnectedHop(); }

  friend bool operator==(const DerivedRoute &, const DerivedRoute &) = default;

 private:
  NextHop ConnectedHop() const {
    return NextHop{.egress = egress, .neighbor = NeighborState::kIncomplete};
  }
};

// The deterministic set of derived routes. Entries are sorted by (domain,
// prefix address, prefix length, kind) and unique by key.
class DerivedRoutes {
 public:
  static std::expected<DerivedRoutes, DerivationError> Derive(
      std::span<const InterfaceAddress> interfaces);

  std::span<const DerivedRoute> entries() const noexcept { return entries_; }
  size_t size() const noexcept { return entries_.size(); }
  bool empty() const noexcept { return entries_.empty(); }
  // The entry with `key`, or nullptr. A binary search over the stable order.
  const DerivedRoute *Find(const RouteKey &key) const noexcept;

 private:
  explicit DerivedRoutes(std::vector<DerivedRoute> entries)
      : entries_(std::move(entries)) {}

  std::vector<DerivedRoute> entries_;
};

// The operations that install `entries` through `router`'s next-hop and route
// resources, for the controller to submit as one transaction: each entry's hop
// is upserted, then its route. `hop_ids[i]` names `entries[i]`'s next hop;
// the controller owns the ids (they share a namespace with its other routes).
//
// The plan is refused as a whole when `entries` holds the same route key twice,
// a hop id is 0 or shared by two different hops, or an entry's kind and egress
// disagree. Nothing is written: this builds operations only.
std::expected<std::vector<dataplane::Op>, DerivationError> InstallationOps(
    const Router &router, std::span<const DerivedRoute> entries,
    std::span<const NextHopId> hop_ids);

// Operations that erase the route keys in `stale`. The controller applies them
// in the same transaction as the plan that replaced them (address removal or
// change), after the new routes are in place; the engine orders the resources.
std::vector<dataplane::Op> EraseOps(const Router &router,
                                    std::span<const RouteKey> stale);

// The keys `previous` derives but `next` does not: what an address removal or
// change leaves behind. Sorted in the derivation order, unique.
std::vector<RouteKey> StaleRouteKeys(std::span<const DerivedRoute> previous,
                                     std::span<const DerivedRoute> next);

// The prefix length of a contiguous IPv4 netmask, or nullopt for a
// non-contiguous one (a configuration error, not a length). 0.0.0.0 is /0;
// 255.255.255.255 is /32.
std::optional<uint8_t> PrefixLengthFromMask(uint32_t mask);

}  // namespace bess::route

#endif  // BESS_ROUTE_CONNECTED_ROUTES_H_
