// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ROUTER_H_
#define BESS_MODULES_ROUTER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <rte_ring.h>

#include "framework/module_tick.h"
#include "framework/resource_bindings.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "route/forward.h"
#include "route/neighbor_resolver.h"
#include "route/router.h"

// K7's route table and next hops as a module, enrolled in the transaction
// engine (D-023).
//
// Packet path, two modes (D-155). The mode is configuration, set in Init: a
// module decides, it does not change what it decides from because a graph's
// wiring changed underneath it.
//
//  1. ROUTE_MODE_NEXT_HOP_ID (the default). The next-hop id arrives in packet
//     metadata (a `next_hop_id` be32 attribute a classifier result writes): the
//     module resolves it -- one acquire load per id -- and the packet leaves
//     on the gate that hop's egress names (in a module graph, the path to the
//     egress port). This is the classifier's decision and the module executes
//     it. A graph that writes no id fails closed: every packet is dropped,
//     and the resume hook says so once, because a guessed gate is worse than
//     a drop.
//
//  2. ROUTE_MODE_DESTINATION. The module decides, the way the router
//     appliance does (examples/appliances/router) and with the same library
//     calls: the route domain the packet carries (a `route_domain` be32
//     attribute a VIF writes; domain 0 when nothing writes it) selects the
//     FIB. An IPv4 frame goes through the library's forwarding chain
//     (route/forward.h, D-157), which parses the frame, looks the destination
//     up in that domain, checks the neighbor, decrements the TTL and writes
//     the hop's addresses. forward.h is IPv4-only: an IPv6 frame is resolved
//     here, against the domain's IPv6 routes, with the same hop, neighbor,
//     group and punt policy, route::DecrementHopLimit and the same address
//     write (D-179). Link-scoped IPv6 -- a link-local or multicast destination,
//     or a link-local source -- is never forwarded (RFC 4291 2.5.6): it goes
//     to the local punt gate, or is dropped, and is counted either way. A
//     packet leaves on its hop's egress gate. A route names a next hop or a
//     next-hop group; a group route picks its member by the frame's flow hash,
//     so a flow keeps one member in both paths. A frame whose headers do not
//     parse, a non-initial IPv4 fragment (its L4 header is in an earlier
//     fragment, so it cannot be hashed consistently), and a frame that is
//     neither IPv4 nor IPv6 are dropped and counted. The `next_hop_id`
//     attribute is ignored in this mode, even if some module writes it.
//
// Both modes drop a packet whose hop is unknown, or whose neighbor is not
// resolved: what to do toward an unresolved neighbor (queue, punt, drop) is
// the application's, and dropping is the choice that never forwards on a
// guess. docs/routing.md is the guide: the library's pieces, the two recipes
// and what a graph decides.
//
// Neighbor resolution (D-179). With `interfaces` and `neighbor_bindings`
// (both fixed at Init), the module owns those next hops' adjacencies: a
// route::NeighborResolver solicits (ARP, ND) from a task, learns from ARP
// replies and Neighbor Advertisements read with the ingress interface
// attribute (default `vif_id`), ages and fails neighbors, and publishes each
// change as a next-hop update through the engine on the maintenance tick.
// Timings are configured in milliseconds and converted to ticks at Init.
// ARP requests and Neighbor Solicitations are not the resolver's: they go to
// the local punt gate, or are dropped, and are counted.
// docs/neighbor-resolution.md describes the state machine.
//
// A resolved hop that names both of its addresses has them written into the
// frame's Ethernet header before it leaves -- through the forwarding chain in
// destination mode (route/forward.h's WriteForwarded, D-157), through
// route::RewriteL2 in the next-hop-id mode -- so a frame crossing a routing
// boundary carries this node's addresses rather than the ones it arrived
// with. A hop that names none (or only one) is forwarded
// untouched: a zero address means "not named" in the resource's read-back
// (D-129), and writing one would put an invalid frame on the wire. A frame
// whose header cannot be written -- a shared payload that could not be copied,
// or a header that crosses a segment -- is dropped and counted, because
// forwarding it with the addresses it arrived with would put it on the wire as
// someone else's.
//
// Control path: `<module>/next_hops`, `<module>/groups` (sized by
// `max_groups`; a route names a next hop or a group) and `<module>/routes`
// are resources, so one transaction can create a session's next hop, the
// route to it and the rule whose result names the hop. An enrolled
// router has one writer, the engine: the direct setters refuse with kEnrolled,
// and the engine's ledger replaces the router's own reference counts. New
// routes are placed during prepare with the value their addresses already
// resolve to, so rte_lpm capacity is settled before anything is visible.
class Router final : public Module {
 public:
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;
  // "<module>/next_hops" (RouterNextHopIdKey -> RouterNextHopValue),
  // "<module>/groups" (RouterGroupIdKey -> RouterGroupValue, with
  // `max_groups`) and "<module>/routes" (RouterRouteKey -> RouterRouteValue),
  // D-129.
  static const ResourceTables resource_tables;

  Router() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::RouterArg &arg);
  void DeInit() override;

  // Reports an unreadable 'next_hop_id' attribute once per resume (the
  // control side's place for it, as in ExactMatch): the packet path only
  // fails closed.
  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  // Emits queued ARP/ND solicitations from the resolver's control ring.
  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;
  std::string GetDesc() const override;

  // The enrolled router, for tests and for the module's own introspection.
  const bess::route::Router *router() const { return router_.get(); }

  // True when the module decides each packet's next hop itself, from the
  // packet's destination address in its route domain (D-155).
  bool routes_by_destination() const {
    return mode_ == bess::pb::ROUTE_MODE_DESTINATION;
  }

  // Frames dropped because a header could not be written: a resolved next
  // hop's addresses into the Ethernet header, or the TTL into the IPv4 header
  // (the payload was shared and could not be copied, or the header crosses a
  // segment). Relaxed: the packet path only increments, control reads it
  // (GetDesc).
  uint64_t header_write_failures() const noexcept {
    return header_write_failures_.load(std::memory_order_relaxed);
  }

  // What destination routing dropped, by reason (relaxed, as above): a frame
  // that is neither IPv4 nor IPv6, one whose headers do not parse, one that
  // is not the first fragment of its datagram, one the FIB has no route for,
  // one whose hop's neighbor is not resolved, one whose TTL or hop limit ran
  // out, one whose hop forwards nowhere (a blackhole route, D-160), and one
  // the route says is this node's own address (local delivery, D-178).
  uint64_t not_ip_frames() const noexcept {
    return not_ip_frames_.load(std::memory_order_relaxed);
  }
  uint64_t malformed_frames() const noexcept {
    return malformed_frames_.load(std::memory_order_relaxed);
  }
  uint64_t non_initial_fragments() const noexcept {
    return non_initial_fragments_.load(std::memory_order_relaxed);
  }
  uint64_t no_route_frames() const noexcept {
    return no_route_frames_.load(std::memory_order_relaxed);
  }
  uint64_t unresolved_frames() const noexcept {
    return unresolved_frames_.load(std::memory_order_relaxed);
  }
  uint64_t ttl_expired_frames() const noexcept {
    return ttl_expired_frames_.load(std::memory_order_relaxed);
  }
  uint64_t blackholed_frames() const noexcept {
    return blackholed_frames_.load(std::memory_order_relaxed);
  }
  // Frames a local route addressed to this node (D-178). Counted in both modes;
  // where they went is punt_local_gate, DROP_GATE meaning no local path.
  uint64_t local_frames() const noexcept {
    return local_frames_.load(std::memory_order_relaxed);
  }
  // IPv6 frames that must not be forwarded (D-179): a link-local or multicast
  // destination, or a link-local source. Like local frames, they go to
  // punt_local_gate or are dropped, and are counted either way.
  uint64_t link_scope_frames() const noexcept {
    return link_scope_frames_.load(std::memory_order_relaxed);
  }
  // ARP requests and Neighbor Solicitations a resolving router received: not
  // the resolver's input, so they go to punt_local_gate or are dropped, and
  // are counted either way.
  uint64_t neighbor_requests() const noexcept {
    return neighbor_requests_.load(std::memory_order_relaxed);
  }
  // ARP replies and Neighbor Advertisements dropped before the resolver saw
  // them: no readable interface id, an interface the resolver does not have,
  // or a full input ring.
  uint64_t neighbor_input_drops() const noexcept {
    return neighbor_input_drops_.load(std::memory_order_relaxed);
  }
  // Neighbor changes learned but not yet published (they wait for a later
  // tick when the engine is busy or a tick's cap is reached).
  size_t pending_neighbor_updates() const noexcept {
    return pending_neighbor_updates_.size();
  }

 private:
  // Mode 1 (D-155): resolve the `next_hop_id` each packet carries.
  void RouteById(Context *ctx, bess::PacketBatch *batch, bess::metadata::mt_offset_t offset);
  // Mode 2 (D-155): look each packet's destination address up in its domain's
  // FIB, then run the forwarding chain the appliance runs.
  void RouteByDestination(Context *ctx, bess::PacketBatch *batch);
  // Resolves frames [begin, end) of `batch`, all in `domain`, and fills their
  // gates: one batched lookup for a run of more than one (route/forward.h's
  // ResolveFrames; a run of one takes the scalar ResolveFrame), then the
  // per-frame verdict handling below.
  void ResolveRun(bess::PacketBatch *batch, int begin, int end,
                  bess::route::RouteDomainId domain, gate_idx_t *gates);
  // One counter per verdict the chain reports (route/forward.h). kForward is
  // not a drop; kNoEgress is the blackhole shape (D-160), counted apart from a
  // miss because the route exists and chose that hop.
  void CountVerdict(bess::route::ForwardVerdict verdict);
  // The gate a counted, unforwarded verdict sends its frame to (D-162,
  // D-178): the miss, unresolved or local punt gate, else DROP_GATE.
  gate_idx_t PuntGateOf(bess::route::ForwardVerdict verdict) const;
  // Destination mode for one IPv6 frame (forward.h is IPv4-only): resolves,
  // counts, and writes the hop limit and addresses. Returns the frame's gate.
  gate_idx_t RouteIpv6(bess::PacketBatch *batch, int i,
                       bess::route::RouteDomainId domain,
                       const bess::conntrack::ParsedFlowPacket &parsed);
  // Takes resolver input (ARP replies, Neighbor Advertisements) out of the
  // batch onto the receive ring, punts or drops ARP requests and Neighbor
  // Solicitations, and leaves everything else in the batch.
  void DivertNeighborFrames(Context *ctx, bess::PacketBatch *batch);
  // Publishes at most kMaxNeighborUpdatesPerTick pending neighbor changes in
  // one transaction; the rest wait for the next tick.
  void PublishNeighborUpdates(uint64_t now);
  void ResolverTick(bess::framework::TickContext &ctx);
  void QueueNeighborUpdates(
      std::span<const std::pair<bess::route::NextHopId,
                                bess::route::NextHop>> updates);
  void CleanupNeighborResolver();

  // One resolver input frame on the receive ring: the packet and the
  // interface it arrived on, read when the worker enqueued it (metadata
  // offsets may move at the next resume; the frame's interface does not).
  struct NeighborFrame {
    bess::PacketHandle packet;
    uint32_t interface;
    uint32_t reserved;
  };
  static_assert(sizeof(NeighborFrame) % 4 == 0, "an rte_ring element size");
  static constexpr size_t kMaxNeighborUpdatesPerTick = 64;
  // At most one publication warning per this many maintenance ticks.
  static constexpr uint64_t kNeighborWarningInterval = 1000;

  std::unique_ptr<bess::route::Router> router_;
  // After router_, which owns the resources they bind (D-044).
  bess::framework::ResourceBinding next_hops_binding_;
  bess::framework::ResourceBinding groups_binding_;
  bess::framework::ResourceBinding routes_binding_;
  std::unique_ptr<bess::route::NeighborResolver> neighbor_resolver_;
  bess::framework::TickEndpoint neighbor_tick_;
  rte_ring *neighbor_rx_ring_ = nullptr;  // NeighborFrame elements
  rte_ring *neighbor_tx_ring_ = nullptr;
  int ingress_interface_attr_ = -1;
  std::string ingress_interface_attr_name_;
  // Fixed after Init, so workers read it without a lock.
  std::unordered_map<uint32_t, gate_idx_t> resolver_gates_;
  std::vector<std::pair<bess::route::NextHopId, bess::route::NextHop>>
      pending_neighbor_updates_;
  // When the last publication warning was given (maintenance ticks).
  uint64_t neighbor_warned_tick_ = 0;
  bool neighbor_warned_ = false;
  int next_hop_id_attr_ = -1;
  // The registered names, for the log lines that name them.
  std::string next_hop_attr_name_;
  std::string domain_attr_name_;
  // The route domain a packet arrived in, written by the graph's VIF; domain 0
  // when nothing writes it (D-155, the attribute's default contract).
  int route_domain_attr_ = -1;
  // What the packet path decides from (D-155): configuration, not a guess
  // about the graph's wiring.
  bess::pb::RouteMode mode_ = bess::pb::ROUTE_MODE_NEXT_HOP_ID;
  // Where a frame the destination path cannot forward goes, instead of being
  // dropped (D-162): a miss and an unresolved neighbor each have their own
  // gate, and DROP_GATE means drop. Either way the frame is counted.
  gate_idx_t punt_miss_gate_ = DROP_GATE;
  gate_idx_t punt_unresolved_gate_ = DROP_GATE;
  // Where a frame to this node's own address goes (D-178): no kernel path
  // exists in a dataplane graph, so the graph names the slow path (a stack
  // adapter, a control endpoint) or drops. Either way it is counted.
  gate_idx_t punt_local_gate_ = DROP_GATE;
  // Which fields a group route hashes to pick its member (D-163). The proto
  // default is the whole flow, so this only differs when a graph says so.
  bess::route::FlowHashFields flow_fields_ = bess::route::FlowHashFields::kL3L4;
  std::atomic<uint64_t> header_write_failures_{0};
  std::atomic<uint64_t> not_ip_frames_{0};
  std::atomic<uint64_t> malformed_frames_{0};
  std::atomic<uint64_t> non_initial_fragments_{0};
  std::atomic<uint64_t> no_route_frames_{0};
  std::atomic<uint64_t> unresolved_frames_{0};
  std::atomic<uint64_t> ttl_expired_frames_{0};
  // A route sent the frame to a hop that names no egress interface: the
  // blackhole shape (D-160), counted apart from a miss.
  std::atomic<uint64_t> blackholed_frames_{0};
  // A route said the destination is this node's address (D-178): delivery, not
  // a drop and not a miss.
  std::atomic<uint64_t> local_frames_{0};
  // An IPv6 frame of link scope (D-179): never forwarded.
  std::atomic<uint64_t> link_scope_frames_{0};
  // ARP requests and Neighbor Solicitations: punted or dropped, not resolved.
  std::atomic<uint64_t> neighbor_requests_{0};
  // Resolver input dropped before the resolver saw it.
  std::atomic<uint64_t> neighbor_input_drops_{0};
};

#endif  // BESS_MODULES_ROUTER_H_
