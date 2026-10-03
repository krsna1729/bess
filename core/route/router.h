// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_ROUTER_H_
#define BESS_ROUTE_ROUTER_H_

#include <any>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "utils/logging.h"

#include "dataplane/batch_stages.h"
#include "dataplane/interface_id.h"
#include "dataplane/resource.h"
#include "dataplane/slot_table.h"
#include "packet.h"
#include "packet_mutation.h"
#include "route/next_hop_id.h"
#include "route/route_domain.h"
#include "route/route_table.h"
#include "utils/ether.h"

namespace bess::dataplane {
class TransactionEngine;
}  // namespace bess::dataplane

namespace bess::route {


enum class NeighborState : uint8_t {
  kResolved,     // L2 addresses known; forward with RewriteL2()
  kIncomplete,   // resolution pending (ARP/ND in flight)
  kUnreachable,  // resolution failed
};

// Where a route leads (K7): the egress interface, the neighbor state, and the
// L2 addresses to write when the neighbor is resolved. What to do with packets
// toward an unresolved neighbor (queue, punt, drop) is the application's.
//
// `egress` is a logical interface (`dataplane::InterfaceId`, zero = none), not
// a graph gate: the owner maps it to a gate, a port and queue, or a hardware
// action (M7, D-049). This library knows nothing of any of them.
struct NextHop {
  dataplane::InterfaceId egress = dataplane::kInvalidInterfaceId;
  NeighborState neighbor = NeighborState::kIncomplete;
  utils::Ethernet::Address dst_mac{};
  utils::Ethernet::Address src_mac{};
};

// Writes `hop`'s destination and source MAC into the packet's Ethernet
// header. Requires the header in the first segment and exclusive payload
// storage (use K4 EnsureWritable first for a shared packet).
inline std::expected<void, packet::MutationError> RewriteL2(
    PacketRef pkt, const NextHop &hop) noexcept {
  if (pkt.handle() == nullptr) {
    return std::unexpected(packet::MutationError::kNullPacket);
  }
  if (pkt.head_len() < sizeof(utils::Ethernet)) {
    return std::unexpected(packet::MutationError::kCrossesSegment);
  }
  if (packet::PayloadWriteabilityOf(pkt) !=
      packet::PayloadWriteability::kWritable) {
    return std::unexpected(packet::MutationError::kSharedStorage);
  }
  auto *eth = pkt.head_data<utils::Ethernet *>();
  eth->dst_addr = hop.dst_mac;
  eth->src_addr = hop.src_mac;
  return {};
}

// Route domains plus next hops (K7, K7.1, M6):
//
//   (domain, IPv4 dst) --LPM--> NextHopId --SlotTable--> NextHop
//
// One Router owns both halves: the per-domain FIBs and the next hops every
// domain's routes share. Many routes share a next hop, so a neighbor change
// (a new MAC, an unresolved neighbor) publishes one next-hop object (a
// SlotTable pointer store, O(1)) and never touches a FIB; a route change is
// one in-place rte_lpm update and never copies next hops. A next hop is named
// by routes in any domain, and the reference counts below span all of them.
//
// Domains. A Router is created with `max_domains` (default 1: just the
// default domain 0, with the cost and behaviour of a router that has no
// domains). A domain id is its index: domains are a dense array of
// `max_domains` slots published through a SlotTable (one acquire load, bounds
// check included), so a reader resolves `domain` to its FIB without a lock
// and without a search, an unknown, removed or out-of-range domain is a miss
// (never undefined behaviour), and CreateDomain()/RemoveDomain() are safe
// while workers resolve. Domain 0 exists from Create() and cannot be removed.
// Domains are structural, not transactional: they are created before
// Enroll(), and once enrolled they are frozen (kEnrolled). D-046 records why
// a dense SlotTable and not a map or a rebuilt vector.
//
// Update modes, stated plainly:
//
//  - SetRoute()/RemoveRoute(): ordinary live updates. One writer, applied in
//    place to the domain's rte_lpm, O(1) in the table size, no FIB rebuild.
//    Each call is atomic to readers; a *sequence* of calls is not -- readers
//    can see any prefix of it.
//  - ReplaceRouteSetAtomic(): strict replacement of one domain's whole route
//    set. The replacement FIB is built off to the side, checked against
//    every limit (next hops, rules, tbl8 groups) before it is visible, and
//    published with one pointer store; readers see the old set or the new
//    one. It costs a FIB build, not an in-place update, and on any failure
//    the old set stays visible and nothing changes.
//
// The two halves are published independently, and the ordering rules that
// keep a reader from ever resolving a route to a missing next hop are
// enforced here rather than left to callers:
//
//  - a route may only name a next hop that exists; the next hop is published
//    first and a release fence orders it before the route's entries;
//  - a reader resolves routes first and looks the next hop up after an
//    acquire fence, so a reader that sees a route sees its next hop;
//  - a next hop cannot be removed while any route names it, and its removal
//    is published only after a grace period, so no reader can still be
//    holding an id it looked up before the last route to it went away.
//    Removal does not wait for that grace period: it records a token and
//    returns. The entry stays published until the grace period completes;
//    later control calls (or ReclaimRetired()) then drop it. Until then the
//    id is *retiring*: routes cannot name it, and SetNextHop() refuses to
//    reuse it (kNextHopRetiring), because a reader still holding the id from
//    a removed route would otherwise reach the new next hop.
//
// Control methods are serialized internally and never block on readers; they
// must not be called from a worker. Readers are RcuDomain readers (workers
// are) and need no Module, metadata or gate: Resolve()/ResolveBatch() are the
// whole direct, specialized path.
//
// Transactions (G1.2b, D-023): Enroll() registers the next hops and the
// routes as two resources of a TransactionEngine, so that one transaction can
// change them together with other modules' tables (a rule, the action it
// names, the route that action forwards to). The routes resource holds every
// domain's routes under one resource; the key carries the domain (RouteKey).
// An enrolled router has one writer, the engine: the direct setters above
// refuse (kEnrolled), and the engine's reference ledger replaces the
// router's own counts.
class Router {
 public:
  using Config = LpmRouteTable::Config;

  // `config` sizes the default domain's FIB; CreateDomain() takes the others'.
  // `max_domains` (1..kMaxRouteDomains) bounds the domain ids: 0..max_domains-1.
  static std::expected<std::unique_ptr<Router>, RouteError> Create(
      std::string name, const Config &config, size_t max_next_hops,
      rcu::RcuDomain &domain, size_t max_domains = 1);

  ~Router();

  Router(const Router &) = delete;
  Router &operator=(const Router &) = delete;

  // -- control ----------------------------------------------------------------

  // Adds next hop `id`, or replaces it (a neighbor update).
  std::expected<void, RouteError> SetNextHop(NextHopId id, const NextHop &hop);
  std::expected<void, RouteError> RemoveNextHop(NextHopId id);

  // Drops retiring next hops whose grace period has completed, and returns
  // how many are still waiting. Every control method does this first.
  size_t ReclaimRetired();

  // -- domains ----------------------------------------------------------------

  // Adds domain `domain` (0 < domain < max_domains) with its own FIB sized by
  // `config`. kInvalidId beyond max_domains, kDomainExists, kEnrolled.
  std::expected<void, RouteError> CreateDomain(RouteDomainId domain,
                                               const Config &config);
  // Removes an empty domain other than the default; its FIB is freed after a
  // grace period and readers meanwhile miss or still use it, never a mix.
  // kUnknownDomain, kDomainInUse (default domain, or routes remain),
  // kEnrolled.
  std::expected<void, RouteError> RemoveDomain(RouteDomainId domain);

  bool HasDomain(RouteDomainId domain) const noexcept {
    return TableOf(domain) != nullptr;
  }
  size_t domain_count() const;
  size_t max_domains() const noexcept { return max_domains_; }

  // -- routes -----------------------------------------------------------------

  // Adds or re-points a route in `domain`; /0 is the domain's default route.
  // Ordinary live update (see above). kUnknownDomain for a domain that does
  // not exist; the overloads without a domain act on the default domain.
  std::expected<void, RouteError> SetRoute(RouteDomainId domain,
                                           Ipv4Prefix prefix, NextHopId hop);
  std::expected<void, RouteError> RemoveRoute(RouteDomainId domain,
                                              Ipv4Prefix prefix);
  std::expected<void, RouteError> SetRoute(Ipv4Prefix prefix, NextHopId hop) {
    return SetRoute(kDefaultRouteDomainId, prefix, hop);
  }
  std::expected<void, RouteError> RemoveRoute(Ipv4Prefix prefix) {
    return RemoveRoute(kDefaultRouteDomainId, prefix);
  }

  // Replaces every route of `domain` with `routes` (a prefix named twice takes
  // its last entry), published as one pointer store. All validation happens
  // before anything is visible: every next hop must exist (kInvalidId,
  // kUnknownNextHop) and the FIB built off to the side must hold the whole set
  // (kTableFull for rules or tbl8 groups). On any error the previous set
  // stays published and the next-hop reference counts are unchanged. An empty
  // set empties the domain. kEnrolled when written through transactions.
  std::expected<void, RouteError> ReplaceRouteSetAtomic(
      RouteDomainId domain, const RouteSet &routes);

  // Routes in all domains / in one domain (0 for an unknown domain).
  size_t route_count() const;
  size_t route_count(RouteDomainId domain) const;
  size_t next_hop_count() const;
  // Routes naming `id` (control-side reference count), across all domains.
  size_t RouteReferences(NextHopId id) const;

  // -- transactions (D-023) ---------------------------------------------------

  // Registers "<name>/next_hops" (key: EncodeKey(NextHopId), value: NextHop)
  // and "<name>/routes" (key: RouteKey(domain, prefix), value: NextHopId; each
  // route references its next hop) with `engine`, which must outlive the router
  // or its enrollment. Refused once any route exists or a removed next hop is
  // still retiring (the ledger must start from what it can see). Freezes the
  // set of domains. Destroying an enrolled router unregisters both (with
  // workers paused).
  std::expected<void, std::string> Enroll(dataplane::TransactionEngine &engine);
  bool enrolled() const noexcept { return engine_ != nullptr; }

  // Releases both resources for teardown, tolerating a referrer that is still
  // registered and live keys (the graph is being disconnected around the
  // router): the owning module calls this from DeInit(), because the order
  // modules are destroyed in must not decide whether a pipeline can be torn
  // down. The destructor's own path stays strict (D-023) for direct users.
  std::expected<void, std::string> Release();

  const std::string &next_hops_resource() const { return next_hops_name_; }
  const std::string &routes_resource() const { return routes_name_; }
  // The registered resources while enrolled (null otherwise), so the owning
  // module can attach the RPC's typed codecs to them (D-025).
  dataplane::Resource *next_hops_resource_object() const noexcept {
    return next_hops_res_.get();
  }
  dataplane::Resource *routes_resource_object() const noexcept {
    return routes_res_.get();
  }

  // The routes resource's key: bits 40..63 the domain, 8..39 the address, 0..7
  // the prefix length. The default domain's keys are the bytes a router
  // without domains always used. `domain` must be below kMaxRouteDomains.
  static dataplane::ResourceKey RouteKey(RouteDomainId domain,
                                         Ipv4Prefix prefix) {
    DCHECK_LT(domain.value(), kMaxRouteDomains);
    return dataplane::EncodeKey(uint64_t{domain.value()} << 40 |
                                uint64_t{prefix.addr()} << 8 | prefix.length());
  }
  static dataplane::ResourceKey RouteKey(Ipv4Prefix prefix) {
    return RouteKey(kDefaultRouteDomainId, prefix);
  }
  static dataplane::ResourceKey RouteKey(const route::RouteKey &key) {
    return RouteKey(key.domain, key.prefix);
  }

  // Operations for this router's resources.
  dataplane::Op SetNextHopOp(NextHopId id, const NextHop &hop) const {
    return dataplane::Op::Upsert(next_hops_name_, dataplane::EncodeKey(id),
                                 std::any(hop));
  }
  dataplane::Op RemoveNextHopOp(NextHopId id) const {
    return dataplane::Op::Erase(next_hops_name_, dataplane::EncodeKey(id));
  }
  dataplane::Op SetRouteOp(RouteDomainId domain, Ipv4Prefix prefix,
                           NextHopId hop) const {
    return dataplane::Op::Upsert(routes_name_, RouteKey(domain, prefix),
                                 std::any(hop));
  }
  dataplane::Op RemoveRouteOp(RouteDomainId domain, Ipv4Prefix prefix) const {
    return dataplane::Op::Erase(routes_name_, RouteKey(domain, prefix));
  }
  dataplane::Op SetRouteOp(Ipv4Prefix prefix, NextHopId hop) const {
    return SetRouteOp(kDefaultRouteDomainId, prefix, hop);
  }
  dataplane::Op RemoveRouteOp(Ipv4Prefix prefix) const {
    return RemoveRouteOp(kDefaultRouteDomainId, prefix);
  }

  // -- reader -----------------------------------------------------------------

  static constexpr size_t kMaxBatch = LpmRouteTable::kMaxBatch;

  // Resolves each destination (host order) in `domain` to its next hop.
  // Returns a mask with bit i set where `hops[i]` was written; other positions
  // untouched. A domain that does not exist resolves nothing (mask 0).
  uint64_t ResolveBatch(RouteDomainId domain, std::span<const uint32_t> dst,
                        std::span<const NextHop *> hops) const noexcept {
    const LpmRouteTable *routes = TableOf(domain);
    if (unlikely(routes == nullptr)) {
      return 0;
    }
    return ResolveBatchIn(*routes, dst, hops);
  }
  // The default domain.
  uint64_t ResolveBatch(std::span<const uint32_t> dst,
                        std::span<const NextHop *> hops) const noexcept {
    return ResolveBatchIn(*default_routes_, dst, hops);
  }

  // The next hop `dst` resolves to in `domain`, or nullptr for a miss or an
  // unknown domain. Valid until the calling worker's next quiescent state.
  const NextHop *Resolve(RouteDomainId domain, uint32_t dst) const noexcept {
    const LpmRouteTable *routes = TableOf(domain);
    if (unlikely(routes == nullptr)) {
      return nullptr;
    }
    return ResolveIn(*routes, dst);
  }
  const NextHop *Resolve(uint32_t dst) const noexcept {
    return ResolveIn(*default_routes_, dst);
  }

  // Only the route lookup: the next-hop id `dst` matches in `domain`, or
  // kInvalidNextHopId (a miss, an unknown domain, or a route a transaction is
  // still placing, D-023). For pipelines that carry ids and resolve them later
  // with LookupNextHop(s).
  NextHopId LookupRoute(RouteDomainId domain, uint32_t dst) const noexcept {
    const LpmRouteTable *routes = TableOf(domain);
    if (unlikely(routes == nullptr)) {
      return kInvalidNextHopId;
    }
    const uint32_t id = routes->Read().LookupOrMiss(dst);
    return id != LpmRouteTable::kNoDefault ? NextHopId(id) : kInvalidNextHopId;
  }

  // The next hop an id names, or nullptr for an invalid, out-of-range or
  // removed one. Valid until the calling worker's next quiescent state. This
  // is the lookup a session pipeline makes when the action it matched carries
  // a next-hop id instead of a destination address.
  const NextHop *LookupNextHop(NextHopId id) const noexcept {
    return next_hops_.Lookup(id);
  }

  // Resolves ids to their next hops: bit i of the result is set where
  // `hops[i]` was written; other positions are untouched. Resolve stage
  // first, so a batch's next-hop lines are in flight together (K4.6).
  uint64_t LookupNextHops(std::span<const NextHopId> ids,
                          std::span<const NextHop *> hops) const noexcept {
    promise(ids.size() == hops.size());
    uint64_t mask = 0;
    dataplane::RunStages(
        ids.size(),
        [&](size_t i) {
          hops[i] = next_hops_.Lookup(ids[i]);
          if (hops[i] != nullptr) {
            dataplane::Prefetch(hops[i]);
          }
        },
        [&](size_t i) {
          if (hops[i] != nullptr) {
            mask |= uint64_t{1} << i;
          }
        });
    return mask;
  }

 private:
  // A domain's FIB. Immutable once published, like every SlotTable object:
  // the FIB inside synchronizes itself (rte_lpm in-place updates, the RcuPtr
  // generation ReplaceAll publishes), so it is reached through a const Domain.
  class Domain {
   public:
    explicit Domain(std::unique_ptr<LpmRouteTable> table)
        : table_(std::move(table)) {}
    LpmRouteTable &table() const noexcept { return *table_; }

   private:
    std::unique_ptr<LpmRouteTable> table_;
  };

  // SlotTable ids are one-based; a domain's slot is its id + 1.
  struct DomainSlotTag;
  using DomainSlot = dataplane::StrongId<DomainSlotTag, uint32_t>;
  // 2^32-1 wraps to the invalid slot 0, so every out-of-range id misses.
  static DomainSlot SlotOf(RouteDomainId domain) noexcept {
    return DomainSlot(domain.value() + 1);
  }

  Router(std::string name, std::unique_ptr<LpmRouteTable> default_routes,
         size_t max_next_hops, size_t max_domains, rcu::RcuDomain &rcu);

  class RouteResource;

  // The domain's FIB, or nullptr for a domain that does not exist.
  LpmRouteTable *TableOf(RouteDomainId domain) const noexcept {
    const Domain *d = domains_.Lookup(SlotOf(domain));
    return d != nullptr ? &d->table() : nullptr;
  }

  // Control-side walk over the existing domains, in id order. The caller holds
  // mutex_, or the router is enrolled (domains frozen).
  template <typename Fn>
  void ForEachDomain(Fn &&fn) const {
    for (const RouteDomainId id : domain_ids_) {
      fn(id, *TableOf(id));
    }
  }
  size_t CountRoutes() const;

  bool ValidId(NextHopId id) const noexcept { return next_hops_.ValidId(id); }

  const NextHop *ResolveIn(const LpmRouteTable &routes,
                           uint32_t dst) const noexcept {
    const uint32_t id = routes.Read().LookupOrMiss(dst);
    std::atomic_thread_fence(std::memory_order_acquire);
    return id != LpmRouteTable::kNoDefault ? next_hops_.Lookup(NextHopId(id))
                                           : nullptr;
  }

  uint64_t ResolveBatchIn(const LpmRouteTable &routes,
                          std::span<const uint32_t> dst,
                          std::span<const NextHop *> hops) const noexcept {
    promise(dst.size() == hops.size() && dst.size() <= kMaxBatch);
    uint32_t ids[kMaxBatch];
    uint64_t mask = routes.Read().LookupBatch(dst, std::span(ids, dst.size()));
    std::atomic_thread_fence(std::memory_order_acquire);
    for (uint64_t m = mask; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(__builtin_ctzll(m));
      if (const NextHop *hop = next_hops_.Lookup(NextHopId(ids[i]))) {
        hops[i] = hop;
      } else {
        // Id 0: a route a transaction is placing where there is no covering
        // route (D-023) -- a miss, as before it was placed.
        mask &= ~(uint64_t{1} << i);
      }
    }
    return mask;
  }

  // ReclaimRetired() with mutex_ held.
  size_t CompleteRetirementsLocked();

  const std::string name_;
  rcu::RcuDomain &rcu_;
  const size_t max_domains_;
  // Slot = domain id + 1. Read lock-free by workers and the engine; written
  // under mutex_ (and never once enrolled).
  dataplane::SlotTable<DomainSlot, Domain> domains_;
  // Domain 0's FIB: it is never removed, so the default-domain readers skip
  // the slot lookup.
  LpmRouteTable *const default_routes_;
  std::vector<RouteDomainId> domain_ids_;  // existing domains, ascending
  // One immutable NextHop per id, changed one at a time (mode C): a neighbor
  // update publishes one object, O(1), instead of rebuilding every next hop.
  dataplane::SlotTable<NextHopId, NextHop> next_hops_;

  mutable std::mutex mutex_;
  std::vector<uint32_t> references_;  // routes per next hop, index = id
  struct Retiring {
    NextHopId id;
    rcu::GracePeriod token;
  };
  std::vector<Retiring> retiring_;  // removed, still published

  // Set by Enroll(); the resources exist while enrolled.
  const std::string next_hops_name_;
  const std::string routes_name_;
  dataplane::TransactionEngine *engine_ = nullptr;
  std::unique_ptr<dataplane::Resource> next_hops_res_;
  std::unique_ptr<dataplane::Resource> routes_res_;
};

}  // namespace bess::route

#endif  // BESS_ROUTE_ROUTER_H_
