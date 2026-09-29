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
#include <vector>

#include "dataplane/batch_stages.h"
#include "dataplane/resource.h"
#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"
#include "gate.h"
#include "packet.h"
#include "packet_mutation.h"
#include "route/route_table.h"
#include "utils/ether.h"

namespace bess::dataplane {
class TransactionEngine;
}  // namespace bess::dataplane

namespace bess::route {

// A next hop's stable id: one-based, zero invalid (like ActionId), and at
// most 24 bits because it is what the route table stores.
struct NextHopIdTag;
using NextHopId = dataplane::StrongId<NextHopIdTag, uint32_t>;
inline constexpr NextHopId kInvalidNextHopId{};

enum class NeighborState : uint8_t {
  kResolved,     // L2 addresses known; forward with RewriteL2()
  kIncomplete,   // resolution pending (ARP/ND in flight)
  kUnreachable,  // resolution failed
};

// Where a route leads (K7): the egress (a BESS output gate -- in a module
// graph, the path to the egress port), the neighbor state, and the L2
// addresses to write when the neighbor is resolved. What to do with packets
// toward an unresolved neighbor (queue, punt, drop) is the application's.
struct NextHop {
  gate_idx_t egress = DROP_GATE;
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

// Routes plus next hops (K7):
//
//   IPv4 dst --LPM--> NextHopId --ObjectTable--> NextHop
//
// Many routes share a next hop, so a neighbor change (a new MAC, an
// unresolved neighbor) publishes one next-hop object (a SlotTable pointer
// store, O(1)) and never touches the route table; a route change is one
// in-place rte_lpm update and never copies next hops.
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
// must not be called from a worker.
//
// Transactions (G1.2b, D-023): Enroll() registers the next hops and the
// routes as two resources of a TransactionEngine, so that one transaction can
// change them together with other modules' tables (a rule, the action it
// names, the route that action forwards to). An enrolled router has one
// writer, the engine: the direct setters above refuse (kEnrolled), and the
// engine's reference ledger replaces the router's own counts.
class Router {
 public:
  using Config = LpmRouteTable::Config;

  static std::expected<std::unique_ptr<Router>, RouteError> Create(
      std::string name, const Config &config, size_t max_next_hops,
      rcu::RcuDomain &domain);

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

  // Adds or re-points a route; /0 is the default route.
  std::expected<void, RouteError> SetRoute(Ipv4Prefix prefix, NextHopId hop);
  std::expected<void, RouteError> RemoveRoute(Ipv4Prefix prefix);

  size_t route_count() const { return routes_->size(); }
  size_t next_hop_count() const;
  // Routes naming `id` (control-side reference count).
  size_t RouteReferences(NextHopId id) const;

  // -- transactions (D-023) ---------------------------------------------------

  // Registers "<name>/next_hops" (key: EncodeKey(NextHopId), value: NextHop)
  // and "<name>/routes" (key: RouteKey(prefix), value: NextHopId; each route
  // references its next hop) with `engine`, which must outlive the router or
  // its enrollment. Refused once any route exists or a removed next hop is
  // still retiring (the ledger must start from what it can see). Destroying
  // an enrolled router unregisters both (with workers paused).
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
  static dataplane::ResourceKey RouteKey(Ipv4Prefix prefix) {
    return dataplane::EncodeKey(uint64_t{prefix.addr()} << 8 |
                                prefix.length());
  }

  // Operations for this router's resources.
  dataplane::Op SetNextHopOp(NextHopId id, const NextHop &hop) const {
    return dataplane::Op::Upsert(next_hops_name_, dataplane::EncodeKey(id),
                                 std::any(hop));
  }
  dataplane::Op RemoveNextHopOp(NextHopId id) const {
    return dataplane::Op::Erase(next_hops_name_, dataplane::EncodeKey(id));
  }
  dataplane::Op SetRouteOp(Ipv4Prefix prefix, NextHopId hop) const {
    return dataplane::Op::Upsert(routes_name_, RouteKey(prefix),
                                 std::any(hop));
  }
  dataplane::Op RemoveRouteOp(Ipv4Prefix prefix) const {
    return dataplane::Op::Erase(routes_name_, RouteKey(prefix));
  }

  // -- reader -----------------------------------------------------------------

  static constexpr size_t kMaxBatch = LpmRouteTable::kMaxBatch;

  // Resolves each destination (host order) to its next hop. Returns a mask
  // with bit i set where `hops[i]` was written; other positions untouched.
  uint64_t ResolveBatch(std::span<const uint32_t> dst,
                        std::span<const NextHop *> hops) const noexcept {
    promise(dst.size() == hops.size() && dst.size() <= kMaxBatch);
    uint32_t ids[kMaxBatch];
    uint64_t mask = routes_->Read().LookupBatch(dst, std::span(ids, dst.size()));
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

  const NextHop *Resolve(uint32_t dst) const noexcept {
    const auto id = routes_->Read().Lookup(dst);
    std::atomic_thread_fence(std::memory_order_acquire);
    return id ? next_hops_.Lookup(*id) : nullptr;
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
  Router(std::string name, std::unique_ptr<RouteTable<NextHopId>> routes,
         size_t max_next_hops, rcu::RcuDomain &domain);

  class RouteResource;

  bool ValidId(NextHopId id) const noexcept { return next_hops_.ValidId(id); }

  // ReclaimRetired() with mutex_ held.
  size_t CompleteRetirementsLocked();

  std::unique_ptr<RouteTable<NextHopId>> routes_;
  rcu::RcuDomain &domain_;
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
