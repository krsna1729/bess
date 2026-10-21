// SPDX-License-Identifier: BSD-3-Clause

#include "route/connected_routes.h"

#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "route/connected_route.h"

namespace bess::route {
namespace {

// The deterministic order and the stable-key order: domain, then prefix, then
// kind. Duplicate detection runs on this order, so two entries with the same
// key are adjacent.
bool Order(const DerivedRoute &a, const DerivedRoute &b) {
  return std::tie(a.domain, a.prefix, a.kind) <
         std::tie(b.domain, b.prefix, b.kind);
}

bool SameRoute(const DerivedRoute &a, const DerivedRoute &b) {
  return a.domain == b.domain && a.prefix == b.prefix;
}

bool SameHop(const NextHop &a, const NextHop &b) {
  return a.egress == b.egress && a.neighbor == b.neighbor &&
         a.kind == b.kind && a.dst_mac == b.dst_mac && a.src_mac == b.src_mac;
}

}  // namespace

const char *DerivationErrorName(DerivationError error) {
  switch (error) {
    case DerivationError::kInvalidInterface:
      return "invalid interface id";
    case DerivationError::kInvalidPrefixLength:
      return "invalid prefix length";
    case DerivationError::kConflictingRoute:
      return "two interfaces derive the same route";
    case DerivationError::kInvalidRoute:
      return "a derived route's kind and egress disagree";
    case DerivationError::kHopCountMismatch:
      return "one next-hop id per derived route was not given";
    case DerivationError::kInvalidHopId:
      return "next-hop id 0 is invalid";
    case DerivationError::kDuplicateHopId:
      return "one next-hop id names two different hops";
  }
  return "unknown derivation error";
}

std::expected<DerivedRoutes, DerivationError> DerivedRoutes::Derive(
    std::span<const InterfaceAddress> interfaces) {
  std::vector<DerivedRoute> entries;
  entries.reserve(interfaces.size() * 2);
  for (const InterfaceAddress &interface : interfaces) {
    if (interface.interface == dataplane::kInvalidInterfaceId) {
      return std::unexpected(DerivationError::kInvalidInterface);
    }
    if (interface.length > 32) {
      return std::unexpected(DerivationError::kInvalidPrefixLength);
    }
    // The connected route is the interface's subnet, with the one arithmetic
    // D-164 owns. A /32 address has no subnet beyond the address itself: its
    // connected and local entries would share the key, and the local one is
    // the honest meaning of the address.
    if (HasFlag(interface.flags, DerivedRouteFlags::kConnected) &&
        interface.length < 32) {
      const auto connected =
          ConnectedRouteFor(interface.address, interface.length, interface.interface);
      if (!connected) {
        // Only a length above 32 can fail, and it was checked above.
        return std::unexpected(DerivationError::kInvalidPrefixLength);
      }
      entries.push_back(DerivedRoute{.domain = interface.domain,
                                     .kind = DerivedRouteKind::kConnected,
                                     .prefix = connected->prefix,
                                     .interface = interface.interface,
                                     .egress = interface.interface});
    }
    // The local route is the address itself, a host route: a packet to the
    // node's own address is delivered here, not forwarded to the subnet's hop.
    if (HasFlag(interface.flags, DerivedRouteFlags::kLocal)) {
      const auto prefix = Ipv4Prefix::Make(interface.address, 32);
      if (!prefix) {
        return std::unexpected(DerivationError::kInvalidPrefixLength);
      }
      entries.push_back(DerivedRoute{.domain = interface.domain,
                                     .kind = DerivedRouteKind::kLocal,
                                     .prefix = *prefix,
                                     .interface = interface.interface,
                                     .egress = dataplane::kInvalidInterfaceId});
    }
  }

  std::sort(entries.begin(), entries.end(), Order);
  std::vector<DerivedRoute> unique;
  unique.reserve(entries.size());
  for (const DerivedRoute &entry : entries) {
    if (!unique.empty() && SameRoute(unique.back(), entry)) {
      // A duplicate input (the same address twice, or a /32 address whose
      // local route is also its implicit connected one) collapses; two
      // different interfaces deriving the same key is a conflict the
      // controller must resolve, not one the library picks an interface for.
      if (unique.back().kind == entry.kind &&
          unique.back().interface == entry.interface) {
        continue;
      }
      return std::unexpected(DerivationError::kConflictingRoute);
    }
    unique.push_back(entry);
  }
  return DerivedRoutes(std::move(unique));
}

const DerivedRoute *DerivedRoutes::Find(const RouteKey &key) const noexcept {
  const auto it = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const DerivedRoute &entry, const RouteKey &wanted) {
        return entry.key() < wanted;
      });
  if (it == entries_.end() || !(it->key() == key)) {
    return nullptr;
  }
  return &*it;
}

std::expected<std::vector<dataplane::Op>, DerivationError> InstallationOps(
    const Router &router, std::span<const DerivedRoute> entries,
    std::span<const NextHopId> hop_ids) {
  if (entries.size() != hop_ids.size()) {
    return std::unexpected(DerivationError::kHopCountMismatch);
  }
  std::vector<dataplane::Op> ops;
  ops.reserve(entries.size() * 2);
  std::set<RouteKey> keys;
  std::map<uint32_t, NextHop> id_hops;
  for (size_t i = 0; i < entries.size(); i++) {
    const DerivedRoute &entry = entries[i];
    const NextHopId id = hop_ids[i];
    if (id == kInvalidNextHopId) {
      return std::unexpected(DerivationError::kInvalidHopId);
    }
    // The entry's own consistency: it names the interface its address came
    // from; a connected route forwards out that interface, and a local one has
    // no egress. A controller that built entries by hand gets its mistake back
    // instead of a hop that claims both.
    if (entry.interface == dataplane::kInvalidInterfaceId) {
      return std::unexpected(DerivationError::kInvalidRoute);
    }
    if (entry.IsLocal() ? entry.egress != dataplane::kInvalidInterfaceId
                        : entry.egress != entry.interface) {
      return std::unexpected(DerivationError::kInvalidRoute);
    }
    if (!keys.insert(entry.key()).second) {
      return std::unexpected(DerivationError::kConflictingRoute);
    }
    const NextHop hop = entry.hop();
    const auto [it, inserted] = id_hops.try_emplace(id.value(), hop);
    if (!inserted && !SameHop(it->second, hop)) {
      // One id can only name one hop; two routes naming different hops with
      // one id would silently point at whichever upsert ran last.
      return std::unexpected(DerivationError::kDuplicateHopId);
    }
    if (inserted) {
      ops.push_back(router.SetNextHopOp(id, hop));
    }
    ops.push_back(router.SetRouteOp(entry.domain, entry.prefix, id));
  }
  return ops;
}

std::vector<dataplane::Op> EraseOps(const Router &router,
                                    std::span<const RouteKey> stale) {
  std::vector<dataplane::Op> ops;
  ops.reserve(stale.size());
  for (const RouteKey &key : stale) {
    ops.push_back(router.RemoveRouteOp(key.domain, key.prefix));
  }
  return ops;
}

std::vector<RouteKey> StaleRouteKeys(std::span<const DerivedRoute> previous,
                                     std::span<const DerivedRoute> next) {
  std::set<RouteKey> next_keys;
  for (const DerivedRoute &entry : next) {
    next_keys.insert(entry.key());
  }
  std::vector<RouteKey> stale;
  for (const DerivedRoute &entry : previous) {
    if (!next_keys.contains(entry.key())) {
      stale.push_back(entry.key());
    }
  }
  std::sort(stale.begin(), stale.end());
  stale.erase(std::unique(stale.begin(), stale.end()), stale.end());
  return stale;
}

std::optional<uint8_t> PrefixLengthFromMask(uint32_t mask) {
  if (mask == 0) {
    return 0;
  }
  const uint32_t host_bits = ~mask;
  // A contiguous netmask's host bits are a run of low ones: adding one carries
  // through them, so the two share no bit. A hole in the mask leaves one.
  if ((host_bits & (host_bits + 1)) != 0) {
    return std::nullopt;
  }
  return static_cast<uint8_t>(32 - std::popcount(host_bits));
}

}  // namespace bess::route
