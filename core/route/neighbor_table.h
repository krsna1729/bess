// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_NEIGHBOR_TABLE_H_
#define BESS_ROUTE_NEIGHBOR_TABLE_H_

#include <compare>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "dataplane/interface_id.h"
#include "route/neighbor_key.h"
#include "route/next_hop_id.h"
#include "route/router.h"
#include "utils/ether.h"
#include "utils/ip.h"

namespace bess::route {

// Neighbor state, kept apart from the routes and from how it is learned
// (M15, D-065, D-174). A neighbor is (interface, family-tagged address); next
// hops are bound to it. When resolution learns something -- an ARP reply
// parsed with ParseArp, an ND advertisement parsed with
// ParseNeighborDiscovery, or a controller's statement -- Update
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
  using Key = NeighborKey;
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

  // Drops what was learned about `key` once no next hop is bound to it, so a
  // later binding starts incomplete instead of reusing an old MAC. A key that
  // still has bindings is kept.
  void Forget(Key key) {
    auto n = neighbors_.find(key);
    if (n != neighbors_.end() && n->second.hops.empty()) {
      neighbors_.erase(n);
    }
  }

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

  // Everything an update needs, built without changing the table. Prepare()
  // is the fallible half (it allocates the update vector); Commit() is
  // noexcept and changes the neighbor and, when `src_mac` is set, every
  // binding's source MAC. A caller that changes several neighbors at once
  // prepares them all first, so a refused allocation leaves every one of them
  // untouched; this is what an interface identity change and the timer wheel's
  // due transitions use.
  struct Prepared {
    Key key{};
    Neighbor neighbor{};
    Updates updates;
    std::optional<utils::Ethernet::Address> src_mac;
    bool changed = false;
  };

  Prepared Prepare(Key key, NeighborState state,
                   const utils::Ethernet::Address &mac,
                   std::optional<utils::Ethernet::Address> src_mac =
                       std::nullopt) const {
    Prepared prepared;
    prepared.key = key;
    prepared.neighbor = Neighbor{state, mac};
    prepared.src_mac = src_mac;
    auto n = neighbors_.find(key);
    if (n == neighbors_.end()) {
      return prepared;
    }
    bool differs = n->second.neighbor.state != state ||
                   n->second.neighbor.mac != mac;
    if (src_mac) {
      for (const NextHopId hop : n->second.hops) {
        if (bindings_.at(hop).src_mac != *src_mac) {
          differs = true;
          break;
        }
      }
    }
    if (!differs) {
      return prepared;
    }
    prepared.changed = true;
    prepared.updates.reserve(n->second.hops.size());
    for (const NextHopId hop : n->second.hops) {
      const utils::Ethernet::Address &src =
          src_mac ? *src_mac : bindings_.at(hop).src_mac;
      prepared.updates.emplace_back(hop, Make(key, prepared.neighbor, src));
    }
    return prepared;
  }

  // Makes a prepared update visible. Infallible: every allocation happened in
  // Prepare(). A key that vanished meanwhile (its last hop unbound) is left
  // alone.
  void Commit(Prepared &prepared) noexcept {
    if (!prepared.changed) {
      return;
    }
    auto n = neighbors_.find(prepared.key);
    if (n == neighbors_.end()) {
      return;
    }
    if (prepared.src_mac) {
      for (const NextHopId hop : n->second.hops) {
        auto b = bindings_.find(hop);
        if (b != bindings_.end()) {
          b->second.src_mac = *prepared.src_mac;
        }
      }
    }
    n->second.learned = true;
    n->second.neighbor = prepared.neighbor;
  }

  // Records a neighbor state and moves every hop bound to `key` to
  // `src_mac`, returning the bound next hops' new objects when the state or a
  // binding's source MAC changed (empty otherwise). An interface identity
  // change uses this: the hops that stay bound must send from the interface's
  // new MAC, which only the bindings know (Bind captured it), and the caller
  // needs a new object for each of them. It is Prepare() + Commit() for one
  // key; a caller changing several keys stages all of them with Prepare()
  // first.
  Updates Reconfigure(Key key, const utils::Ethernet::Address &src_mac,
                      NeighborState state,
                      const utils::Ethernet::Address &mac) {
    Prepared prepared = Prepare(key, state, mac, src_mac);
    Commit(prepared);
    return std::move(prepared.updates);
  }

  std::optional<Neighbor> Find(Key key) const {
    auto n = neighbors_.find(key);
    if (n == neighbors_.end()) {
      return std::nullopt;
    }
    return n->second.neighbor;
  }
  // A next hop's binding: the neighbor it names and the source MAC Bind
  // captured for it.
  struct Bound {
    Key key;
    utils::Ethernet::Address src_mac;
  };
  // The binding of `hop`, if it is bound at all. A resolver uses this to
  // stop tracking a neighbor no next hop names any more, and to restore a
  // binding it moved.
  std::optional<Bound> BindingOf(NextHopId hop) const {
    auto b = bindings_.find(hop);
    if (b == bindings_.end()) {
      return std::nullopt;
    }
    return Bound{b->second.key, b->second.src_mac};
  }
  // Whether any next hop is bound to `key` now.
  bool HasBindings(Key key) const {
    auto n = neighbors_.find(key);
    return n != neighbors_.end() && !n->second.hops.empty();
  }
  // Next hops bound to `key` now (zero for an absent key).
  size_t BindingCount(Key key) const {
    auto n = neighbors_.find(key);
    return n == neighbors_.end() ? 0 : n->second.hops.size();
  }
  // Every neighbor key on `interface`, in key order. Control side only; a
  // resolver uses it to invalidate retained learning when an interface's
  // identity changes, including neighbors no hop is bound to any more.
  std::vector<Key> KeysForInterface(dataplane::InterfaceId interface) const {
    std::vector<Key> keys;
    for (const auto &[key, entry] : neighbors_) {
      if (key.interface == interface) {
        keys.push_back(key);
      }
    }
    return keys;
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

// The same updates as operations on `router`'s "<router>/next_hops" resource,
// for a caller that publishes them through the TransactionEngine (D-021). The
// transaction is all-or-nothing: if any operation is refused, none of the
// updates is visible. Visibility is the engine's referential order, not a
// snapshot -- upserts are published one next hop at a time (each object
// replaced whole), so a packet may see one hop's new adjacency and another's
// old one until the transaction finishes. That is enough for a neighbor
// update, where every next hop is independent; code that needs all of them to
// switch in one store must not use this (the Router's next-hop resource is
// kReferential and a kScopeSnapshot request is refused). The router must be
// enrolled in that engine. One op per update, in the updates' order.
inline std::vector<dataplane::Op> NextHopOps(
    const Router &router,
    std::span<const std::pair<NextHopId, NextHop>> updates) {
  std::vector<dataplane::Op> ops;
  ops.reserve(updates.size());
  for (const auto &[id, hop] : updates) {
    ops.push_back(router.SetNextHopOp(id, hop));
  }
  return ops;
}

}  // namespace bess::route

#endif  // BESS_ROUTE_NEIGHBOR_TABLE_H_
