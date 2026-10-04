// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_NEIGHBOR_TABLE_H_
#define BESS_ROUTE_NEIGHBOR_TABLE_H_

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "dataplane/interface_id.h"
#include "route/next_hop_id.h"
#include "route/router.h"
#include "utils/ether.h"

namespace bess::route {

// Neighbor state, kept apart from the routes and from how it is learned
// (M15, D-065). A neighbor is (interface, IPv4 address); next hops are bound
// to it. When resolution learns something -- an ARP reply parsed with
// ParseArp, an ND advertisement later, or a controller's statement -- Update
// returns the next-hop objects to republish, and Publish writes them to a
// Router: neighbor changes are next-hop updates, never route changes, and a
// controller-programmed neighbor and an ARP-resolved one end in the same
// NextHop object. No ARP is sent or answered here (BuildArpRequest and
// BuildArpReply are the packet helpers; a resolver module is the
// application's).
//
// Control side only: not thread-safe, never read by workers.
class NeighborTable {
 public:
  struct Key {
    dataplane::InterfaceId interface;
    uint32_t ipv4;  // host order
    friend auto operator<=>(const Key &, const Key &) = default;
  };
  struct Neighbor {
    NeighborState state = NeighborState::kIncomplete;
    utils::Ethernet::Address mac{};
  };
  using Updates = std::vector<std::pair<NextHopId, NextHop>>;

  // Binds next hop `hop` to neighbor `key`, sending from `src_mac` (the
  // interface's address). Returns the NextHop to publish for it now: the
  // neighbor's state and MAC if known, kIncomplete otherwise. Rebinding a hop
  // moves it. Every allocation comes before the first change, so a refused
  // one (std::bad_alloc) leaves the table as it was, the hop's old binding
  // included.
  NextHop Bind(NextHopId hop, Key key, const utils::Ethernet::Address &src_mac) {
    std::set<NextHopId> hop_node{hop};
    std::map<NextHopId, Binding> binding_node{{hop, Binding{key, src_mac}}};
    Entry &entry = neighbors_.try_emplace(key).first->second;
    Unbind(hop, &entry);
    entry.hops.insert(hop_node.extract(hop_node.begin()));
    bindings_.insert(binding_node.extract(binding_node.begin()));
    return Make(key, entry.neighbor, src_mac);
  }

  // Forgets `hop`'s binding (a neighbor with no hops and never learned is
  // dropped). The next hop itself is the caller's to remove.
  void Unbind(NextHopId hop) { Unbind(hop, nullptr); }

  // Records what resolution learned about `key`. Returns every bound next hop
  // with its new object (empty when nothing a next hop carries changed). The
  // list is built before the neighbor changes, so a refused allocation leaves
  // it unchanged and a retry still reports every hop.
  Updates Update(Key key, NeighborState state, const utils::Ethernet::Address &mac) {
    // A new entry has no hops: reserving for them does not allocate.
    Entry &entry = neighbors_.try_emplace(key).first->second;
    const Neighbor learned{state, mac};
    Updates updates;
    if (entry.neighbor.state != state || entry.neighbor.mac != mac) {
      updates.reserve(entry.hops.size());
      for (const NextHopId hop : entry.hops) {
        updates.emplace_back(hop, Make(key, learned, bindings_.at(hop).src_mac));
      }
    }
    entry.learned = true;
    entry.neighbor = learned;
    return updates;
  }

  std::optional<Neighbor> Find(Key key) const {
    auto n = neighbors_.find(key);
    if (n == neighbors_.end()) {
      return std::nullopt;
    }
    return n->second.neighbor;
  }
  size_t size() const noexcept { return neighbors_.size(); }

 private:
  struct Entry {
    Neighbor neighbor;
    bool learned = false;
    std::set<NextHopId> hops;
  };
  struct Binding {
    Key key;
    utils::Ethernet::Address src_mac;
  };

  static NextHop Make(Key key, const Neighbor &n, const utils::Ethernet::Address &src_mac) {
    NextHop hop;
    hop.egress = key.interface;
    hop.neighbor = n.state;
    hop.dst_mac = n.state == NeighborState::kResolved ? n.mac : utils::Ethernet::Address{};
    hop.src_mac = src_mac;
    return hop;
  }

  // Unbind, except that `keep` (Bind's target) is not dropped when it is left
  // empty. Only erases: it cannot fail.
  void Unbind(NextHopId hop, const Entry *keep) {
    auto b = bindings_.find(hop);
    if (b == bindings_.end()) {
      return;
    }
    auto n = neighbors_.find(b->second.key);
    n->second.hops.erase(hop);
    if (n->second.hops.empty() && !n->second.learned && &n->second != keep) {
      neighbors_.erase(n);
    }
    bindings_.erase(b);
  }

  std::map<Key, Entry> neighbors_;
  std::map<NextHopId, Binding> bindings_;
};

// Publishes Update's (or Bind's) next hops to `router`: one SetNextHop each,
// O(1) per hop, no route touched. Stops at the first error.
inline std::expected<void, RouteError> Publish(Router &router,
                                               std::span<const std::pair<NextHopId, NextHop>> updates) {
  for (const auto &[id, hop] : updates) {
    if (auto set = router.SetNextHop(id, hop); !set) {
      return set;
    }
  }
  return {};
}

}  // namespace bess::route

#endif  // BESS_ROUTE_NEIGHBOR_TABLE_H_
