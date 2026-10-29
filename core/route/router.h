// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_ROUTER_H_
#define BESS_ROUTE_ROUTER_H_

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/logging.h"

#include "dataplane/batch_stages.h"
#include "dataplane/interface_id.h"
#include "dataplane/member_select.h"
#include "dataplane/resource.h"
#include "dataplane/slot_table.h"
#include "packet.h"
#include "packet_mutation.h"
#include "route/neighbor_key.h"
#include "route/next_hop_id.h"
#include "route/route_domain.h"
#include "route/route_table.h"
#include "utils/ether.h"

namespace bess::dataplane {
class ResourceRegistry;
}  // namespace bess::dataplane

namespace bess::route {


enum class NeighborState : uint8_t {
  kResolved,     // L2 addresses known; forward with RewriteL2()
  kIncomplete,   // resolution pending (ARP/ND in flight)
  kUnreachable,  // resolution failed
};

// What a route's hop does with a packet that reaches it (D-178).
enum class NextHopKind : uint8_t {
  kForward,  // forward out `egress`, subject to `neighbor`
  kLocal,    // deliver to this node: no egress, no neighbor, no L2 rewrite
};

// Where a route leads (K7): the egress interface, the neighbor state, and the
// L2 addresses to write when the neighbor is resolved. What to do with packets
// toward an unresolved neighbor (queue, punt, drop) is the application's.
//
// `kind` says whether the hop forwards or delivers locally. A local hop
// (`LocalNextHop()`) is the node's own address: the chain reports
// `kLocalDelivery` for a frame that reaches it and the caller decides where a
// local packet goes (a punt/local gate, a stack, a socket -- BESS has no
// kernel path here). A local hop names no egress and no addresses, and its
// neighbor state is irrelevant.
//
// A forwarding hop with no egress interface (`kInvalidInterfaceId`) forwards
// nowhere: the chain reports `kNoEgress` for a frame that reaches it, and the
// caller counts that as its own outcome. It is the shape of a blackhole route
// -- a default route that drops, a prefix this node refuses to forward (D-160,
// `BlackholeNextHop()`). Such a hop names no addresses and its neighbor state
// is irrelevant: a route to it is meant to end the packet.
//
// `egress` is a logical interface (`dataplane::InterfaceId`, zero = none), not
// a graph gate: the owner maps it to a gate, a port and queue, or a hardware
// action (M7, D-049). This library knows nothing of any of them.
struct NextHop {
  dataplane::InterfaceId egress = dataplane::kInvalidInterfaceId;
  NeighborState neighbor = NeighborState::kIncomplete;
  // Uses the byte the interface/neighbor pair left as padding: the hop stays
  // 16 bytes (D-060).
  NextHopKind kind = NextHopKind::kForward;
  utils::Ethernet::Address dst_mac{};
  utils::Ethernet::Address src_mac{};
};
// A neighbor's update to the next hop bound to it ("<router>/neighbors"): the
// state and addresses a resolver learned. Accepted only for a bound id, by
// the neighbors resource; the hop keeps its binding's interface as egress.
struct NeighborManagedNextHop {
  NextHop value;
};

// One change to the neighbor bindings, in the order transactions published
// them: `neighbor` is the key `id` is now bound to, or empty for an unbind.
// The owner reads them (TakeNeighborBindingChanges) to start or stop
// resolving; the next hop itself was already published, incomplete.
struct NeighborBindingChange {
  NextHopId id;
  std::optional<NeighborKey> neighbor;
};

// What the owner of neighbor resolution accepts as bindings: how many next
// hops may be bound at once, and a check of each neighbor key (its interface
// exists, the router has an address of its family there, it is not one of
// the router's own) before a transaction may bind it.
struct NeighborBindingPolicy {
  size_t capacity = 0;
  std::function<std::expected<void, std::string>(const NeighborKey &)> validate;
};

// Every resolved packet loads one; at 16 bytes four fill a cache line and none
// straddles two. A 32-bit egress made it 20 bytes and cost a hot 1K-route
// lookup 14% (D-060).
static_assert(sizeof(NextHop) == 16, "a next hop is 16 bytes");

// A next hop that forwards nowhere: no egress interface (D-160). A route to it
// blackholes a prefix -- `0.0.0.0/0` to this is the domain's default that
// drops. What the chain reports for it (`kNoEgress`) is a caller's outcome to
// count, and it is not a miss: the route exists.
inline NextHop BlackholeNextHop() noexcept {
  NextHop hop;
  hop.egress = dataplane::kInvalidInterfaceId;
  hop.neighbor = NeighborState::kResolved;
  return hop;
}

// A next hop that delivers to this node (D-178): the route is a local one, the
// node's own address, and the chain reports `kLocalDelivery` rather than
// forwarding. There is no egress, no neighbor to resolve and no address to
// write; where the packet then goes is the caller's policy (a local/punt gate).
inline NextHop LocalNextHop() noexcept {
  NextHop hop;
  hop.egress = dataplane::kInvalidInterfaceId;
  hop.neighbor = NeighborState::kResolved;
  hop.kind = NextHopKind::kLocal;
  return hop;
}

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

// An ECMP group (M15, D-065) with active and standby members (D-184):
// 1..kMaxMembers next hops in all, immutable once published. A flow picks a
// member by its hash, so one flow keeps one path while the group and its
// members' availability are unchanged; how the hash is made (which fields,
// which algorithm) is the application's (dataplane/member_select.h, D-066).
//
// The configuration is `members`: `size` active members, then `standby_size`
// standby members, each list in selection order, a repeated member a weight. A
// next hop is in at most one of the two lists, and there is at least one
// active member.
//
// What Select() reads is the eligible set, which the Router computes from the
// configuration and its members' availability (Router::SetNextHopDown,
// "<name>/next_hop_state") before it publishes the group:
//
//  - the available active members, if any;
//  - else the available standby members, if any;
//  - else every active member: fail open, so the neighbor state decides, as it
//    does with no availability input at all.
//
// The chosen list's n positions get kMaxMembers / n consecutive slots each,
// and an unavailable position's slots are dealt round-robin to the available
// positions' members. So with every active member available a hash picks the
// member it picked before availability existed, and a member that goes down
// moves only its own flows, spread over the others.
struct NextHopGroup {
  static constexpr size_t kMaxMembers = 64;
  // Read by Select(), and first, so the packet path reads the same leading
  // bytes as when a group held only its members.
  uint32_t eligible_size = 0;
  std::array<NextHopId, kMaxMembers> eligible{};
  // The configuration, read on the control side.
  uint32_t size = 0;          // active members, members[0, size)
  uint32_t standby_size = 0;  // standby members, members[size, size + standby_size)
  std::array<NextHopId, kMaxMembers> members{};

  std::span<const NextHopId> actives() const noexcept {
    return std::span(members).first(size);
  }
  std::span<const NextHopId> standbys() const noexcept {
    return std::span(members).subspan(size, standby_size);
  }

  // Uniform over the eligible slots for a uniform 32-bit hash, no division.
  NextHopId Select(uint32_t flow_hash) const noexcept {
    return eligible[dataplane::RangeSelect(flow_hash, eligible_size)];
  }
};

// A next hop's availability (D-184), the value of "<name>/next_hop_state": a
// next hop with no entry is available. It is an input: a producer outside the
// Router (BFD, ICMP or application probes, a controller) decides it.
struct NextHopState {
  bool down = false;
};

// Route domains plus next hops (K7, K7.1, M6), dual-stack:
//
//   (domain, IPv4 dst) --rte_lpm/rte_fib--> NextHopId --SlotTable--> NextHop
//   (domain, IPv6 dst) --rte_fib6-------/
//
// One Router owns both halves: the per-domain FIBs and the next hops every
// domain's routes share. Many routes share a next hop, so a neighbor change
// (a new MAC, an unresolved neighbor) publishes one next-hop object (a
// SlotTable pointer store, O(1)) and never touches a FIB; a route change is
// one FIB update and never copies next hops. A next hop is named by routes of
// either family in any domain, and the reference counts below span all of
// them. Each family is optional and sized on its own (Config): a router
// without one has no FIB of it in any domain.
//
// An IPv4 route change is applied in place to the domain's rte_lpm/rte_fib;
// an IPv6 one is applied to the domain's Ipv6RouteTable spare and published
// by a pointer exchange (route_table.h), so even a sequence of IPv6 changes
// in one transaction reaches readers at once.
//
// Domains. A Router is created with `max_domains` (default 1: just the
// default domain 0, with the cost and behaviour of a router that has no
// domains). A domain id is its index: domains are a dense array of
// `max_domains` slots published through a SlotTable (one acquire load, bounds
// check included), so a reader resolves `domain` to its FIB without a lock
// and without a search, an unknown, removed or out-of-range domain is a miss
// (never undefined behaviour), and CreateDomain()/RemoveDomain() are safe
// while workers resolve. Domain 0 exists from Create() and cannot be removed.
// Domains are structural, not transactional, in the sense that a domain's FIB
// is its own table, not a route in another one: the first route a transaction
// writes into a domain creates it (D-158, CreateDomainLocked), and
// `max_domains` bounds the ids instead of allocating them, so a router
// configured for many network instances pays only for the ones that hold
// routes. D-046 records why a dense SlotTable and not a map or a rebuilt
// vector.
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
// Next-hop groups (M15, D-065): a route names a next hop or a group of
// them, and the reader picks a group member by the flow hash it passes
// (Resolve without a hash uses 0: the group's first eligible member).
// A group is one immutable SlotTable object: changing its members publishes a
// new object (O(1), no FIB change), changing a member's neighbor is the
// member's next-hop update, and a group in use (routes name it) cannot be
// removed. In the FIB a group is the value max_next_hops + group id, so a
// router without groups pays one never-taken branch. Groups are transactional
// like next hops (user decision on D-065): enrolled, Enroll registers
// "<name>/groups" beside next hops and routes -- always, not only with
// max_groups: a class must register every table it declares (D-156), so
// without max_groups the resource exists and holds nothing, and every group
// id is invalid. A route may name a group, a group references its members,
// and the direct group setters refuse (kEnrolled).
//
// Path health (D-184): availability is an input per next hop, not per group
// -- a path is down once, and every group holding it follows. A group has
// active and standby members and selects from its eligible set (see
// NextHopGroup), which is part of the immutable group object: a change of a
// next hop's availability publishes a new object for every group holding it
// (one SlotTable store each, the old object retired), so a reader sees the
// old group or the new one, and the packet path reads nothing it did not read
// before. This holds for every lookup that picks a group member, IPv4 and
// IPv6. The Router keeps the reverse index from a next hop to the groups
// holding it for this. Enrolled, "<name>/next_hop_state" is the input; direct
// users call SetNextHopDown. Availability applies to static and bound next
// hops alike.
//
// Transactions (G1.2b, D-023): Enroll() registers the next hops, the neighbor
// bindings, the next-hop state, the groups and the routes as five resources
// of a TransactionEngine, so that one transaction can change them together
// with other modules' tables (a rule, the action it names, the route that
// action forwards to). The routes resource holds every domain's routes under
// one resource; the key carries the domain (RouteKey). An enrolled router has
// one writer, the engine: the direct setters above refuse (kEnrolled), and the
// engine's reference ledger replaces the router's own counts.
//
// Neighbor bindings (D-183): "<name>/neighbors" (key EncodeKey(NextHopId),
// value NeighborKey) binds a next-hop id to a neighbor that a resolver owns.
// Binding publishes the id's next hop -- egress the neighbor's interface,
// incomplete -- in the same slot table the static next hops use, so routes
// and groups name it like any next hop; its later states arrive as
// NeighborManagedNextHop upserts on the same key. An id is either static
// ("<name>/next_hops") or bound, never both: each resource refuses the
// other's ids. A route, group or state entry that names a bound id references
// "<name>/neighbors", so the engine refuses to unbind it while it is named.
// Which resource a new value references is decided from the bindings as the
// transaction has them: the engine reserves the referenced resources (next
// hops, neighbors) before the routes and groups that name them, so a route
// listed before the binding it names in one request still finds it. Another
// module's value that declares "<name>/next_hops" (a classifier result field)
// may name either kind: "<name>/neighbors" is the key holder of
// "<name>/next_hops" (Resource::DeclareKeyHolder, D-187), so a bound id's
// references are counted and checked there, the same as a route's. Without a
// NeighborBindingPolicy the resource exists and binds nothing.
class Router {
 public:
  // The address families a router serves and how each domain's table of each
  // is sized: a family is enabled iff its config is set, and every domain
  // (the default one, CreateDomain's, and those a transaction creates) has a
  // table for exactly the enabled families. A disabled family has no FIB
  // anywhere: its route writes are refused (kFamilyDisabled) and its lookups
  // miss. `ipv6->max_value` is the router's own (max_next_hops + max_groups,
  // the largest route value), whatever the caller sets, so the IPv6 entry
  // size follows from the router's capacity. A Config with neither family is
  // a next-hop-only router: no FIB in any domain, every route write refused,
  // next hops (static and bound) resolved by id as usual.
  struct Config {
    std::optional<LpmRouteTable::Config> ipv4;
    std::optional<Ipv6RouteTable::Config> ipv6;
  };

  // The most next hops (plus groups) a router with no FIB holds: an id is a
  // one-based 32-bit value, and the bound keeps max + 1 within 32 bits.
  static constexpr size_t kMaxNextHopsWithoutFib = 0xfffffffeu;

  // `config` sizes the default domain's FIBs and those created on demand;
  // CreateDomain() takes the others'.
  // `max_domains` (1..kMaxRouteDomains) bounds the domain ids: 0..max_domains-1.
  // `max_groups` bounds the next-hop group ids: 1..max_groups (0: none);
  // with a family, max_next_hops + max_groups must fit a route value (24
  // bits); with neither, kMaxNextHopsWithoutFib.
  static std::expected<std::unique_ptr<Router>, RouteError> Create(
      std::string name, const Config &config, size_t max_next_hops,
      rcu::RcuDomain &domain, size_t max_domains = 1, size_t max_groups = 0);

  // What Create allocates (D-198): the default domain's FIBs (DPDK, the
  // route tables' InstanceBytes), the three id tables' slots and states
  // (DPDK, SlotTable::Bytes) with the domain slab's first chunk (the default
  // domain's object), and the per-id control vectors and the router itself
  // (glibc, GlibcBytes) -- what bytes() counts after Create. Same arguments
  // as Create.
  static size_t Bytes(const std::string &name, const Config &config,
                      size_t max_next_hops, const rcu::RcuDomain &domain,
                      size_t max_domains = 1, size_t max_groups = 0);
  static size_t GlibcBytes(const std::string &name, const Config &config,
                           size_t max_next_hops, const rcu::RcuDomain &domain,
                           size_t max_domains = 1, size_t max_groups = 0);
  // What the router holds now (any thread): every domain's FIBs (an IPv6
  // table's spare included), the id tables with their slab chunks, and the
  // control vectors. The sorted domain-id list and the transaction scratch
  // are not counted.
  size_t bytes() const noexcept;
  // The admission everything the router grows by asks first (D-198): a
  // domain's FIB set before CreateDomain (or a transaction writing into a
  // new domain) builds it, a FIB's replacement or IPv6 spare, and each new
  // slab chunk of the domain, next-hop and group tables. A refusal throws
  // utils::MemoryRefused and changes nothing. A removed domain's FIBs are
  // given back when they are retired to the RCU domain. Control path only.
  void set_memory_growth(utils::MemoryGrowth *growth) noexcept;

  bool has_ipv4() const noexcept { return domain_config_.ipv4.has_value(); }
  bool has_ipv6() const noexcept { return domain_config_.ipv6.has_value(); }

  // The FIBs that exist across all domains, per family (an IPv6 table counts
  // once, however many rte_fib6 instances it holds).
  struct FibCounts {
    size_t ipv4 = 0;
    size_t ipv6 = 0;
  };
  FibCounts fib_counts() const;

  ~Router();

  Router(const Router &) = delete;
  Router &operator=(const Router &) = delete;

  // -- control ----------------------------------------------------------------

  // Adds next hop `id`, or replaces it (a neighbor update).
  std::expected<void, RouteError> SetNextHop(NextHopId id, const NextHop &hop);
  // Neighbor bindings (see the class comment). Set before Enroll(): the
  // policy every binding must pass; without it no binding is accepted.
  std::expected<void, RouteError> SetNeighborBindingPolicy(
      NeighborBindingPolicy policy);
  // Whether `id` is bound to a neighbor (control side, committed state).
  bool IsNeighborBound(NextHopId id) const;
  // Every binding, in id order (control side, committed state).
  void ForEachNeighborBinding(
      const std::function<void(NextHopId, const NeighborKey &)> &fn) const;
  // The binding changes published since the last call, oldest first.
  std::vector<NeighborBindingChange> TakeNeighborBindingChanges();
  std::expected<void, RouteError> RemoveNextHop(NextHopId id);

  // Drops retiring next hops and groups whose grace period has completed, and
  // returns how many are still waiting. Every control method does this first.
  size_t ReclaimRetired();

  // -- next-hop groups (M15, D-065) ------------------------------------------------

  // Adds group `id` or replaces its configuration: `members` are the active
  // members and `standby` the standby ones (NextHopGroup), 1..kMaxMembers
  // existing next hops in all, at least one active; a next hop may appear more
  // than once in its list, as a weight, and in only one of the two lists.
  // kInvalidId (id, member counts, a next hop in both lists), kUnknownNextHop,
  // kNextHopRetiring (the id was removed and readers may still hold it),
  // kEnrolled.
  std::expected<void, RouteError> SetNextHopGroup(NextHopGroupId id,
                                                  std::span<const NextHopId> members,
                                                  std::span<const NextHopId> standby = {});
  // Removes a group no route names (kNextHopInUse otherwise). Like a next
  // hop, it stays readable until a grace period passes; its members stay
  // referenced until then.
  std::expected<void, RouteError> RemoveNextHopGroup(NextHopGroupId id);
  // Points a route at a group (the next-hop overload's semantics otherwise).
  std::expected<void, RouteError> SetRoute(RouteDomainId domain,
                                           Ipv4Prefix prefix,
                                           NextHopGroupId group);
  std::expected<void, RouteError> SetRoute(RouteDomainId domain,
                                           Ipv6Prefix prefix,
                                           NextHopGroupId group);
  size_t next_hop_group_count() const;
  // Routes naming group `id`, across all domains. Takes the engine's lock
  // after releasing this one, never nested -- the two are taken in both orders
  // elsewhere (tools/sanitizers/tsan.supp, D-158). `engine_` is replaced only
  // by Enroll/Release, which the module lifecycle serialises with this call.
  size_t GroupReferences(NextHopGroupId id) const;
  // Groups naming next hop `id` (counted once per member position).
  size_t GroupMemberships(NextHopId id) const;

  // -- path health (D-184) ----------------------------------------------------

  // Marks next hop `id` unavailable (`down`) or available again, and
  // republishes every group it is a member of with its new eligible set; all
  // of them are built before the first is published. A next hop marked down
  // cannot be removed (kNextHopInUse) until it is marked available, as the
  // resource's entry pins it enrolled. kInvalidId, kUnknownNextHop, kEnrolled.
  std::expected<void, RouteError> SetNextHopDown(NextHopId id, bool down);
  // Every next hop with a state entry, in id order: enrolled, the entries of
  // "<name>/next_hop_state" (an entry may say available); direct, the next
  // hops marked down. Same locking rule as ForEachNextHop.
  void ForEachNextHopState(
      const std::function<void(NextHopId, const NextHopState &)> &fn) const;

  // -- domains ----------------------------------------------------------------

  // A domain's FIB is created when it is first needed: explicitly here, or by
  // the first route a transaction writes into it (D-158), so `max_domains` is a
  // bound on the ids rather than an allocation -- a router configured for many
  // network instances pays for the ones that hold routes. A domain created on
  // demand is sized like the default one.
  //
  // Adds domain `domain` (0 < domain < max_domains) with its own FIBs sized by
  // `config`, which must enable exactly the router's families
  // (kFamilyDisabled otherwise). kInvalidId beyond max_domains,
  // kDomainExists, kEnrolled.
  std::expected<void, RouteError> CreateDomain(RouteDomainId domain,
                                               const Config &config);
  // Removes an empty domain other than the default; its FIB is freed after a
  // grace period and readers meanwhile miss or still use it, never a mix.
  // kUnknownDomain, kDomainInUse (default domain, or routes remain),
  // kEnrolled.
  std::expected<void, RouteError> RemoveDomain(RouteDomainId domain);

  bool HasDomain(RouteDomainId domain) const noexcept {
    return domains_.Lookup(SlotOf(domain)) != nullptr;
  }
  size_t domain_count() const;
  size_t max_domains() const noexcept { return max_domains_; }

  // -- routes -----------------------------------------------------------------

  // Adds or re-points a route in `domain`; /0 is the domain's default route.
  // Ordinary live update (see above). kFamilyDisabled for a family the router
  // does not serve, kUnknownDomain for a domain that does not exist; the
  // overloads without a domain act on the default domain.
  std::expected<void, RouteError> SetRoute(RouteDomainId domain,
                                           Ipv4Prefix prefix, NextHopId hop);
  std::expected<void, RouteError> RemoveRoute(RouteDomainId domain,
                                              Ipv4Prefix prefix);
  std::expected<void, RouteError> SetRoute(RouteDomainId domain,
                                           Ipv6Prefix prefix, NextHopId hop);
  std::expected<void, RouteError> RemoveRoute(RouteDomainId domain,
                                              Ipv6Prefix prefix);
  std::expected<void, RouteError> SetRoute(Ipv4Prefix prefix, NextHopId hop) {
    return SetRoute(kDefaultRouteDomainId, prefix, hop);
  }
  std::expected<void, RouteError> RemoveRoute(Ipv4Prefix prefix) {
    return RemoveRoute(kDefaultRouteDomainId, prefix);
  }
  std::expected<void, RouteError> SetRoute(Ipv6Prefix prefix, NextHopId hop) {
    return SetRoute(kDefaultRouteDomainId, prefix, hop);
  }
  std::expected<void, RouteError> RemoveRoute(Ipv6Prefix prefix) {
    return RemoveRoute(kDefaultRouteDomainId, prefix);
  }

  // Replaces every IPv4 route of `domain` with `routes` (a prefix named twice
  // takes its last entry), published as one pointer store. All validation
  // happens before anything is visible: every next hop must exist
  // (kInvalidId, kUnknownNextHop) and the FIB built off to the side must hold
  // the whole set (kTableFull for rules or tbl8 groups). On any error the
  // previous set stays published and the next-hop reference counts are
  // unchanged. An empty set empties the domain. kEnrolled when written
  // through transactions; kFamilyDisabled without IPv4.
  std::expected<void, RouteError> ReplaceRouteSetAtomic(
      RouteDomainId domain, const RouteSet &routes);

  // Routes in all domains / in one domain (0 for an unknown domain).
  size_t route_count() const;
  size_t route_count(RouteDomainId domain) const;
  size_t next_hop_count() const;
  // Routes naming `id` (control-side reference count), across all domains.
  // Enrolled, routes only: a group holding the hop is GroupMemberships, and a
  // non-route resource naming it lives in the engine's ledger (D-065, D-023).
  // Holds the router's mutex while walking the route resource, and takes no
  // engine lock -- but a walk must not see a transaction mid-apply, so a
  // caller outside the engine's own paths holds the control plane's mutex, as
  // for the ForEach* walks.
  size_t RouteReferences(NextHopId id) const;

  // Control-side walks over the committed state, for reading it back (D-129):
  // the caller holds the control-plane lock while enrolled (no transaction is
  // in flight). Live next hops only -- a removed one is absent at once, though
  // readers may still see it until its grace period ends -- in id order.
  void ForEachNextHop(
      const std::function<void(NextHopId, const NextHop &)> &fn) const;
  // Every route of every domain, in domain order: `hop` names its next hop,
  // or `group` its next-hop group (the other is 0). `fn` must not call the
  // router (both walks hold its lock).
  void ForEachRoute(
      const std::function<void(const ::bess::route::RouteKey &key,
                               NextHopId hop, NextHopGroupId group)> &fn) const;
  // Every live group, in id order, under the same rule as ForEachNextHop: a
  // removed one is absent at once, though readers may still see it until its
  // grace period ends.
  void ForEachNextHopGroup(
      const std::function<void(NextHopGroupId, const NextHopGroup &)> &fn) const;
  void ForEachIpv6Route(
      const std::function<void(RouteDomainId, Ipv6Prefix, NextHopId,
                               NextHopGroupId)> &fn) const;

  // -- transactions (D-023) ---------------------------------------------------

  // Registers "<name>/next_hops" (key: EncodeKey(NextHopId), value: NextHop),
  // "<name>/neighbors" (key: EncodeKey(NextHopId), value: NeighborKey or
  // NeighborManagedNextHop; see the class comment), "<name>/next_hop_state"
  // (key: EncodeKey(NextHopId), value: NextHopState; each entry references its
  // next hop, D-184), "<name>/groups" (key: EncodeKey(NextHopGroupId), value:
  // NextHopGroup, the configuration only; each group references its distinct
  // members), and "<name>/routes" (key: RouteKey(domain, prefix), value:
  // NextHopId or NextHopGroupId; each route references what it names) with
  // `engine`, which must outlive the router or its enrollment. Refused once any
  // route, group or next-hop state exists or a removed next hop or group is
  // still retiring (the ledger must start from what it can see). Freezes the
  // set of domains. Destroying an enrolled router unregisters them (with
  // workers paused).
  std::expected<void, std::string> Enroll(dataplane::ResourceRegistry &engine);
  bool enrolled() const noexcept { return engine_ != nullptr; }

  // Releases the resources for teardown, tolerating a referrer that is still
  // registered and live keys (the graph is being disconnected around the
  // router): the owning module calls this from DeInit(), because the order
  // modules are destroyed in must not decide whether a pipeline can be torn
  // down. The destructor's own path stays strict (D-023) for direct users.
  std::expected<void, std::string> Release();

  const std::string &next_hops_resource() const { return next_hops_name_; }
  const std::string &neighbors_resource() const { return neighbors_name_; }
  const std::string &next_hop_state_resource() const { return next_hop_state_name_; }
  const std::string &routes_resource() const { return routes_name_; }
  const std::string &groups_resource() const { return groups_name_; }
  // The registered resources while enrolled (null otherwise), so the owning
  // module can attach the RPC's typed codecs to them (D-025).
  dataplane::Resource *next_hops_resource_object() const noexcept {
    return next_hops_res_.get();
  }
  dataplane::Resource *neighbors_resource_object() const noexcept {
    return neighbors_res_.get();
  }
  dataplane::Resource *routes_resource_object() const noexcept {
    return routes_res_.get();
  }
  dataplane::Resource *groups_resource_object() const noexcept {
    return groups_res_.get();
  }
  dataplane::Resource *next_hop_state_resource_object() const noexcept {
    return state_res_.get();
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
  static dataplane::ResourceKey RouteKey(RouteDomainId domain,
                                         Ipv6Prefix prefix) {
    DCHECK_LT(domain.value(), kMaxRouteDomains);
    dataplane::ResourceKey key(1, '\6');
    const uint32_t domain_id = domain.value();
    key.append(reinterpret_cast<const char *>(&domain_id), sizeof(domain_id));
    key.append(reinterpret_cast<const char *>(prefix.addr().bytes), 16);
    key.push_back(static_cast<char>(prefix.length()));
    return key;
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
  // Binds next hop `id` to `neighbor`, or unbinds it ("<name>/neighbors").
  dataplane::Op BindNeighborOp(NextHopId id, const NeighborKey &neighbor) const {
    return dataplane::Op::Upsert(neighbors_name_, dataplane::EncodeKey(id),
                                 std::any(neighbor));
  }
  dataplane::Op UnbindNeighborOp(NextHopId id) const {
    return dataplane::Op::Erase(neighbors_name_, dataplane::EncodeKey(id));
  }
  // A resolver's update to the next hop bound at `id`: its state and
  // addresses (the egress stays the binding's interface).
  dataplane::Op SetNeighborManagedNextHopOp(NextHopId id,
                                            const NextHop &hop) const {
    return dataplane::Op::Upsert(
        neighbors_name_, dataplane::EncodeKey(id),
        std::any(NeighborManagedNextHop{hop}));
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
  dataplane::Op SetRouteOp(RouteDomainId domain, Ipv6Prefix prefix,
                           NextHopId hop) const {
    return dataplane::Op::Upsert(routes_name_, RouteKey(domain, prefix),
                                 std::any(hop));
  }
  dataplane::Op RemoveRouteOp(RouteDomainId domain, Ipv6Prefix prefix) const {
    return dataplane::Op::Erase(routes_name_, RouteKey(domain, prefix));
  }
  dataplane::Op SetRouteOp(Ipv4Prefix prefix, NextHopId hop) const {
    return SetRouteOp(kDefaultRouteDomainId, prefix, hop);
  }
  // A route to a group.
  dataplane::Op SetRouteOp(RouteDomainId domain, Ipv4Prefix prefix,
                           NextHopGroupId group) const {
    return dataplane::Op::Upsert(routes_name_, RouteKey(domain, prefix),
                                 std::any(group));
  }
  dataplane::Op SetRouteOp(RouteDomainId domain, Ipv6Prefix prefix,
                           NextHopGroupId group) const {
    return dataplane::Op::Upsert(routes_name_, RouteKey(domain, prefix),
                                 std::any(group));
  }
  // Adds group `id` or replaces its configuration, under SetNextHopGroup's
  // rules, checked at Reserve (lists too long for a group are refused there).
  dataplane::Op SetNextHopGroupOp(NextHopGroupId id, std::span<const NextHopId> members,
                                  std::span<const NextHopId> standby = {}) const {
    NextHopGroup group;
    if (members.size() + standby.size() > NextHopGroup::kMaxMembers) {
      group.size = NextHopGroup::kMaxMembers + 1;  // refused at Reserve
    } else {
      group.size = static_cast<uint32_t>(members.size());
      group.standby_size = static_cast<uint32_t>(standby.size());
      const auto standbys = std::ranges::copy(members, group.members.begin()).out;
      std::ranges::copy(standby, standbys);
    }
    return dataplane::Op::Upsert(groups_name_, dataplane::EncodeKey(id),
                                 std::any(group));
  }
  dataplane::Op RemoveNextHopGroupOp(NextHopGroupId id) const {
    return dataplane::Op::Erase(groups_name_, dataplane::EncodeKey(id));
  }
  // Sets next hop `id`'s availability entry (D-184), or removes it (the next
  // hop is then available).
  dataplane::Op SetNextHopStateOp(NextHopId id, NextHopState state) const {
    return dataplane::Op::Upsert(next_hop_state_name_, dataplane::EncodeKey(id),
                                 std::any(state));
  }
  dataplane::Op RemoveNextHopStateOp(NextHopId id) const {
    return dataplane::Op::Erase(next_hop_state_name_, dataplane::EncodeKey(id));
  }
  dataplane::Op RemoveRouteOp(Ipv4Prefix prefix) const {
    return RemoveRouteOp(kDefaultRouteDomainId, prefix);
  }

  // -- reader -----------------------------------------------------------------

  static constexpr size_t kMaxBatch = LpmRouteTable::kMaxBatch;
  static_assert(Ipv6RouteTable::kMaxBatch == kMaxBatch);

  // Resolves each destination (host order) in `domain` to its next hop.
  // Returns a mask with bit i set where `hops[i]` was written; other positions
  // untouched. A domain that does not exist, or a router without IPv4,
  // resolves nothing (mask 0).
  uint64_t ResolveBatch(RouteDomainId domain, std::span<const uint32_t> dst,
                        std::span<const NextHop *> hops) const noexcept {
    const LpmRouteTable *routes = TableOf(domain);
    if (unlikely(routes == nullptr)) {
      return 0;
    }
    return ResolveBatchIn<false>(*routes, dst, {}, hops);
  }
  // The default domain.
  uint64_t ResolveBatch(std::span<const uint32_t> dst,
                        std::span<const NextHop *> hops) const noexcept {
    if (unlikely(default_routes_ == nullptr)) {
      return 0;
    }
    return ResolveBatchIn<false>(*default_routes_, dst, {}, hops);
  }
  // With each packet's flow hash, which picks the member of a group route.
  uint64_t ResolveBatch(RouteDomainId domain, std::span<const uint32_t> dst,
                        std::span<const uint32_t> flow_hashes,
                        std::span<const NextHop *> hops) const noexcept {
    promise(flow_hashes.size() == dst.size());
    const LpmRouteTable *routes = TableOf(domain);
    if (unlikely(routes == nullptr)) {
      return 0;
    }
    return ResolveBatchIn<true>(*routes, dst, flow_hashes, hops);
  }
  // The IPv6 batch: one rte_fib6_lookup_bulk for the batch, then as above.
  // A domain that does not exist, or a router without IPv6, resolves nothing.
  uint64_t ResolveBatch6(RouteDomainId domain,
                         std::span<const utils::Ipv6Address> dst,
                         std::span<const uint32_t> flow_hashes,
                         std::span<const NextHop *> hops) const noexcept {
    promise(flow_hashes.size() == dst.size() && dst.size() == hops.size() &&
            dst.size() <= kMaxBatch);
    const Ipv6RouteTable *routes = Table6Of(domain);
    if (unlikely(routes == nullptr)) {
      return 0;
    }
    uint32_t ids[kMaxBatch];
    const uint64_t mask =
        routes->Read().LookupBatch(dst, std::span(ids, dst.size()));
    std::atomic_thread_fence(std::memory_order_acquire);
    return HopsOf<true>(mask, ids, flow_hashes, hops);
  }

  // The next hop `dst` resolves to in `domain`, or nullptr for a miss, an
  // unknown domain or a family the router does not serve. Valid until the
  // calling worker's next quiescent state. `flow_hash` picks the member of a
  // group route.
  const NextHop *Resolve(RouteDomainId domain, const utils::Ipv6Address &dst,
                         uint32_t flow_hash = 0) const noexcept {
    const Ipv6RouteTable *routes = Table6Of(domain);
    if (unlikely(routes == nullptr)) {
      return nullptr;
    }
    return HopOf(routes->Read().LookupOrMiss(dst), flow_hash);
  }
  const NextHop *Resolve(RouteDomainId domain, uint32_t dst,
                         uint32_t flow_hash = 0) const noexcept {
    const LpmRouteTable *routes = TableOf(domain);
    if (unlikely(routes == nullptr)) {
      return nullptr;
    }
    return HopOf(routes->Read().LookupOrMiss(dst), flow_hash);
  }
  const NextHop *Resolve(uint32_t dst) const noexcept {
    if (unlikely(default_routes_ == nullptr)) {
      return nullptr;
    }
    return HopOf(default_routes_->Read().LookupOrMiss(dst), 0);
  }

  // Only the route lookup: the next-hop id `dst` matches in `domain`, or
  // kInvalidNextHopId (a miss, an unknown domain, or a route a transaction is
  // still placing, D-023). For pipelines that carry ids and resolve them later
  // with LookupNextHop(s).
  // A group route yields the member `flow_hash` picks.
  NextHopId LookupRoute(RouteDomainId domain, uint32_t dst,
                        uint32_t flow_hash = 0) const noexcept {
    const LpmRouteTable *routes = TableOf(domain);
    if (unlikely(routes == nullptr)) {
      return kInvalidNextHopId;
    }
    uint32_t id = routes->Read().LookupOrMiss(dst);
    if (unlikely(id > max_next_hops_)) {
      if (id == LpmRouteTable::kNoDefault) {
        return kInvalidNextHopId;
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      id = MemberOf(id, flow_hash);
    }
    return NextHopId(id);
  }

  // The group `id` names, or nullptr. Valid until the calling worker's next
  // quiescent state.
  const NextHopGroup *LookupNextHopGroup(NextHopGroupId id) const noexcept {
    return groups_.Lookup(id);
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
  // A domain's FIBs, one per enabled family (null for a disabled one).
  // Immutable once published, like every SlotTable object: each FIB
  // synchronizes itself (rte_lpm or rte_fib in-place updates, the RcuPtr
  // generations ReplaceAll and the IPv6 table publish), so it is reached
  // through a const Domain.
  class Domain {
   public:
    Domain(std::unique_ptr<LpmRouteTable> table,
           std::unique_ptr<Ipv6RouteTable> table6)
        : table_(std::move(table)), table6_(std::move(table6)) {}
    LpmRouteTable *table() const noexcept { return table_.get(); }
    Ipv6RouteTable *table6() const noexcept { return table6_.get(); }
    // Routes in both tables.
    size_t size() const {
      return (table_ ? table_->size() : 0) + (table6_ ? table6_->size() : 0);
    }
    // What its FIBs hold, and their growth hook (D-198).
    size_t bytes() const noexcept {
      return (table_ ? table_->bytes() : 0) + (table6_ ? table6_->bytes() : 0);
    }
    void set_memory_growth(utils::MemoryGrowth *growth) const noexcept {
      if (table_) {
        table_->set_memory_growth(growth);
      }
      if (table6_) {
        table6_->set_memory_growth(growth);
      }
    }

   private:
    std::unique_ptr<LpmRouteTable> table_;
    std::unique_ptr<Ipv6RouteTable> table6_;
  };

  // SlotTable ids are one-based; a domain's slot is its id + 1.
  struct DomainSlotTag;
  using DomainSlot = dataplane::StrongId<DomainSlotTag, uint32_t>;
  // 2^32-1 wraps to the invalid slot 0, so every out-of-range id misses.
  static DomainSlot SlotOf(RouteDomainId domain) noexcept {
    return DomainSlot(domain.value() + 1);
  }

  Router(const std::string &name, const Config &config,
         Domain default_domain, size_t max_next_hops,
         size_t max_domains, size_t max_groups, rcu::RcuDomain &rcu);

  // A domain's FIBs for the router's families, named after `name`.
  static std::expected<Domain, RouteError> MakeDomain(
      const std::string &name, const Config &config, rcu::RcuDomain &rcu);
  // `config` as Create sizes it: `ipv6->max_value` is the largest route
  // value, max_next_hops + max_groups.
  static Config SizedConfig(const Config &config, size_t max_next_hops,
                            size_t max_groups);
  // One domain's FIB set for `config` (D-198): what MakeDomain allocates.
  static size_t DomainBytes(const Config &config) noexcept;

  // What every domain's FIBs hold, and the hook they grow through (D-198):
  // each FIB admits its replacement or IPv6 spare here, as CreateDomain
  // admits a new domain's FIB set and RemoveDomain gives one back. Forwards
  // to the router's own hook (set_memory_growth) when there is one; counts
  // either way, so bytes() follows the FIBs without walking the domains.
  class FibMemory final : public utils::MemoryGrowth {
   public:
    explicit FibMemory(size_t bytes) noexcept : bytes_(bytes) {}
    void Grow(size_t bytes, size_t glibc_bytes) override {
      if (outer_ != nullptr) {
        outer_->Grow(bytes, glibc_bytes);
      }
      bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void Shrink(size_t bytes, size_t glibc_bytes) noexcept override {
      bytes_.fetch_sub(bytes, std::memory_order_relaxed);
      if (outer_ != nullptr) {
        outer_->Shrink(bytes, glibc_bytes);
      }
    }
    size_t bytes() const noexcept {
      return bytes_.load(std::memory_order_relaxed);
    }
    void set_outer(utils::MemoryGrowth *outer) noexcept { outer_ = outer; }

   private:
    std::atomic<size_t> bytes_;
    utils::MemoryGrowth *outer_ = nullptr;
  };

  class RouteResource;
  class NextHopResource;
  class NeighborResource;
  class GroupResource;
  class NextHopStateResource;

  // The resource a value naming next hop `id` references: "<name>/neighbors"
  // for a bound id, else "<name>/next_hops". `in_transaction` reads the
  // bindings as the transaction in progress has them so far (a new value);
  // otherwise the committed ones (a stored value). Engine-serialized.
  dataplane::Reference HopReference(NextHopId id, bool in_transaction) const;
  bool BoundInTransaction(NextHopId id) const;

  // The domain's IPv4 / IPv6 FIB, or null for a domain that does not exist
  // or a family the router does not serve.
  LpmRouteTable *TableOf(RouteDomainId domain) const noexcept {
    const Domain *d = domains_.Lookup(SlotOf(domain));
    return d != nullptr ? d->table() : nullptr;
  }
  Ipv6RouteTable *Table6Of(RouteDomainId domain) const noexcept {
    const Domain *d = domains_.Lookup(SlotOf(domain));
    return d != nullptr ? d->table6() : nullptr;
  }

  // Control-side walk over the existing domains, in id order. The caller holds
  // mutex_, or the transaction engine's lock for an enrolled router (the one
  // writer that can still create a domain holds it, D-158), or has exclusive
  // access (Enroll's check).
  template <typename Fn>
  void ForEachDomain(Fn &&fn) const {
    for (const RouteDomainId id : domain_ids_) {
      fn(id, *domains_.Lookup(SlotOf(id)));
    }
  }
  size_t CountRoutes() const;

  bool ValidId(NextHopId id) const noexcept { return next_hops_.ValidId(id); }
  // 1..max_groups (none when max_groups is 0).
  bool ValidGroupId(NextHopGroupId id) const noexcept {
    return id.value() >= 1 && id.value() < group_references_.size();
  }

  // The member of the group a route value above max_next_hops_ names, as a
  // next-hop id value; 0 (a miss) for a group not published.
  uint32_t MemberOf(uint32_t value, uint32_t flow_hash) const noexcept {
    const NextHopGroup *group = groups_.Lookup(
        NextHopGroupId(static_cast<uint32_t>(value - max_next_hops_)));
    return group != nullptr ? group->Select(flow_hash).value() : 0;
  }

  // A group's value (and kNoDefault) is beyond the next-hop table, where
  // Lookup returns nullptr: the group check runs only on that path, so a
  // router without groups resolves exactly as before (D-065 measured a
  // check ahead of the lookup at +5-15% on the domain benchmarks).
  // Out of line and cold: inlined, it grew the resolve loops enough to cost
  // the no-group path (D-065).
  [[gnu::noinline, gnu::cold]] const NextHop *GroupMemberHop(
      uint32_t value, uint32_t flow_hash) const noexcept {
    if (value <= max_next_hops_ || value == LpmRouteTable::kNoDefault) {
      return nullptr;
    }
    return next_hops_.Lookup(NextHopId(MemberOf(value, flow_hash)));
  }

  // The next hop a looked-up route value names (a next hop or a group
  // member), or nullptr for a miss. The acquire fence pairs with the release
  // a route write puts after its next hop.
  const NextHop *HopOf(uint32_t id, uint32_t flow_hash) const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    const NextHop *hop = next_hops_.Lookup(NextHopId(id));
    if (unlikely(hop == nullptr)) {
      hop = GroupMemberHop(id, flow_hash);
    }
    return hop;
  }

  // Without kHashed, `flow_hashes` is unused and every packet uses hash 0; a
  // template, so the plain path compiles as it did before groups (D-065).
  template <bool kHashed>
  uint64_t ResolveBatchIn(const LpmRouteTable &routes,
                          std::span<const uint32_t> dst,
                          std::span<const uint32_t> flow_hashes,
                          std::span<const NextHop *> hops) const noexcept {
    promise(dst.size() == hops.size() && dst.size() <= kMaxBatch);
    uint32_t ids[kMaxBatch];
    const uint64_t mask =
        routes.Read().LookupBatch(dst, std::span(ids, dst.size()));
    std::atomic_thread_fence(std::memory_order_acquire);
    return HopsOf<kHashed>(mask, ids, flow_hashes, hops);
  }

  // The hops for the looked-up values in `mask`, after the caller's acquire
  // fence; clears the bits that name nothing.
  template <bool kHashed>
  uint64_t HopsOf(uint64_t mask, const uint32_t *ids,
                  std::span<const uint32_t> flow_hashes,
                  std::span<const NextHop *> hops) const noexcept {
    for (uint64_t m = mask; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(__builtin_ctzll(m));
      const NextHop *hop = next_hops_.Lookup(NextHopId(ids[i]));
      if (unlikely(hop == nullptr)) {
        hop = GroupMemberHop(ids[i], kHashed ? flow_hashes[i] : 0);
      }
      if (hop != nullptr) {
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

  // A next hop's availability entry (D-184), control side. A direct user's
  // entries are only kDown (SetNextHopDown(id, false) removes the entry);
  // enrolled, an entry may also say available.
  enum class HopState : uint8_t { kNone, kUp, kDown };
  bool IsDown(NextHopId id) const noexcept {
    return hop_states_[id.value()] == HopState::kDown;
  }
  void SetHopState(NextHopId id, HopState state) noexcept;
  // A transaction's staged availability: SortStagedStates orders
  // staged_states_ (each Prepare does, before it asks), and StagedDown says
  // whether `id` is down once the transaction commits -- its staged state,
  // else its committed one.
  void SortStagedStates() noexcept;
  bool StagedDown(NextHopId id) const noexcept;

  // The reverse index (D-184). ReserveMemberships makes room for one more
  // membership per element of `hops` (a next hop listed twice, two), and may
  // allocate; Add/RemoveMemberships stay within that room and never allocate;
  // EraseEmptyMemberships drops the entries of `hops` left with no group.
  void ReserveMemberships(std::vector<NextHopId> hops);
  void AddMemberships(NextHopGroupId group, std::span<const NextHopId> hops) noexcept;
  void RemoveMemberships(NextHopGroupId group, std::span<const NextHopId> hops) noexcept;
  void EraseEmptyMemberships(std::span<const NextHopId> hops) noexcept;

  const std::string name_;
  rcu::RcuDomain &rcu_;
  const size_t max_domains_;
  const uint32_t max_next_hops_;  // route values above it name groups
  // The families and what a domain created on demand is sized with: the
  // default domain's config (D-158), `ipv6->max_value` set by Create().
  const Config domain_config_;
  // Every domain's FIBs' bytes and growth (D-198); declared before the
  // domains, whose FIBs point at it.
  FibMemory fib_memory_;
  // Slot = domain id + 1. Read lock-free by workers and the engine; written
  // under mutex_ -- by CreateDomain, and by the route resource for a domain a
  // transaction first writes into (D-158).
  dataplane::SlotTable<DomainSlot, Domain> domains_;
  // Domain 0's IPv4 FIB (null without IPv4): it is never removed, so the
  // default-domain readers skip the slot lookup.
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

  // Groups (D-065), mode C like the next hops.
  dataplane::SlotTable<NextHopGroupId, NextHopGroup> groups_;
  std::vector<uint32_t> group_references_;   // routes per group, index = id
  std::vector<uint32_t> member_references_;  // group members per next hop
  struct RetiringGroup {
    NextHopGroupId id;
    rcu::GracePeriod token;
  };
  std::vector<RetiringGroup> retiring_groups_;

  // Path health (D-184), control side: one entry per next hop id, and how
  // many are not kNone.
  std::vector<HopState> hop_states_;
  size_t hop_state_count_ = 0;
  // The groups each next hop is a member of, active or standby: live groups
  // only, so a change of availability never republishes a retiring id. An
  // entry may be empty while a change that reserved room for it is pending.
  std::unordered_map<NextHopId, std::vector<NextHopGroupId>, dataplane::StrongIdHash<NextHopId>>
      memberships_;
  // The transaction being applied: the state changes the state resource
  // reserved, and the groups the groups resource publishes or erases. Each
  // resource clears its own at EndTransaction.
  std::vector<std::pair<NextHopId, HopState>> staged_states_;
  std::vector<NextHopGroupId> staged_groups_;

  // CreateDomain()'s body without the engine guard: the route resource creates
  // a domain a transaction writes into (D-158). Under mutex_.
  std::expected<void, RouteError> CreateDomainLocked(RouteDomainId domain,
                                                     const Config &config);
  // A route value's reference (a next hop's or a group's), control side.
  void Reference(uint32_t value) noexcept;
  void Unreference(uint32_t value) noexcept;

  // Set by Enroll(); the resources exist while enrolled.
  const std::string next_hops_name_;
  const std::string neighbors_name_;
  const std::string next_hop_state_name_;
  const std::string routes_name_;
  const std::string groups_name_;
  dataplane::ResourceRegistry *engine_ = nullptr;
  std::unique_ptr<dataplane::Resource> next_hops_res_;
  std::unique_ptr<dataplane::Resource> neighbors_res_;
  std::unique_ptr<dataplane::Resource> state_res_;
  std::unique_ptr<dataplane::Resource> groups_res_;  // holds nothing without max_groups
  std::unique_ptr<dataplane::Resource> routes_res_;

  // Neighbor bindings (D-183), written by the engine's serialized
  // transactions and read by the control side under the same serialization.
  // Index = next-hop id, sized at construction, so publishing a binding never
  // allocates.
  std::vector<std::optional<NeighborKey>> bindings_;
  size_t bound_count_ = 0;
  NeighborBindingPolicy neighbor_policy_;
  // The transaction in progress: the ids it binds (with their keys) or unbinds
  // (empty), and the static ids it upserts. Cleared when it ends.
  std::map<NextHopId, std::optional<NeighborKey>> staged_bindings_;
  std::set<NextHopId> staged_static_;
  // Published changes the owner has not taken yet; room for every staged one
  // is reserved before anything publishes.
  std::vector<NeighborBindingChange> binding_changes_;
};

}  // namespace bess::route

#endif  // BESS_ROUTE_ROUTER_H_
