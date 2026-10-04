// SPDX-License-Identifier: BSD-3-Clause

#ifndef APPLIANCES_ROUTER_ROUTER_APP_H_
#define APPLIANCES_ROUTER_ROUTER_APP_H_

// Reference appliance R1 (roadmap M24): an IPv4 router with VRFs, ECMP and
// neighbor resolution, written as an application against BESS's installed
// headers. What is the application's: the static control policy below, the
// interface-to-device mapping (an interface is a graph gate here), the flow
// hash, and what to do with an unresolved neighbor (drop). What is BESS's:
// route domains and their FIBs, next hops and groups (route/router.h), the
// neighbor table (route/neighbor_table.h), the checked parse
// (conntrack/packet_parse.h), TTL and the L2 rewrite (route/l3_packet.h).
//
// The same class runs two ways: called directly on frames (no module graph:
// RouterApp::Process), and inside a module that maps gates to interfaces
// (router_appliance.cc). Both make the same decision for the same frame.

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "conntrack/packet_parse.h"
#include "dataplane/interface_id.h"
#include "rcu/rcu_domain.h"
#include "route/l3_packet.h"
#include "route/neighbor_table.h"
#include "route/router.h"
#include "utils/ether.h"
#include "utils/ip.h"

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
  // An interface: its MAC and the VRF packets arriving on it are routed in.
  struct Interface {
    InterfaceId id;
    Ethernet::Address mac;
    RouteDomainId vrf;
  };

  enum class Verdict : uint8_t { kForward, kNotIpv4, kNoRoute, kUnresolved, kTtlExpired };
  struct Decision {
    Verdict verdict = Verdict::kNotIpv4;
    InterfaceId egress{};
  };

  static std::expected<std::unique_ptr<RouterApp>, std::string> Create(bess::rcu::RcuDomain &rcu) {
    auto router = bess::route::Router::Create(
        "r1_appliance", bess::route::Router::Config{.max_routes = 64, .tbl8_groups = 8},
        /*max_next_hops=*/16, rcu, /*max_domains=*/3, /*max_groups=*/4);
    if (!router) {
      return std::unexpected(std::string("router: ") + bess::route::RouteErrorName(router.error()));
    }
    std::unique_ptr<RouterApp> app(new RouterApp(std::move(*router)));
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

  // Routes one frame in place (the direct path): the checked parse, the VRF's
  // FIB with the flow hash for a group route, the neighbor state, TTL, then the
  // L2 rewrite. Nothing of the frame changes unless the verdict is kForward.
  // `frame` is the packet's first segment, `total_len` its length (0: the span's).
  Decision Process(InterfaceId ingress, std::span<uint8_t> frame, size_t total_len = 0) const noexcept {
    const Interface *in = InterfaceOf(ingress);
    bess::conntrack::ParsedFlowPacket p;
    if (in == nullptr ||
        bess::conntrack::ParseFrame(frame, p, total_len) != bess::conntrack::ParseStatus::kOk ||
        p.l3 != bess::conntrack::L3Kind::kIpv4) {
      return {Verdict::kNotIpv4};
    }
    uint32_t dst;
    std::memcpy(&dst, p.dst.data(), 4);
    dst = __builtin_bswap32(dst);
    const NextHop *hop = router_->Resolve(in->vrf, dst, FlowHash(p));
    if (hop == nullptr) {
      return {Verdict::kNoRoute};
    }
    if (hop->neighbor != bess::route::NeighborState::kResolved) {
      return {Verdict::kUnresolved, hop->egress};  // the application's choice: drop
    }
    auto *ip = reinterpret_cast<bess::utils::Ipv4 *>(frame.data() + p.l3_offset);
    if (bess::route::DecrementTtl(*ip) != bess::route::TtlResult::kForward) {
      return {Verdict::kTtlExpired, hop->egress};
    }
    auto *eth = reinterpret_cast<Ethernet *>(frame.data());
    eth->dst_addr = hop->dst_mac;
    eth->src_addr = hop->src_mac;
    return {Verdict::kForward, hop->egress};
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
  static constexpr uint32_t kGatewayA = 0xc0000201;   // 192.0.2.1 via if1 (VRF 1)
  static constexpr uint32_t kGatewayB = 0xc0000202;   // 192.0.2.2 via if2 (VRF 1)
  static constexpr uint32_t kGatewayC = 0xc6336401;   // 198.51.100.1 via if3 (VRF 2)
  static constexpr RouteDomainId kVrf1{1}, kVrf2{2};

 private:
  explicit RouterApp(std::unique_ptr<bess::route::Router> router) : router_(std::move(router)) {}

  static Ethernet::Address Mac(uint8_t last) {
    Ethernet::Address a;
    const uint8_t bytes[6] = {0x02, 0, 0, 0, 0, last};
    std::memcpy(&a, bytes, 6);
    return a;
  }

  // The application's flow hash: addresses, ports and protocol, mixed.
  static uint32_t FlowHash(const bess::conntrack::ParsedFlowPacket &p) noexcept {
    uint32_t s, d;
    std::memcpy(&s, p.src.data(), 4);
    std::memcpy(&d, p.dst.data(), 4);
    uint64_t x = (uint64_t{s} << 32 | d) ^ (uint64_t{p.src_port} << 24 | uint64_t{p.dst_port} << 8 |
                                           p.protocol);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    return static_cast<uint32_t>(x ^ (x >> 31));
  }

  // The static control policy (the application's): four interfaces, two VRFs
  // that both route 10.0.0.0/8 (overlapping), an ECMP pair in VRF 1, and
  // 172.16.0.0/12 in VRF 1 through one gateway.
  std::string ApplyStaticPolicy() {
    interfaces_ = {Interface{InterfaceId{uint16_t{1}}, Mac(1), kVrf1},
                   Interface{InterfaceId{uint16_t{2}}, Mac(2), kVrf1},
                   Interface{InterfaceId{uint16_t{3}}, Mac(3), kVrf2},
                   Interface{InterfaceId{uint16_t{4}}, Mac(4), kVrf2}};
    for (const RouteDomainId vrf : {kVrf1, kVrf2}) {
      if (!router_->CreateDomain(vrf, bess::route::Router::Config{.max_routes = 16, .tbl8_groups = 4})) {
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
    // Resolution (ARP, in a real router) learns the gateways' MACs.
    return LearnNeighbor(InterfaceId{uint16_t{1}}, kGatewayA, Mac(0xa1)) &&
                   LearnNeighbor(InterfaceId{uint16_t{2}}, kGatewayB, Mac(0xa2)) &&
                   LearnNeighbor(InterfaceId{uint16_t{3}}, kGatewayC, Mac(0xa3))
               ? ""
               : "neighbor";
  }

  std::unique_ptr<bess::route::Router> router_;
  bess::route::NeighborTable neighbors_;
  std::vector<Interface> interfaces_;
  size_t route_writes_ = 0;
};

}  // namespace appliance

#endif  // APPLIANCES_ROUTER_ROUTER_APP_H_
