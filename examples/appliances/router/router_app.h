// SPDX-License-Identifier: BSD-3-Clause

#ifndef APPLIANCES_ROUTER_ROUTER_APP_H_
#define APPLIANCES_ROUTER_ROUTER_APP_H_

// Reference appliance R1 (roadmap M24): an IPv4 router with VRFs, ECMP and
// neighbor resolution, written as an application against BESS's installed
// headers. What is the application's: the static control policy below, the
// interface-to-device mapping (an interface is a graph gate here), and what a
// verdict becomes -- an unresolved neighbor is dropped, a TTL of 0 or 1 is
// dropped. What is BESS's: route domains and their FIBs, next hops and groups
// (route/router.h), the neighbor table (route/neighbor_table.h), and the
// forwarding chain (route/forward.h): the checked parse, the lookup with the
// frame's own flow hash, the neighbor state, the TTL and the L2 rewrite.
//
// The same class runs two ways: called directly on frames (no module graph:
// RouterApp::Process), and inside a module that maps gates to interfaces
// (router_appliance.cc). Both call the same chain, so they cannot disagree
// about a frame.

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dataplane/interface_id.h"
#include "framework/memory_scope.h"
#include "rcu/rcu_domain.h"
#include "route/connected_routes.h"
#include "route/forward.h"
#include "route/neighbor_table.h"
#include "route/router.h"
#include "utils/ether.h"

namespace appliance {

using bess::dataplane::InterfaceId;
using bess::route::Ipv4Prefix;
using bess::route::NextHop;
using bess::route::NextHopGroupId;
using bess::route::NextHopId;
using bess::route::RouteDomainId;
using bess::utils::Ethernet;

class RouterApp {
 public:
  // An interface: its MAC, the VRF packets arriving on it are routed in, and
  // its own address, which is what its connected route is made from (D-164).
  struct Interface {
    InterfaceId id;
    Ethernet::Address mac;
    RouteDomainId vrf;
    uint32_t address;  // host order
    uint8_t length;    // its prefix length
  };

  // The verdict vocabulary is the library's (route/forward.h): this path and
  // the Router module name the same outcomes, and a decision adds only where
  // the frame was headed.
  struct Decision {
    bess::route::ForwardVerdict verdict = bess::route::ForwardVerdict::kNotIp;
    InterfaceId egress{};
  };

  // The router is admitted through `memory` (D-198, resource "router") before
  // it is allocated: a refusal throws utils::MemoryRefused and builds nothing.
  // Domains the static policy adds are growth admitted through the same lease;
  // all of it is released when the app is destroyed.
  static std::expected<std::unique_ptr<RouterApp>, std::string> Create(
      bess::rcu::RcuDomain &rcu, bess::framework::MemoryScope &memory) {
    // IPv4 only: the static policy routes IPv4, so IPv6 frames are
    // kFamilyDisabled and no IPv6 FIB is allocated.
    const std::string name = "r1_appliance";
    const bess::route::Router::Config config{
        .ipv4 = bess::route::LpmRouteTable::Config{.max_routes = 64, .tbl8_groups = 8},
        .ipv6 = std::nullopt};
    constexpr size_t kMaxNextHops = 16, kMaxDomains = 3, kMaxGroups = 4;
    auto lease = memory.LeaseFor<bess::route::Router>("router", name, config, kMaxNextHops, rcu,
                                                      kMaxDomains, kMaxGroups);
    auto router = bess::route::Router::Create(name, config, kMaxNextHops, rcu, kMaxDomains,
                                              kMaxGroups);
    if (!router) {
      return std::unexpected(std::string("router: ") + bess::route::RouteErrorName(router.error()));
    }
    std::unique_ptr<RouterApp> app(
        new RouterApp(bess::framework::MemoryScope::Adopt(std::move(lease), std::move(*router))));
    if (std::string err = app->ApplyStaticPolicy(); !err.empty()) {
      return std::unexpected(err);
    }
    return app;
  }

  // The interface (1-based id) a frame arrived on decides its VRF.
  const Interface *InterfaceOf(InterfaceId id) const noexcept {
    const size_t i = static_cast<size_t>(id.value()) - 1;
    return i < interfaces_.size() ? &interfaces_[i] : nullptr;
  }

  // Routes one frame in place (the direct path): the library's forwarding chain
  // (route/forward.h, D-157), which parses the frame, looks the destination up
  // in the VRF's FIB with the frame's own flow hash, checks the neighbor,
  // decrements the TTL and writes the hop's addresses. The VRF comes from the
  // interface the frame arrived on; an unresolved neighbor is dropped, which is
  // the application's choice. Nothing of the frame changes unless the verdict
  // is kForward. `frame` is the packet's first segment, `total_len` its length
  // (0: the span's).
  Decision Process(InterfaceId ingress, std::span<uint8_t> frame, size_t total_len = 0) const noexcept {
    const Interface *in = InterfaceOf(ingress);
    if (in == nullptr) {
      return {};  // not an interface of this appliance: nothing to route by
    }
    const bess::route::FrameRoute resolved =
        bess::route::ResolveFrame(*router_, in->vrf, frame, total_len);
    if (resolved.verdict != bess::route::ForwardVerdict::kForward) {
      return {resolved.verdict, resolved.egress};
    }
    if (bess::route::WriteForwarded(frame, resolved.family, resolved.l3_offset, *resolved.hop) !=
        bess::route::TtlResult::kForward) {
      // ResolveFrame refuses a TTL of 0 or 1, so this needs the header to have
      // changed between the two calls; naming it keeps the boundary honest.
      return {bess::route::ForwardVerdict::kTtlExpired, resolved.egress};
    }
    return {bess::route::ForwardVerdict::kForward, resolved.egress};
  }

  // What address resolution learned: every next hop bound to the neighbor is
  // republished (one pointer store each). No route is written: the FIBs keep
  // naming the same next-hop ids.
  bool LearnNeighbor(InterfaceId interface, uint32_t ipv4, const Ethernet::Address &mac,
                     bess::route::NeighborState state = bess::route::NeighborState::kResolved) {
    for (const auto &[id, hop] : neighbors_.Update({interface, ipv4}, state, mac)) {
      if (!router_->SetNextHop(id, hop)) {
        return false;
      }
    }
    return true;
  }

  const bess::route::Router &router() const noexcept { return *router_; }
  // Route writes the application made: only the static policy's.
  size_t route_writes() const noexcept { return route_writes_; }

  // The static policy's addresses (host order), for callers and tests.
  static constexpr uint32_t kGatewayA = 0xc0000201;  // 192.0.2.1 via if1 (VRF 1)
  static constexpr uint32_t kGatewayB = 0xc6120001;  // 198.18.0.1 via if2 (VRF 1)
  static constexpr uint32_t kGatewayC = 0xc6336401;  // 198.51.100.1 via if3 (VRF 2)
  static constexpr RouteDomainId kVrf1{1}, kVrf2{2};

 private:
  explicit RouterApp(bess::framework::Sized<bess::route::Router> router) : router_(std::move(router)) {}

  static Ethernet::Address Mac(uint8_t last) {
    Ethernet::Address a;
    const uint8_t bytes[6] = {0x02, 0, 0, 0, 0, last};
    std::memcpy(&a, bytes, 6);
    return a;
  }

  // The static control policy (the application's): four interfaces, two VRFs
  // that both route 10.0.0.0/8 (overlapping), an ECMP pair in VRF 1,
  // 172.16.0.0/12 in VRF 1 through one gateway, and each interface's own
  // connected route (D-164).
  std::string ApplyStaticPolicy() {
    interfaces_ = {Interface{InterfaceId{uint16_t{1}}, Mac(1), kVrf1, 0xc00002fe, 24},
                   Interface{InterfaceId{uint16_t{2}}, Mac(2), kVrf1, 0xc61200fe, 24},
                   Interface{InterfaceId{uint16_t{3}}, Mac(3), kVrf2, 0xc63364fe, 24},
                   Interface{InterfaceId{uint16_t{4}}, Mac(4), kVrf2, 0xcb0071fe, 24}};
    for (const RouteDomainId vrf : {kVrf1, kVrf2}) {
      if (!router_->CreateDomain(
              vrf, bess::route::Router::Config{
                       .ipv4 = bess::route::LpmRouteTable::Config{.max_routes = 16, .tbl8_groups = 4},
                       .ipv6 = std::nullopt})) {
        return "create VRF";
      }
    }
    const struct {
      NextHopId id;
      uint16_t interface;
      uint32_t gateway;
    } hops[] = {{NextHopId{1}, 1, kGatewayA}, {NextHopId{2}, 2, kGatewayB}, {NextHopId{3}, 3, kGatewayC}};
    for (const auto &h : hops) {
      const InterfaceId ifid{h.interface};
      const NextHop bound = neighbors_.Bind(h.id, {ifid, h.gateway}, InterfaceOf(ifid)->mac);
      if (!router_->SetNextHop(h.id, bound)) {
        return "next hop";
      }
    }
    const std::array<NextHopId, 2> pair = {NextHopId{1}, NextHopId{2}};
    if (!router_->SetNextHopGroup(NextHopGroupId{1}, pair)) {
      return "group";
    }
    const auto ten = *Ipv4Prefix::Make(0x0a000000, 8);
    const auto private_b = *Ipv4Prefix::Make(0xac100000, 12);
    if (!router_->SetRoute(kVrf1, ten, NextHopGroupId{1}) || !router_->SetRoute(kVrf2, ten, NextHopId{3}) ||
        !router_->SetRoute(kVrf1, private_b, NextHopId{1})) {
      return "route";
    }
    route_writes_ = 3;
    // The routes the interfaces' own addresses imply (D-164, D-178), derived
    // by the library so this application and a controller cannot disagree
    // about the arithmetic: each address's subnet is a connected route (the
    // hop names the interface, no neighbor resolved, so a frame to the subnet
    // itself is unresolved and dropped), and the address itself is a local
    // route (delivery, never forwarded). A host on a subnet needs its own /32
    // route and hop: LearnNeighbor updates the hops bound to a neighbor
    // (NeighborTable::Bind), and these are bound to nothing -- binding one to
    // a host would point the whole subnet at that host's MAC. Hops 1..3 above
    // are the gateways, so these start at 4.
    std::vector<bess::route::InterfaceAddress> addresses;
    addresses.reserve(interfaces_.size());
    for (const Interface &iface : interfaces_) {
      addresses.push_back(bess::route::InterfaceAddress{
          .interface = iface.id,
          .domain = iface.vrf,
          .address = iface.address,
          .length = iface.length,
      });
    }
    const auto derived = bess::route::DerivedRoutes::Derive(addresses);
    if (!derived) {
      return std::string("derived routes: ") +
             bess::route::DerivationErrorName(derived.error());
    }
    uint32_t derived_hop = 4;
    for (const bess::route::DerivedRoute &entry : derived->entries()) {
      const NextHopId hop_id{derived_hop++};
      if (!router_->SetNextHop(hop_id, entry.hop()) ||
          !router_->SetRoute(entry.domain, entry.prefix, hop_id)) {
        return "derived route";
      }
      route_writes_++;
    }
    // Resolution (ARP, in a real router) learns the gateways' MACs.
    return LearnNeighbor(InterfaceId{uint16_t{1}}, kGatewayA, Mac(0xa1)) &&
                   LearnNeighbor(InterfaceId{uint16_t{2}}, kGatewayB, Mac(0xa2)) &&
                   LearnNeighbor(InterfaceId{uint16_t{3}}, kGatewayC, Mac(0xa3))
               ? ""
               : "neighbor";
  }

  bess::framework::Sized<bess::route::Router> router_;
  bess::route::NeighborTable neighbors_;
  std::vector<Interface> interfaces_;
  size_t route_writes_ = 0;
};

}  // namespace appliance

#endif  // APPLIANCES_ROUTER_ROUTER_APP_H_
