// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_ROUTE_ROUTER_H_
#define BESS_ROUTE_ROUTER_H_

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

#include "dataplane/object_table.h"
#include "dataplane/strong_id.h"
#include "gate.h"
#include "packet.h"
#include "packet_mutation.h"
#include "route/route_table.h"
#include "utils/ether.h"

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
// unresolved neighbor) republishes only the next-hop table and never touches
// the route table; a route change is one in-place rte_lpm update and never
// copies next hops.
//
// The two halves are published independently, and the ordering rules that
// keep a reader from ever resolving a route to a missing next hop are
// enforced here rather than left to callers:
//
//  - a route may only name a next hop that exists; the next hop is published
//    first and a release fence orders it before the route's entries;
//  - a reader resolves routes first and loads the next-hop generation after an
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
    const NextHopTable *table = next_hops_.Read();
    for (uint64_t m = mask; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(__builtin_ctzll(m));
      if (const NextHop *hop = table->Lookup(NextHopId(ids[i]))) {
        hops[i] = hop;
      } else {
        mask &= ~(uint64_t{1} << i);  // unreachable by construction
      }
    }
    return mask;
  }

  const NextHop *Resolve(uint32_t dst) const noexcept {
    const auto id = routes_->Read().Lookup(dst);
    std::atomic_thread_fence(std::memory_order_acquire);
    return id ? next_hops_.Read()->Lookup(*id) : nullptr;
  }

 private:
  using NextHopTable = dataplane::ObjectTable<NextHopId, NextHop>;

  Router(std::unique_ptr<RouteTable<NextHopId>> routes, size_t max_next_hops,
         rcu::RcuDomain &domain);

  bool ValidId(NextHopId id) const noexcept {
    return id.value() != 0 && id.value() < desired_.size();
  }

  // Builds and publishes the next-hop table from desired_.
  void PublishNextHops();
  // ReclaimRetired() with mutex_ held.
  size_t CompleteRetirementsLocked();

  std::unique_ptr<RouteTable<NextHopId>> routes_;
  rcu::RcuDomain &domain_;
  rcu::RcuPtr<NextHopTable> next_hops_;

  mutable std::mutex mutex_;
  std::vector<std::optional<NextHop>> desired_;  // index = id
  std::vector<uint32_t> references_;             // routes per next hop
  struct Retiring {
    NextHopId id;
    rcu::GracePeriod token;
  };
  std::vector<Retiring> retiring_;  // removed, still published
  std::vector<bool> is_retiring_;   // index = id
};

}  // namespace bess::route

#endif  // BESS_ROUTE_ROUTER_H_
