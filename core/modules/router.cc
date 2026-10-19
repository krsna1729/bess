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
#include "dataplane/transaction_engine.h"
#include "modules/router_gate_map.h"
#include "packet_reshape.h"
#include "route/forward.h"
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
    ResourceTable::Map<bess::pb::RouterGroupIdKey, bess::pb::RouterGroupValue>(
        "groups"),
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
  const size_t max_groups =
      arg.max_groups() ? static_cast<size_t>(arg.max_groups()) : 0;
  // A FIB value is 24 bits and holds max_next_hops + group id (D-065).
  if (max_groups > route::LpmRouteTable::kMaxValue - max_next_hops) {
    return CommandFailure(EINVAL, "router capacity exceeds backend limits");
  }

  const size_t max_domains =
      arg.max_domains() ? static_cast<size_t>(arg.max_domains()) : 1;
  if (max_domains > route::kMaxRouteDomains) {
    return CommandFailure(EINVAL, "max_domains exceeds %zu",
                          route::kMaxRouteDomains);
  }

  auto router = route::Router::Create(name(), config, max_next_hops,
                                      init_context().rcu(), max_domains,
                                      max_groups);
  if (!router) {
    return CommandFailure(route::RouteErrno(router.error()), "router: %s",
                          route::RouteErrorName(router.error()));
  }
  router_ = std::move(*router);
  // Domains are not allocated here (D-158): a domain a controller first writes
  // a route into is created by that write, so `max_domains` bounds the ids
  // while a router configured for many network instances pays only for the ones
  // that hold routes. A failure to create one surfaces at the route write, as a
  // refused transaction.
  if (auto enrolled = router_->Enroll(init_context().resources());
      !enrolled) {
    next_hops_binding_.Reset();
    groups_binding_.Reset();
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
  groups_binding_ = init_context().codecs().Bind(
      *router_->groups_resource_object(),
      std::make_shared<bess::framework::TypedCodec<bess::pb::RouterGroupIdKey,
                                             bess::pb::RouterGroupValue>>(
          [](const bess::pb::RouterGroupIdKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            if (key.id() == 0) {
              return std::unexpected("group id 0 is invalid");
            }
            return dataplane::EncodeKey(route::NextHopGroupId(key.id()));
          },
          [](const bess::pb::RouterGroupValue &value)
              -> std::expected<std::any, std::string> {
            const int members = value.member_ids_size();
            if (members == 0 ||
                static_cast<size_t>(members) > route::NextHopGroup::kMaxMembers) {
              return std::unexpected(
                  "a group has 1 to " +
                  std::to_string(route::NextHopGroup::kMaxMembers) +
                  " members");
            }
            // A repeated member is a weight: it takes that many selection
            // slots (D-065), so the order is the group's meaning and is kept.
            route::NextHopGroup group;
            group.size = static_cast<uint32_t>(members);
            for (int i = 0; i < members; i++) {
              const uint32_t member = value.member_ids(i);
              if (member == 0) {
                return std::unexpected("group member id 0 is invalid");
              }
              group.members[static_cast<size_t>(i)] = route::NextHopId(member);
            }
            return std::any(group);
          },
          [this](const auto &visit) {
            router_->ForEachNextHopGroup(
                [&visit](route::NextHopGroupId id,
                         const route::NextHopGroup &group) {
                  bess::pb::RouterGroupIdKey key;
                  key.set_id(id.value());
                  bess::pb::RouterGroupValue value;
                  for (uint32_t i = 0; i < group.size; i++) {
                    value.add_member_ids(group.members[i].value());
                  }
                  visit(key, value);
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
            // Absent means the default domain, as before domains existed. A
            // domain the router does not have is created by an upsert's key use
            // (D-158); an erase in a missing domain stays "not found".
            if (key.domain() >= route::kMaxRouteDomains) {
              return std::unexpected("invalid route domain " +
                                     std::to_string(key.domain()));
            }
            return route::Router::RouteKey(route::RouteDomainId(key.domain()),
                                           *prefix);
          },
          [](const bess::pb::RouterRouteValue &value)
              -> std::expected<std::any, std::string> {
            // An id is one-based, so 0 names nothing: a route names exactly
            // one of the two, never both (they are separate namespaces).
            const uint32_t hop = value.next_hop_id();
            const uint32_t group = value.group_id();
            if (hop != 0 && group != 0) {
              return std::unexpected("a route names a next hop or a group, not both");
            }
            if (hop != 0) {
              return std::any(route::NextHopId(hop));
            }
            if (group != 0) {
              return std::any(route::NextHopGroupId(group));
            }
            return std::unexpected("a route names a next hop or a group");
          },
          [this](const auto &visit) {
            router_->ForEachRoute([&visit](const route::RouteKey &id,
                                           route::NextHopId hop,
                                           route::NextHopGroupId group) {
              bess::pb::RouterRouteKey key;
              key.set_ipv4(bess::utils::ToIpv4Address(
                  bess::utils::be32_t(id.prefix.addr())));
              key.set_prefix_length(id.prefix.length());
              key.set_domain(id.domain.value());
              bess::pb::RouterRouteValue value;
              if (group.value() != 0) {
                value.set_group_id(group.value());
              } else {
                value.set_next_hop_id(hop.value());
              }
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
    groups_binding_.Reset();
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
    groups_binding_.Reset();
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
  groups_binding_.Reset();
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

// One counter per verdict the chain reports (route/forward.h). kForward is not
// a drop; kNoEgress is the blackhole shape (D-160), counted apart from a miss
// because the route exists and chose that hop.
void Router::CountVerdict(route::ForwardVerdict verdict) {
  switch (verdict) {
    case route::ForwardVerdict::kNotIpv4:
      not_ipv4_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kMalformed:
      malformed_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kNonInitialFragment:
      non_initial_fragments_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kNoRoute:
      no_route_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kUnresolved:
      unresolved_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kTtlExpired:
      ttl_expired_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kNoEgress:
      // A route to a hop that names no egress interface: the blackhole shape
      // (D-160). Counted, unlike a hop whose interface has no gate in this
      // graph (the adapter's own check, which is configuration).
      blackholed_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kForward:
      return;
  }
}

// Mode 2 (D-155): the destination path, through the library's forwarding chain
// (route/forward.h, D-157) -- the same two calls the appliance makes
// (examples/appliances/router, Process), so the two paths cannot disagree
// about a fragment, a TTL boundary or which hop address is written.
//
// What is this module's: the domain from metadata (policy a controller writes),
// the copy a shared payload needs before its header is written (packet
// ownership, which is a graph's business and not a library's), the counters,
// and the gate a hop's egress maps to. The parse, the prefix lookup with the
// frame's flow hash, the neighbor state, the TTL and the addresses are the
// chain's.
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

    // The domain id is 24 bits wide (Router::RouteKey packs it above the
    // prefix; route/route_domain.h): no narrowing here, so a domain this
    // router does not hold is looked up as itself and misses instead of
    // aliasing onto another domain's FIB.
    const route::RouteDomainId domain =
        has_domain
            ? route::RouteDomainId(
                  get_attr_with_offset<bess::utils::be32_t>(domain_offset, pkt).value())
            : route::kDefaultRouteDomainId;
    const route::FrameRoute resolved = route::ResolveFrame(
        *router_, domain,
        std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()), pkt.total_len());
    if (resolved.verdict != route::ForwardVerdict::kForward) {
      CountVerdict(resolved.verdict);
      continue;
    }
    const gate_idx_t gate = GateOf(resolved.egress);
    if (gate == DROP_GATE) {
      // A hop whose egress is no gate of this graph: configuration, not
      // traffic, so nothing is counted and nothing is written.
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
        static_cast<size_t>(resolved.l3_offset) + sizeof(bess::utils::Ipv4)) {
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (route::WriteForwarded(
            std::span(writable.head_data<uint8_t *>(), writable.head_len()),
            resolved.l3_offset, *resolved.hop) != route::TtlResult::kForward) {
      // ResolveFrame refuses a TTL of 0 or 1, so this is unreachable with the
      // same bytes; counting it keeps the boundary honest if a mutation ever
      // rewrote the header between the two calls.
      ttl_expired_frames_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    gates[i] = gate;
  }
  EmitBatch(ctx, batch, gates);
}

std::string Router::GetDesc() const {
  if (!routes_by_destination()) {
    return bess::utils::Format(
        "%zu routes, %zu next hops, %zu groups, %zu header write failures, next-hop id from metadata",
        router_->route_count(), router_->next_hop_count(),
        router_->next_hop_group_count(),
        static_cast<size_t>(header_write_failures()));
  }
  return bess::utils::Format(
      "%zu routes, %zu next hops, %zu groups, %zu header write failures, routing by "
      "destination (not ipv4: %zu, malformed: %zu, non-initial fragments: %zu, "
      "no route: %zu, unresolved: %zu, ttl expired: %zu, blackholed: %zu)",
      router_->route_count(), router_->next_hop_count(),
      router_->next_hop_group_count(),
      static_cast<size_t>(header_write_failures()),
      static_cast<size_t>(not_ipv4_frames()), static_cast<size_t>(malformed_frames()),
      static_cast<size_t>(non_initial_fragments()), static_cast<size_t>(no_route_frames()),
      static_cast<size_t>(unresolved_frames()),
      static_cast<size_t>(ttl_expired_frames()),
      static_cast<size_t>(blackholed_frames()));
}

ADD_MODULE(Router, "router",
           "forwards to the egress its next-hop id names, or -- with no "
           "next_hop_id writer -- routes each packet by its destination "
           "address in its route domain; writes a resolved next hop's "
           "addresses into the frame (routes and next hops as transactional "
           "resources)")
