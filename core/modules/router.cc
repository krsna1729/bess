// SPDX-License-Identifier: BSD-3-Clause

#include "router.h"

#include <array>
#include <cstdint>
#include <limits>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "framework/resource_bindings.h"
#include "conntrack/packet_parse.h"
#include "dataplane/transaction_engine.h"
#include "modules/router_gate_map.h"
#include "packet_reshape.h"
#include "route/l3_packet.h"
#include "utils/endian.h"
#include "utils/ether.h"
#include "utils/format.h"
#include "utils/ip.h"

namespace dataplane = bess::dataplane;
namespace route = bess::route;

const Commands Router::cmds = {};

const ResourceTables Router::resource_tables = {
    ResourceTable::Map<bess::pb::RouterNextHopIdKey,
                       bess::pb::RouterNextHopValue>("next_hops"),
    ResourceTable::Map<bess::pb::RouterRouteKey, bess::pb::RouterRouteValue>(
        "routes"),
};

namespace {

// The RPC's neighbor state (D-025). Unset means resolved: a next hop written
// through the RPC is one the controller means to forward to.
std::expected<route::NeighborState, std::string> NeighborFromPb(
    bess::pb::RouterNeighborState state) {
  switch (state) {
    case bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE:
      return route::NeighborState::kIncomplete;
    case bess::pb::ROUTER_NEIGHBOR_STATE_UNREACHABLE:
      return route::NeighborState::kUnreachable;
    case bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED:
    case bess::pb::ROUTER_NEIGHBOR_STATE_UNSPECIFIED:
      return route::NeighborState::kResolved;
    default:
      return std::unexpected("invalid neighbor state " +
                             std::to_string(static_cast<int>(state)));
  }
}

// A 6-byte MAC, or the zero address when the field is absent.
std::expected<bess::utils::Ethernet::Address, std::string> MacFromPb(
    const std::string &bytes, const char *what) {
  bess::utils::Ethernet::Address mac{};
  if (bytes.empty()) {
    return mac;
  }
  if (bytes.size() != bess::utils::Ethernet::Address::kSize) {
    return std::unexpected(std::string(what) + " must be " +
                           std::to_string(bess::utils::Ethernet::Address::kSize) +
                           " bytes");
  }
  return bess::utils::Ethernet::Address(
      reinterpret_cast<const uint8_t *>(bytes.data()));
}

// The read-back's canonical next hop (D-129): defaults omitted, as proto3
// does -- no neighbor when resolved (an unset neighbor means resolved), no
// MAC when zero, any other MAC as its 6 bytes. A client sending an implied
// default explicitly (RESOLVED, 6 zero bytes) gets the same next hop, which
// reads back in this form.
bess::pb::RouterNextHopValue NextHopToPb(const route::NextHop &hop) {
  bess::pb::RouterNextHopValue value;
  value.set_egress_gate(bess::modules::router::GateOf(hop.egress));
  switch (hop.neighbor) {
    case route::NeighborState::kResolved:
      break;
    case route::NeighborState::kIncomplete:
      value.set_neighbor(bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE);
      break;
    case route::NeighborState::kUnreachable:
      value.set_neighbor(bess::pb::ROUTER_NEIGHBOR_STATE_UNREACHABLE);
      break;
  }
  if (!hop.dst_mac.IsZero()) {
    value.set_dst_mac(reinterpret_cast<const char *>(hop.dst_mac.bytes),
                      bess::utils::Ethernet::Address::kSize);
  }
  if (!hop.src_mac.IsZero()) {
    value.set_src_mac(reinterpret_cast<const char *>(hop.src_mac.bytes),
                      bess::utils::Ethernet::Address::kSize);
  }
  return value;
}

}  // namespace

using bess::modules::router::GateOf;
using bess::modules::router::InterfaceOf;

CommandResponse Router::Init(const bess::pb::RouterArg &arg) {
  if (arg.max_routes() > std::numeric_limits<uint32_t>::max() ||
      arg.max_tbl8s() > std::numeric_limits<uint32_t>::max() ||
      arg.max_next_hops() > route::LpmRouteTable::kMaxValue) {
    return CommandFailure(EINVAL, "router capacity exceeds backend limits");
  }
  // The packet path's mode is configuration (D-155): an unknown value is
  // refused rather than silently treated as the default.
  if (arg.mode() != bess::pb::ROUTE_MODE_NEXT_HOP_ID &&
      arg.mode() != bess::pb::ROUTE_MODE_DESTINATION) {
    return CommandFailure(EINVAL, "unknown route mode %d", static_cast<int>(arg.mode()));
  }
  mode_ = arg.mode();
  route::LpmRouteTable::Config config;
  config.max_routes = arg.max_routes() ? arg.max_routes() : 4096;
  config.tbl8_groups = arg.max_tbl8s() ? arg.max_tbl8s() : 256;
  switch (arg.backend()) {
    case bess::pb::ROUTE_BACKEND_LPM:
      config.backend = route::RouteBackend::kRteLpm;
      break;
    case bess::pb::ROUTE_BACKEND_FIB:
      config.backend = route::RouteBackend::kRteFib;
      break;
    default:
      return CommandFailure(EINVAL, "unknown route backend %d", arg.backend());
  }
  const size_t max_next_hops =
      arg.max_next_hops() ? static_cast<size_t>(arg.max_next_hops()) : 4096;

  const size_t max_domains =
      arg.max_domains() ? static_cast<size_t>(arg.max_domains()) : 1;
  if (max_domains > route::kMaxRouteDomains) {
    return CommandFailure(EINVAL, "max_domains exceeds %zu",
                          route::kMaxRouteDomains);
  }

  auto router = route::Router::Create(name(), config, max_next_hops,
                                      init_context().rcu(), max_domains);
  if (!router) {
    return CommandFailure(route::RouteErrno(router.error()), "router: %s",
                          route::RouteErrorName(router.error()));
  }
  router_ = std::move(*router);
  // Domains are structural: all exist before the router is enrolled.
  for (size_t d = 1; d < max_domains; d++) {
    if (auto created =
            router_->CreateDomain(route::RouteDomainId(static_cast<uint32_t>(d)),
                                  config);
        !created) {
      const route::RouteError error = created.error();
      router_.reset();
      return CommandFailure(route::RouteErrno(error), "router: domain %zu: %s",
                            d, route::RouteErrorName(error));
    }
  }
  if (auto enrolled = router_->Enroll(init_context().resources());
      !enrolled) {
    next_hops_binding_.Reset();
    routes_binding_.Reset();
    router_.reset();
    return CommandFailure(EINVAL, "%s", enrolled.error().c_str());
  }

  // Typed keys and values over the RPC (D-025).
  next_hops_binding_ = init_context().codecs().Bind(
      *router_->next_hops_resource_object(),
      std::make_shared<bess::framework::TypedCodec<bess::pb::RouterNextHopIdKey,
                                             bess::pb::RouterNextHopValue>>(
          [](const bess::pb::RouterNextHopIdKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            if (key.id() == 0) {
              return std::unexpected("next hop id 0 is invalid");
            }
            return dataplane::EncodeKey(route::NextHopId(key.id()));
          },
          [](const bess::pb::RouterNextHopValue &value)
              -> std::expected<std::any, std::string> {
            if (!bess::IsValidGateValue(value.egress_gate())) {
              return std::unexpected("invalid egress gate " +
                                     std::to_string(value.egress_gate()));
            }
            auto dst = MacFromPb(value.dst_mac(), "dst_mac");
            if (!dst) {
              return std::unexpected(dst.error());
            }
            auto src = MacFromPb(value.src_mac(), "src_mac");
            if (!src) {
              return std::unexpected(src.error());
            }
            route::NextHop hop;
            hop.egress = InterfaceOf(value.egress_gate());
            auto neighbor = NeighborFromPb(value.neighbor());
            if (!neighbor) {
              return std::unexpected(neighbor.error());
            }
            hop.neighbor = *neighbor;
            hop.dst_mac = *dst;
            hop.src_mac = *src;
            return std::any(hop);
          },
          [this](const auto &visit) {
            router_->ForEachNextHop(
                [&visit](route::NextHopId id, const route::NextHop &hop) {
                  bess::pb::RouterNextHopIdKey key;
                  key.set_id(id.value());
                  visit(key, NextHopToPb(hop));
                });
          }));
  routes_binding_ = init_context().codecs().Bind(
      *router_->routes_resource_object(),
      std::make_shared<bess::framework::TypedCodec<bess::pb::RouterRouteKey,
                                             bess::pb::RouterRouteValue>>(
          [](const bess::pb::RouterRouteKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            bess::utils::be32_t addr;
            if (!bess::utils::ParseIpv4Address(key.ipv4(), &addr)) {
              return std::unexpected("invalid IPv4 prefix address '" +
                                     key.ipv4() + "'");
            }
            if (key.prefix_length() > 32) {
              return std::unexpected("invalid prefix length");
            }
            const auto prefix = route::Ipv4Prefix::Make(
                addr.value(), static_cast<uint8_t>(key.prefix_length()));
            if (!prefix) {
              return std::unexpected(route::RouteErrorName(prefix.error()));
            }
            // Absent means the default domain, as before domains existed; a
            // domain the router does not have is refused when the key is used.
            if (key.domain() >= route::kMaxRouteDomains) {
              return std::unexpected("invalid route domain " +
                                     std::to_string(key.domain()));
            }
            return route::Router::RouteKey(route::RouteDomainId(key.domain()),
                                           *prefix);
          },
          [](const bess::pb::RouterRouteValue &value)
              -> std::expected<std::any, std::string> {
            if (value.next_hop_id() == 0) {
              return std::unexpected("next hop id 0 is invalid");
            }
            return std::any(route::NextHopId(value.next_hop_id()));
          },
          // The module binds no groups: every route names a next hop.
          [this](const auto &visit) {
            router_->ForEachRoute([&visit](const route::RouteKey &id,
                                           route::NextHopId hop,
                                           route::NextHopGroupId) {
              bess::pb::RouterRouteKey key;
              key.set_ipv4(bess::utils::ToIpv4Address(
                  bess::utils::be32_t(id.prefix.addr())));
              key.set_prefix_length(id.prefix.length());
              key.set_domain(id.domain.value());
              bess::pb::RouterRouteValue value;
              value.set_next_hop_id(hop.value());
              visit(key, value);
            });
          }));

  next_hop_id_attr_ = AddMetadataAttr("next_hop_id",
                                      sizeof(bess::utils::be32_t),
                                      bess::metadata::Attribute::AccessMode::kRead);
  if (next_hop_id_attr_ < 0) {
    auto released = router_->Release();
    CHECK(released) << released.error();
    next_hops_binding_.Reset();
    routes_binding_.Reset();
    router_.reset();
    return CommandFailure(-next_hop_id_attr_, "add_metadata_attr() failed");
  }
  // Optional (D-155): the route domain a packet arrived in, which the graph's
  // VIF writes. No writer means domain 0, the default domain -- the same
  // attribute name the VIF uses, so a graph gets the packet's own domain with
  // no configuration on either side.
  route_domain_attr_ = AddMetadataAttr("route_domain",
                                       sizeof(bess::utils::be32_t),
                                       bess::metadata::Attribute::AccessMode::kRead);
  if (route_domain_attr_ < 0) {
    auto released = router_->Release();
    CHECK(released) << released.error();
    next_hops_binding_.Reset();
    routes_binding_.Reset();
    router_.reset();
    return CommandFailure(-route_domain_attr_, "add_metadata_attr() failed");
  }
  return CommandSuccess();
}

void Router::DeInit() {
  if (router_ == nullptr) {
    return;
  }
  // An action references these next hops, so release rather than unregister:
  // the order modules are destroyed in must not decide whether a pipeline can
  // be torn down.
  auto released = router_->Release();
  CHECK(released) << released.error();
  next_hops_binding_.Reset();
  routes_binding_.Reset();
  router_.reset();
}

int Router::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  // Metadata offsets are assigned just before this: an attribute with no
  // valid offset means the packet path would read byte 0, which is worth
  // saying once, here, rather than never.
  if (routes_by_destination()) {
    const bool has_domain =
        bess::metadata::IsValidOffset(attr_offset(route_domain_attr_));
    LOG(INFO) << "Router '" << name() << "': routing by destination address in "
              << (has_domain ? "the route domain each packet carries"
                             : "domain 0 (nothing writes 'route_domain')");
    return 0;
  }
  if (!bess::metadata::IsValidOffset(attr_offset(next_hop_id_attr_))) {
    LOG(ERROR) << "Router '" << name()
               << "': the 'next_hop_id' attribute is unreadable (is an "
                  "ActionTable writing it?); dropping packets";
  }
  return 0;
}

void Router::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (!routes_by_destination()) {
    RouteById(ctx, batch, attr_offset(next_hop_id_attr_));
    return;
  }
  RouteByDestination(ctx, batch);
}

// Mode 1 (D-155): the next-hop id each packet carries, resolved to a hop.
void Router::RouteById(Context *ctx, bess::PacketBatch *batch,
                       bess::metadata::mt_offset_t offset) {
  const int cnt = batch->cnt();
  if (!bess::metadata::IsValidOffset(offset)) {
    // Fail closed: an unresolved id would send the packet to a guessed gate.
    // OnEvent(PreResume) is where this is reported.
    RunChooseModule(ctx, DROP_GATE, batch);
    return;
  }
  std::array<route::NextHopId, bess::PacketBatch::kMaxBurst> ids;
  std::array<const route::NextHop *, bess::PacketBatch::kMaxBurst> hops;
  for (int i = 0; i < cnt; i++) {
    ids[i] = route::NextHopId(
        get_attr_with_offset<bess::utils::be32_t>(offset, batch->packet(i))
            .value());
  }
  const uint64_t resolved = router_->LookupNextHops(
      std::span(ids).first(static_cast<size_t>(cnt)),
      std::span(hops).first(static_cast<size_t>(cnt)));
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    const route::NextHop *hop =
        (resolved & (uint64_t{1} << i)) != 0 ? hops[i] : nullptr;
    const bool usable =
        hop != nullptr && hop->neighbor == route::NeighborState::kResolved;
    // A zero address means "not named" (D-129), and half a pair would write an
    // invalid frame: a hop that names both is rewritten, any other is
    // forwarded with the header it arrived with.
    if (!usable || hop->dst_mac.IsZero() || hop->src_mac.IsZero()) {
      gates[i] = usable ? GateOf(hop->egress) : DROP_GATE;
      continue;
    }
    gates[i] = GateOf(hop->egress);
    bess::PacketRef pkt = batch->packet(i);
    auto rewritten = route::RewriteL2(pkt, *hop);
    if (!rewritten &&
        rewritten.error() == bess::packet::MutationError::kSharedStorage) {
      // A shared payload: copy it first, then write (K4). The copy may
      // replace the mbuf, so the batch keeps the new handle.
      bess::PacketHandle handle = pkt.handle();
      if (!bess::packet::EnsureWritable(handle)) {
        gates[i] = DROP_GATE;
        header_write_failures_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      batch->handles()[i] = handle;
      rewritten = route::RewriteL2(bess::PacketRef(handle), *hop);
    }
    if (!rewritten) {
      gates[i] = DROP_GATE;
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  EmitBatch(ctx, batch, gates);
}

// Mode 2 (D-155): the appliance's chain (examples/appliances/router, Process),
// with the same library calls in the same order -- parse the frame, look the
// destination up in the packet's route domain, drop what the FIB does not
// resolve and what the neighbor is not resolved for, decrement the TTL, write
// the hop's addresses, and leave on the hop's egress gate.
//
// A non-initial fragment is dropped, as the appliance drops it: its L4 header
// is in an earlier fragment, so it has no ports to hash with, and a group
// route would then pick a member by a different hash than its own first
// fragment did. Forwarding fragments is a separate decision, and it needs a
// hash that covers every fragment of a datagram consistently.
void Router::RouteByDestination(Context *ctx, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  const bess::metadata::mt_offset_t domain_offset = attr_offset(route_domain_attr_);
  const bool has_domain = bess::metadata::IsValidOffset(domain_offset);
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    gates[i] = DROP_GATE;
    bess::PacketRef pkt = batch->packet(i);

    bess::conntrack::ParsedFlowPacket p;
    const bess::conntrack::ParseStatus parsed = bess::conntrack::ParseFrame(
        std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()), p, pkt.total_len());
    if (parsed != bess::conntrack::ParseStatus::kOk) {
      // Not an IP packet, headers that do not fit or lie about their lengths,
      // or a non-initial fragment: nothing to route by, or nothing to hash
      // with.
      if (parsed == bess::conntrack::ParseStatus::kNotIp) {
        not_ipv4_frames_.fetch_add(1, std::memory_order_relaxed);
      } else if (parsed == bess::conntrack::ParseStatus::kFragment) {
        non_initial_fragments_.fetch_add(1, std::memory_order_relaxed);
      } else {
        malformed_frames_.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    if (p.l3 != bess::conntrack::L3Kind::kIpv4) {
      not_ipv4_frames_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    // The destination in the LPM's own order (host order, whatever the host
    // is: the endian wrapper converts) and the flow hash a group route picks a
    // member with -- route::FlowHash, the one function the appliance hashes
    // with too, so a flow keeps one member in both paths. Its address words
    // are opaque, so both paths pass the header's bytes as they are.
    bess::utils::be32_t dst;
    std::memcpy(&dst, p.dst.data(), sizeof(dst));
    uint32_t src_word, dst_word;
    std::memcpy(&src_word, p.src.data(), sizeof(src_word));
    std::memcpy(&dst_word, p.dst.data(), sizeof(dst_word));
    const uint32_t flow_hash =
        route::FlowHash(src_word, dst_word, p.src_port, p.dst_port, p.protocol);
    // The domain id is 24 bits wide (Router::RouteKey packs it above the
    // prefix; route/route_domain.h): no narrowing here, so a domain this
    // router does not hold is looked up as itself and misses instead of
    // aliasing onto another domain's FIB.
    const route::RouteDomainId domain =
        has_domain
            ? route::RouteDomainId(
                  get_attr_with_offset<bess::utils::be32_t>(domain_offset, pkt).value())
            : route::kDefaultRouteDomainId;
    const route::NextHop *hop = router_->Resolve(domain, dst.value(), flow_hash);
    if (hop == nullptr) {
      no_route_frames_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (hop->neighbor != route::NeighborState::kResolved) {
      unresolved_frames_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const gate_idx_t gate = GateOf(hop->egress);
    if (gate == DROP_GATE) {
      // The hop names no usable egress: the frame is leaving anyway, so it is
      // neither copied nor written.
      continue;
    }

    auto *ip = reinterpret_cast<const bess::utils::Ipv4 *>(
        pkt.head_data<const uint8_t *>() + p.l3_offset);
    if (ip->ttl <= 1) {
      // The same boundary DecrementTtl refuses at, read before the header is
      // made writable so a packet that must expire is not copied first.
      ttl_expired_frames_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // The TTL is written whatever the hop names, so the header must be
    // writable: a shared payload is copied once (K4) and the copy replaces
    // the batch's handle.
    bess::PacketHandle handle = pkt.handle();
    if (!bess::packet::EnsureWritable(handle)) {
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    batch->handles()[i] = handle;
    bess::PacketRef writable(handle);
    // The copy may have resegmented the frame, so where the IPv4 header is is
    // re-established rather than assumed: writing it through a stale pointer
    // would touch bytes that are not in this segment.
    if (writable.head_len() <
        static_cast<size_t>(p.l3_offset) + sizeof(bess::utils::Ipv4)) {
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    auto *wip = reinterpret_cast<bess::utils::Ipv4 *>(
        writable.head_data<uint8_t *>() + p.l3_offset);
    if (route::DecrementTtl(*wip) != route::TtlResult::kForward) {
      ttl_expired_frames_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    gates[i] = gate;
    // A hop that names both addresses is written; one that names none, or only
    // one, is forwarded with the header it arrived with (D-129).
    if (hop->dst_mac.IsZero() || hop->src_mac.IsZero()) {
      continue;
    }
    if (!route::RewriteL2(writable, *hop)) {
      gates[i] = DROP_GATE;
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  EmitBatch(ctx, batch, gates);
}

std::string Router::GetDesc() const {
  if (!routes_by_destination()) {
    return bess::utils::Format(
        "%zu routes, %zu next hops, %zu header write failures, next-hop id from metadata",
        router_->route_count(), router_->next_hop_count(),
        static_cast<size_t>(header_write_failures()));
  }
  return bess::utils::Format(
      "%zu routes, %zu next hops, %zu header write failures, routing by "
      "destination (not ipv4: %zu, malformed: %zu, non-initial fragments: %zu, "
      "no route: %zu, unresolved: %zu, ttl expired: %zu)",
      router_->route_count(), router_->next_hop_count(),
      static_cast<size_t>(header_write_failures()),
      static_cast<size_t>(not_ipv4_frames()), static_cast<size_t>(malformed_frames()),
      static_cast<size_t>(non_initial_fragments()), static_cast<size_t>(no_route_frames()),
      static_cast<size_t>(unresolved_frames()),
      static_cast<size_t>(ttl_expired_frames()));
}

ADD_MODULE(Router, "router",
           "forwards to the egress its next-hop id names, or -- with no "
           "next_hop_id writer -- routes each packet by its destination "
           "address in its route domain; writes a resolved next hop's "
           "addresses into the frame (routes and next hops as transactional "
           "resources)")
