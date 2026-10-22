// SPDX-License-Identifier: BSD-3-Clause

#include "router.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <cstring>
#include <expected>
#include <new>
#include <unordered_set>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "modules/router_gate_map.h"
#include "packet_pool.h"
#include "worker.h"
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

// The RPC's hop kind (D-178). Unset means forward, so every value written
// before local delivery existed keeps its meaning.
std::expected<route::NextHopKind, std::string> KindFromPb(
    bess::pb::RouterNextHopKind kind) {
  switch (kind) {
    case bess::pb::ROUTER_NEXT_HOP_KIND_UNSPECIFIED:
    case bess::pb::ROUTER_NEXT_HOP_KIND_FORWARD:
      return route::NextHopKind::kForward;
    case bess::pb::ROUTER_NEXT_HOP_KIND_LOCAL:
      return route::NextHopKind::kLocal;
    default:
      return std::unexpected("invalid next hop kind " +
                             std::to_string(static_cast<int>(kind)));
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
  // A local hop is canonical as kind LOCAL alone: no egress, no neighbor, no
  // MACs, so the desired state that form describes applies as a no-op (D-129,
  // D-178). A forwarding hop omits the kind, which reads back as the default.
  if (hop.kind == route::NextHopKind::kLocal) {
    value.set_kind(bess::pb::ROUTER_NEXT_HOP_KIND_LOCAL);
    return value;
  }
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
  // The punt gates (D-162): 0 is the proto default and DROP_GATE the explicit
  // spelling, and both mean drop and count. Any other value must be a gate
  // this graph can have; gate 0 itself is a real gate and is not a target.
  if (!bess::IsValidGateValue(arg.punt_miss_gate()) ||
      !bess::IsValidGateValue(arg.punt_unresolved_gate()) ||
      !bess::IsValidGateValue(arg.punt_local_gate())) {
    return CommandFailure(EINVAL, "invalid punt gate %llu/%llu/%llu",
                          static_cast<unsigned long long>(arg.punt_miss_gate()),
                          static_cast<unsigned long long>(arg.punt_unresolved_gate()),
                          static_cast<unsigned long long>(arg.punt_local_gate()));
  }
  punt_miss_gate_ = arg.punt_miss_gate() == 0
                        ? DROP_GATE
                        : static_cast<gate_idx_t>(arg.punt_miss_gate());
  punt_unresolved_gate_ = arg.punt_unresolved_gate() == 0
                              ? DROP_GATE
                              : static_cast<gate_idx_t>(arg.punt_unresolved_gate());
  punt_local_gate_ = arg.punt_local_gate() == 0
                         ? DROP_GATE
                         : static_cast<gate_idx_t>(arg.punt_local_gate());
  // The flow hash's fields (D-163): the proto default is the whole flow, and an
  // unknown value (an arg from a newer binary) keeps that default too.
  flow_fields_ = arg.flow_hash() == bess::pb::FLOW_HASH_L3 ? route::FlowHashFields::kL3
                                                           : route::FlowHashFields::kL3L4;
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
  if (arg.resolver_interfaces_size() != 0 ||
      arg.neighbor_bindings_size() != 0) {
    const size_t resolver_capacity =
        arg.max_neighbors() == 0 ? 1024 : static_cast<size_t>(arg.max_neighbors());
    if (static_cast<uint64_t>(resolver_capacity) !=
        (arg.max_neighbors() == 0 ? 1024u : arg.max_neighbors())) {
      return CommandFailure(EINVAL,
                            "neighbor resolver capacity is out of range");
    }
    if (arg.resolver_interfaces_size() == 0 ||
        arg.neighbor_bindings_size() == 0) {
      return CommandFailure(EINVAL,
                            "neighbor resolution requires interfaces and bindings");
    }
    if (arg.neighbor_max_probes() > std::numeric_limits<uint32_t>::max()) {
      return CommandFailure(EINVAL, "neighbor_max_probes is out of range");
    }
    std::unordered_set<uint32_t> bound_next_hops;
    route::NeighborResolver::Config resolver_config;
    resolver_config.capacity = resolver_capacity;
    resolver_config.timing = {
        .first_probe = arg.neighbor_first_probe() == 0
                           ? 1000
                           : arg.neighbor_first_probe(),
        .probe_interval = arg.neighbor_probe_interval() == 0
                              ? 1000
                              : arg.neighbor_probe_interval(),
        .max_probe_interval = arg.neighbor_max_probe_interval() == 0
                                  ? 8000
                                  : arg.neighbor_max_probe_interval(),
        .max_probes = arg.neighbor_max_probes() == 0
                          ? 3
                          : static_cast<uint32_t>(arg.neighbor_max_probes()),
        .reachable_lifetime = arg.neighbor_reachable_lifetime() == 0
                                  ? 30000
                                  : arg.neighbor_reachable_lifetime(),
        .unreachable_retry = arg.neighbor_unreachable_retry() == 0
                                  ? 60000
                                  : arg.neighbor_unreachable_retry(),
    };
    auto resolver = route::NeighborResolver::Create(resolver_config);
    if (!resolver) {
      return CommandFailure(EINVAL, "%s", resolver.error().c_str());
    }
    neighbor_resolver_ = std::move(*resolver);
    for (const auto &entry : arg.resolver_interfaces()) {
      if (entry.id() == 0 || !bess::IsValidGateValue(entry.egress_gate()) ||
          InterfaceOf(entry.egress_gate()).value() != entry.id() ||
          resolver_gates_.contains(entry.id())) {
        return CommandFailure(EINVAL,
                              "resolver interface id must be unique and equal "
                              "to egress_gate + 1");
      }
      auto mac = MacFromPb(entry.mac(), "resolver interface mac");
      if (!mac || mac->IsZero()) {
        return CommandFailure(EINVAL, "%s",
                              mac ? "resolver interface MAC must be nonzero"
                                  : mac.error().c_str());
      }
      uint32_t ipv4 = 0;
      if (!entry.ipv4().empty()) {
        bess::utils::be32_t address;
        if (!bess::utils::ParseIpv4Address(entry.ipv4(), &address)) {
          return CommandFailure(EINVAL, "invalid resolver IPv4 address '%s'",
                                entry.ipv4().c_str());
        }
        ipv4 = address.value();
        if (ipv4 == 0 || ipv4 == 0xffffffffu) {
          return CommandFailure(EINVAL, "invalid resolver IPv4 address");
        }
      }
      bess::utils::Ipv6Address ipv6{};
      if (!entry.ipv6().empty()) {
        auto address = bess::utils::ParseIpv6Address(entry.ipv6());
        if (!address) {
          return CommandFailure(EINVAL, "invalid resolver IPv6 address '%s'",
                                entry.ipv6().c_str());
        }
        ipv6 = *address;
        if (ipv6.IsUnspecified() || ipv6.IsMulticast()) {
          return CommandFailure(EINVAL, "invalid resolver IPv6 address");
        }
      }
      if (ipv4 == 0 && ipv6.IsUnspecified()) {
        return CommandFailure(EINVAL,
                              "resolver interface needs an IPv4 or IPv6 address");
      }
      route::NeighborResolver::Interface interface{
          dataplane::InterfaceId(entry.id()), *mac, ipv4, ipv6};
      auto configured =
          neighbor_resolver_->ConfigureInterface(interface, 0);
      if (!configured) {
        return CommandFailure(EINVAL, "%s", configured.error().c_str());
      }
      resolver_gates_.emplace(entry.id(),
                              static_cast<gate_idx_t>(entry.egress_gate()));
    }
    for (const auto &binding : arg.neighbor_bindings()) {
      const auto matching_interface = std::find_if(
          arg.resolver_interfaces().begin(), arg.resolver_interfaces().end(),
          [&binding](const auto &interface) {
            return interface.id() == binding.interface_id();
          });
      if (matching_interface == arg.resolver_interfaces().end()) {
        return CommandFailure(EINVAL,
                              "neighbor binding names unknown interface %u",
                              binding.interface_id());
      }
      if (binding.next_hop_id() == 0 ||
          (binding.ipv4().empty() == binding.ipv6().empty())) {
        return CommandFailure(EINVAL, "invalid neighbor binding");
      }
      route::NeighborResolver::Key key;
      if (!binding.ipv4().empty()) {
        if (matching_interface->ipv4().empty()) {
          return CommandFailure(EINVAL,
                                "IPv4 neighbor binding requires local IPv4");
        }
        bess::utils::be32_t peer;
        if (!bess::utils::ParseIpv4Address(binding.ipv4(), &peer)) {
          return CommandFailure(EINVAL, "invalid neighbor IPv4 address '%s'",
                                binding.ipv4().c_str());
        }
        key = route::NeighborResolver::Key(
            dataplane::InterfaceId(binding.interface_id()), peer.value());
      } else {
        if (matching_interface->ipv6().empty()) {
          return CommandFailure(EINVAL,
                                "IPv6 neighbor binding requires local IPv6");
        }
        auto peer = bess::utils::ParseIpv6Address(binding.ipv6());
        if (!peer) {
          return CommandFailure(EINVAL, "invalid neighbor IPv6 address '%s'",
                                binding.ipv6().c_str());
        }
        key = route::NeighborResolver::Key(
            dataplane::InterfaceId(binding.interface_id()), *peer);
      }
      if (!bound_next_hops.insert(binding.next_hop_id()).second) {
        return CommandFailure(EINVAL, "duplicate neighbor binding next hop %u",
                              binding.next_hop_id());
      }
      auto hop = neighbor_resolver_->Bind(
          route::NextHopId(binding.next_hop_id()), key, 0);
      if (!hop) {
        return CommandFailure(EINVAL, "%s", hop.error().c_str());
      }
      if (auto set = router_->SetNextHop(route::NextHopId(binding.next_hop_id()),
                                         *hop);
          !set) {
        return CommandFailure(route::RouteErrno(set.error()), "%s",
                              route::RouteErrorName(set.error()));
      }
      if (auto managed = router_->MarkNeighborManagedNextHop(
              route::NextHopId(binding.next_hop_id()));
          !managed) {
        return CommandFailure(route::RouteErrno(managed.error()), "%s",
                              route::RouteErrorName(managed.error()));
      }
    }
    neighbor_interface_attr_name_ =
        arg.neighbor_interface_attr().empty()
            ? "neighbor_interface_id"
            : arg.neighbor_interface_attr();
  }
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
            auto kind = KindFromPb(value.kind());
            if (!kind) {
              return std::unexpected(kind.error());
            }
            hop.kind = *kind;
            hop.dst_mac = *dst;
            hop.src_mac = *src;
            // A local hop delivers to this node: it cannot name an adjacency,
            // and it names no interface (the wire's zero default is accepted,
            // as is the canonical DROP_GATE read-back). Anything else is a hop
            // that both forwards and delivers (D-178).
            if (hop.kind == route::NextHopKind::kLocal) {
              if (value.egress_gate() != 0 && value.egress_gate() != DROP_GATE) {
                return std::unexpected(
                    "a local next hop names no egress gate");
              }
              if (!hop.dst_mac.IsZero() || !hop.src_mac.IsZero()) {
                return std::unexpected(
                    "a local next hop names no MAC addresses");
              }
              hop.egress = bess::dataplane::kInvalidInterfaceId;
            }
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
            if (key.domain() >= route::kMaxRouteDomains) {
              return std::unexpected("invalid route domain " +
                                     std::to_string(key.domain()));
            }
            if (!key.ipv4().empty() && key.ipv6().empty()) {
              bess::utils::be32_t addr;
              if (!bess::utils::ParseIpv4Address(key.ipv4(), &addr)) {
                return std::unexpected("invalid IPv4 prefix address '" +
                                       key.ipv4() + "'");
              }
              if (key.prefix_length() > 32) {
                return std::unexpected("invalid IPv4 prefix length");
              }
              const auto prefix = route::Ipv4Prefix::Make(
                  addr.value(), static_cast<uint8_t>(key.prefix_length()));
              if (!prefix) {
                return std::unexpected(route::RouteErrorName(prefix.error()));
              }
              return route::Router::RouteKey(route::RouteDomainId(key.domain()),
                                             *prefix);
            }
            if (key.ipv4().empty() && !key.ipv6().empty()) {
              const auto addr = bess::utils::ParseIpv6Address(key.ipv6());
              if (!addr) {
                return std::unexpected("invalid IPv6 prefix address '" +
                                       key.ipv6() + "'");
              }
              if (key.prefix_length() > 128) {
                return std::unexpected("invalid IPv6 prefix length");
              }
              const auto prefix = route::Ipv6Prefix::Make(
                  *addr, static_cast<uint8_t>(key.prefix_length()));
              if (!prefix) {
                return std::unexpected(route::RouteErrorName(prefix.error()));
              }
              return route::Router::RouteKey(route::RouteDomainId(key.domain()),
                                             *prefix);
            }
            return std::unexpected("a route key must set exactly one of ipv4 or ipv6");
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
            router_->ForEachIpv6Route(
                [&visit](route::RouteDomainId domain,
                         route::Ipv6Prefix prefix, route::NextHopId hop,
                         route::NextHopGroupId group) {
                  bess::pb::RouterRouteKey key;
                  key.set_ipv6(bess::utils::ToIpv6Address(prefix.addr()));
                  key.set_prefix_length(prefix.length());
                  key.set_domain(domain.value());
                  bess::pb::RouterRouteValue value;
                  if (group.value() != 0) {
                    value.set_group_id(group.value());
                  } else {
                    value.set_next_hop_id(hop.value());
                  }
                  visit(key, value);
                });
          }));

  next_hop_attr_name_ = arg.next_hop_attr().empty() ? "next_hop_id" : arg.next_hop_attr();
  next_hop_id_attr_ = AddMetadataAttr(next_hop_attr_name_,
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
  // no configuration on either side. Declared with the default contract: the
  // framework resolves it at PreResume and the packet path reads the constant
  // without touching packet metadata when no provider exists.
  static constexpr std::array<uint8_t, sizeof(bess::utils::be32_t)>
      kDefaultRouteDomain{};
  domain_attr_name_ = arg.domain_attr().empty() ? "route_domain" : arg.domain_attr();
  route_domain_attr_ = AddMetadataAttrWithDefault(
      domain_attr_name_, sizeof(bess::utils::be32_t),
      bess::metadata::Attribute::AccessMode::kRead, kDefaultRouteDomain);
  if (route_domain_attr_ < 0) {
    auto released = router_->Release();
    CHECK(released) << released.error();
    next_hops_binding_.Reset();
    groups_binding_.Reset();
    routes_binding_.Reset();
    router_.reset();
    return CommandFailure(-route_domain_attr_, "add_metadata_attr() failed");
  }
  if (neighbor_resolver_ != nullptr) {
    static constexpr std::array<uint8_t, sizeof(bess::utils::be32_t)>
        kNoNeighborInterface{};
    neighbor_interface_attr_ = AddMetadataAttrWithDefault(
        neighbor_interface_attr_name_, sizeof(bess::utils::be32_t),
        bess::metadata::Attribute::AccessMode::kRead, kNoNeighborInterface);
    if (neighbor_interface_attr_ < 0) {
      return CommandFailure(-neighbor_interface_attr_,
                            "add_metadata_attr() failed");
    }
    static std::atomic<uint32_t> next_ring_id{0};
    const std::string ring_suffix =
        std::to_string(next_ring_id.fetch_add(1, std::memory_order_relaxed));
    const std::string rx_name = "rrx" + ring_suffix;
    const std::string tx_name = "rtx" + ring_suffix;
    neighbor_rx_ring_ =
        rte_ring_create(rx_name.c_str(), 1024, SOCKET_ID_ANY, 0);
    neighbor_tx_ring_ =
        rte_ring_create(tx_name.c_str(), 1024, SOCKET_ID_ANY, 0);
    if (neighbor_rx_ring_ == nullptr || neighbor_tx_ring_ == nullptr) {
      return CommandFailure(ENOMEM, "neighbor resolver ring creation failed");
    }
    if (RegisterTask(nullptr) == INVALID_TASK_ID) {
      return CommandFailure(ENOMEM, "neighbor resolver task creation failed");
    }
    neighbor_tick_ = bess::framework::TickEndpoint(
        init_context().ticks(), std::chrono::steady_clock::duration::zero(),
        [this](bess::framework::TickContext &ctx) { ResolverTick(ctx); });
  }
  return CommandSuccess();
}

void Router::CleanupNeighborResolver() {
  neighbor_tick_ = bess::framework::TickEndpoint{};
  if (neighbor_rx_ring_ != nullptr) {
    bess::PacketHandle handle = nullptr;
    while (rte_ring_dequeue(
               neighbor_rx_ring_,
               reinterpret_cast<void **>(&handle)) == 0) {
      bess::PacketFree(handle);
    }
    rte_ring_free(neighbor_rx_ring_);
    neighbor_rx_ring_ = nullptr;
  }
  if (neighbor_tx_ring_ != nullptr) {
    route::NeighborResolver::Solicitation *item = nullptr;
    while (rte_ring_dequeue(
               neighbor_tx_ring_,
               reinterpret_cast<void **>(&item)) == 0) {
      delete item;
    }
    rte_ring_free(neighbor_tx_ring_);
    neighbor_tx_ring_ = nullptr;
  }
  neighbor_resolver_.reset();
  resolver_gates_.clear();
  pending_neighbor_updates_.clear();
}

void Router::DeInit() {
  CleanupNeighborResolver();
  if (router_ == nullptr) {
    return;
  }
  // Classifier rules reference these next hops, so release rather than
  // unregister: the order modules are destroyed in must not decide whether a
  // pipeline can be torn down.
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
  if (neighbor_resolver_ != nullptr &&
      !bess::metadata::IsValidOffset(
          attr_offset(neighbor_interface_attr_))) {
    LOG(ERROR) << "Router '" << name() << "': the '"
               << neighbor_interface_attr_name_
               << "' attribute is unreadable; ARP/ND input will be dropped";
  }
  // Metadata offsets are assigned just before this: an attribute with no
  // valid offset means the packet path would read byte 0, which is worth
  // saying once, here, rather than never.
  if (routes_by_destination()) {
    const bess::metadata::mt_offset_t domain_offset =
        attr_offset(route_domain_attr_);
    if (bess::metadata::IsValidOffset(domain_offset)) {
      LOG(INFO) << "Router '" << name()
                << "': routing by destination address in the route domain read "
                   "from '"
                << domain_attr_name_ << "'";
    } else if (domain_offset == bess::metadata::kMetadataOffsetDefault) {
      LOG(INFO) << "Router '" << name()
                << "': routing by destination address in domain 0 (nothing "
                   "writes '"
                << domain_attr_name_ << "'; its declared default applies)";
    } else {
      LOG(ERROR) << "Router '" << name() << "': the '" << domain_attr_name_
                 << "' attribute is unreadable; routing by destination "
                    "address in domain 0";
    }
    return 0;
  }
  if (!bess::metadata::IsValidOffset(attr_offset(next_hop_id_attr_))) {
    LOG(ERROR) << "Router '" << name() << "': the '" << next_hop_attr_name_
               << "' attribute is unreadable (is anything writing it?); "
                  "dropping packets";
  }
  return 0;
}

void Router::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (neighbor_resolver_ != nullptr) {
    std::array<bess::PacketHandle, bess::PacketBatch::kMaxBurst> forwarded;
    size_t forwarded_count = 0;
    const int count = batch->cnt();
    for (int i = 0; i < count; i++) {
      bess::PacketRef pkt = batch->packet(i);
      const uint8_t *frame = pkt.head_data<const uint8_t *>();
      bool neighbor_frame = false;
      if (pkt.head_len() >= sizeof(bess::utils::Ethernet)) {
        const auto *eth =
            reinterpret_cast<const bess::utils::Ethernet *>(frame);
        if (eth->ether_type.value() == bess::utils::Ethernet::kArp) {
          neighbor_frame = true;
        } else if (eth->ether_type.value() == bess::utils::Ethernet::kIpv6 &&
                   pkt.head_len() >= sizeof(bess::utils::Ethernet) + 41) {
          const size_t l3 = sizeof(bess::utils::Ethernet);
          neighbor_frame =
              frame[l3 + 6] == route::kIpv6NextHeaderIcmpv6 &&
              (frame[l3 + 40] == static_cast<uint8_t>(
                                     route::NdType::kNeighborSolicitation) ||
               frame[l3 + 40] == static_cast<uint8_t>(
                                     route::NdType::kNeighborAdvertisement));
        }
      }
      if (!neighbor_frame) {
        forwarded[forwarded_count++] = pkt.handle();
        continue;
      }
      if (rte_ring_enqueue(neighbor_rx_ring_, pkt.handle()) != 0) {
        // Resolver input is bounded; a full ring fails closed rather than
        // forwarding an ARP/ND control frame as routed IP.
        bess::PacketFree(pkt.handle());
      }
    }
    batch->clear();
    for (size_t i = 0; i < forwarded_count; i++) {
      batch->add(bess::PacketRef(forwarded[i]));
    }
    if (batch->cnt() == 0) {
      return;
    }
  }
  if (!routes_by_destination()) {
    RouteById(ctx, batch, attr_offset(next_hop_id_attr_));
    return;
  }
  RouteByDestination(ctx, batch);
}

void Router::QueueNeighborUpdates(
    std::span<const std::pair<route::NextHopId, route::NextHop>> updates) {
  for (const auto &update : updates) {
    const auto it = std::find_if(
        pending_neighbor_updates_.begin(), pending_neighbor_updates_.end(),
        [&update](const auto &pending) { return pending.first == update.first; });
    if (it == pending_neighbor_updates_.end()) {
      pending_neighbor_updates_.push_back(update);
    } else {
      it->second = update.second;
    }
  }
}

void Router::ResolverTick(bess::framework::TickContext &ctx) {
  if (neighbor_resolver_ == nullptr) {
    return;
  }
  const uint64_t now = ctx.tick();
  constexpr unsigned kResolverWorkBudget = 64;
  const bess::metadata::mt_offset_t interface_offset =
      attr_offset(neighbor_interface_attr_);
  for (unsigned n = 0; n < kResolverWorkBudget && !ctx.Expired(); n++) {
    bess::PacketHandle handle = nullptr;
    if (rte_ring_dequeue(neighbor_rx_ring_,
                         reinterpret_cast<void **>(&handle)) != 0) {
      break;
    }
    bess::PacketRef pkt(handle);
    if (!bess::metadata::IsValidOffset(interface_offset) ||
        pkt.head_len() < sizeof(bess::utils::Ethernet)) {
      bess::PacketFree(handle);
      continue;
    }
    const uint32_t id =
        get_attr_with_offset<bess::utils::be32_t>(interface_offset, pkt)
            .value();
    if (id > std::numeric_limits<uint16_t>::max() ||
        !resolver_gates_.contains(id)) {
      bess::PacketFree(handle);
      continue;
    }
    const auto *frame = pkt.head_data<const uint8_t *>();
    const std::span<const uint8_t> bytes(frame, pkt.head_len());
    route::NeighborResolver::Progress progress;
    const auto *eth = reinterpret_cast<const bess::utils::Ethernet *>(frame);
    if (eth->ether_type.value() == bess::utils::Ethernet::kArp) {
      progress = neighbor_resolver_->OnArp(
          now, dataplane::InterfaceId(static_cast<uint16_t>(id)), bytes);
    } else {
      progress = neighbor_resolver_->OnNd(
          now, dataplane::InterfaceId(static_cast<uint16_t>(id)), bytes);
    }
    QueueNeighborUpdates(progress.updates);
    bess::PacketFree(handle);
  }

  if (!ctx.Expired()) {
    route::NeighborResolver::Progress progress =
        neighbor_resolver_->Poll(now, kResolverWorkBudget);
    QueueNeighborUpdates(progress.updates);
    for (const auto &solicitation : progress.solicitations) {
      if (!resolver_gates_.contains(solicitation.egress.value())) {
        continue;
      }
      auto *queued =
          new (std::nothrow) route::NeighborResolver::Solicitation(solicitation);
      if (queued == nullptr || rte_ring_enqueue(neighbor_tx_ring_, queued) != 0) {
        delete queued;
      }
    }
  }
  if (!pending_neighbor_updates_.empty()) {
    std::vector<dataplane::Op> ops;
    ops.reserve(pending_neighbor_updates_.size());
    for (const auto &[id, hop] : pending_neighbor_updates_) {
      ops.push_back(router_->SetNeighborManagedNextHopOp(id, hop));
    }
    auto applied = init_context().resources().Apply(ops);
    if (applied) {
      pending_neighbor_updates_.clear();
    } else {
      LOG(WARNING) << "Router '" << name()
                   << "': neighbor publication deferred: " << applied.error();
    }
  }
}

struct task_result Router::RunTask(Context *ctx, bess::PacketBatch *batch,
                                   void *) {
  batch->clear();
  if (neighbor_resolver_ == nullptr || neighbor_tx_ring_ == nullptr) {
    return {.block = true, .packets = 0, .bits = 0};
  }
  route::NeighborResolver::Solicitation *items[
      bess::PacketBatch::kMaxBurst] = {};
  const unsigned count = rte_ring_dequeue_burst(
      neighbor_tx_ring_, reinterpret_cast<void **>(items),
      bess::PacketBatch::kMaxBurst, nullptr);
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  uint64_t bits = 0;
  for (unsigned i = 0; i < count; i++) {
    route::NeighborResolver::Solicitation *item = items[i];
    const auto gate = resolver_gates_.find(item->egress.value());
    if (gate == resolver_gates_.end() || gate->second == DROP_GATE) {
      delete item;
      continue;
    }
    bess::PacketHandle handle =
        current_worker.packet_pool()->Alloc(item->length);
    if (handle == nullptr) {
      delete item;
      continue;
    }
    bess::PacketRef packet(handle);
    std::memcpy(packet.head_data<uint8_t *>(), item->frame.data(),
                item->length);
    bits += static_cast<uint64_t>(item->length) * 8;
    batch->add(packet);
    gates[batch->cnt() - 1] = gate->second;
    delete item;
  }
  const uint32_t emitted = static_cast<uint32_t>(batch->cnt());
  if (emitted != 0) {
    EmitBatch(ctx, batch, gates);
    // Solicitations to several interfaces leave through EmitPacket, which
    // records this module's gates in the shared Context. Task::operator()
    // flushes those only after a ProcessBatch, so a task module flushes its
    // own -- otherwise the next module's ProcessOGates reads these indexes
    // against its own gates.
    ProcessOGates(ctx);
  }
  return {.block = emitted == 0, .packets = emitted, .bits = bits};
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
    if (hop == nullptr) {
      gates[i] = DROP_GATE;
      continue;
    }
    // A local hop is delivery, not forwarding (D-178). This mode has no punt
    // gates (D-162): a caller that put a local hop behind a next-hop id gets
    // its packet dropped and counted, not forwarded. Destination mode is where
    // a graph names the local path.
    if (hop->kind == route::NextHopKind::kLocal) {
      local_frames_.fetch_add(1, std::memory_order_relaxed);
      gates[i] = DROP_GATE;
      continue;
    }
    if (hop->neighbor != route::NeighborState::kResolved) {
      gates[i] = DROP_GATE;
      continue;
    }
    gates[i] = GateOf(hop->egress);
    bess::PacketRef pkt = batch->packet(i);
    // The destination mode's write path, so the two modes cannot disagree
    // about a TTL boundary or about which addresses a hop writes (D-153): the
    // TTL first, then the addresses when the hop names both. A frame that is
    // not an IPv4 packet has no hop count to decrement: it keeps the TTL it
    // arrived with and, as before, still gets the hop's addresses when it
    // names them. A header the parser cannot read is dropped and counted,
    // never forwarded.
    bess::conntrack::ParsedFlowPacket parsed;
    const bess::conntrack::ParseStatus status = bess::conntrack::ParseFrame(
        std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()), parsed,
        pkt.total_len());
    if (status != bess::conntrack::ParseStatus::kOk &&
        status != bess::conntrack::ParseStatus::kNotIp &&
        status != bess::conntrack::ParseStatus::kFragment) {
      gates[i] = DROP_GATE;
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    bess::PacketHandle handle = pkt.handle();
    if (!bess::packet::EnsureWritable(handle)) {
      gates[i] = DROP_GATE;
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    batch->handles()[i] = handle;
    bess::PacketRef writable(handle);
    // A non-initial fragment still carries its IPv4 header; only its L4 header
    // is missing. So the hop count is every IPv4 frame's, fragments included:
    // the destination mode drops a fragment because its group hash needs ports,
    // while this mode routes by id and forwards it with the TTL written.
    if ((status == bess::conntrack::ParseStatus::kOk ||
         status == bess::conntrack::ParseStatus::kFragment) &&
        parsed.l3 == bess::conntrack::L3Kind::kIpv4) {
      // The parse ran before EnsureWritable, and the copy may have resegmented
      // the frame: where the header is is re-established, not assumed. Writing
      // it through a stale offset would touch bytes outside this segment.
      if (writable.head_len() <
          static_cast<size_t>(parsed.l3_offset) + sizeof(bess::utils::Ipv4)) {
        gates[i] = DROP_GATE;
        header_write_failures_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (route::WriteForwarded(
              std::span(writable.head_data<uint8_t *>(), writable.head_len()),
              parsed.l3_offset, *hop) != route::TtlResult::kForward) {
        gates[i] = DROP_GATE;
        ttl_expired_frames_.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (!route::RewriteL2(writable, *hop)) {
      gates[i] = DROP_GATE;
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  EmitBatch(ctx, batch, gates);
}

// One counter per verdict the chain reports (route/forward.h). kForward is not
// a drop; kLocalDelivery is a delivery (D-178); kNoEgress is the blackhole
// shape (D-160), counted apart from a miss because the route exists and chose
// that hop.
void Router::CountVerdict(route::ForwardVerdict verdict) {
  switch (verdict) {
    case route::ForwardVerdict::kLocalDelivery:
      local_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
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
namespace {
// The domain a packet is in: 24 bits wide (Router::RouteKey packs it above the
// prefix; route/route_domain.h), so no narrowing here -- a domain this router
// does not hold is looked up as itself and misses instead of aliasing onto
// another domain's FIB.
inline route::RouteDomainId DomainOf(
    const bess::metadata::ResolvedAttr<bess::utils::be32_t> &domain,
    bess::PacketRef pkt) {
  return route::RouteDomainId(domain.Get(pkt).value());
}
}  // namespace

void Router::RouteByDestination(Context *ctx, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  const bess::metadata::ResolvedAttr<bess::utils::be32_t> domain =
      ResolveAttr<bess::utils::be32_t>(route_domain_attr_);
  gate_idx_t gates[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    gates[i] = DROP_GATE;
  }
  // The lookup is one call for one domain's table (Router::ResolveBatch), so
  // frames are resolved a run at a time: a batch in one domain -- the common
  // case -- is one lookup for the whole burst, and a batch that mixes domains
  // (policy a controller writes) is resolved in runs, where a run of one is
  // the scalar path the module used before.
  int begin = 0;
  while (begin < cnt) {
    const route::RouteDomainId current =
        DomainOf(domain, batch->packet(begin));
    int end = begin + 1;
    while (end < cnt && end - begin < static_cast<int>(bess::PacketBatch::kMaxBurst) &&
           DomainOf(domain, batch->packet(end)) == current) {
      end++;
    }
    ResolveRun(batch, begin, end, current, gates);
    begin = end;
  }
  EmitBatch(ctx, batch, gates);
}

void Router::ResolveRun(bess::PacketBatch *batch, int begin, int end,
                        route::RouteDomainId domain, gate_idx_t *gates) {
  const size_t n = static_cast<size_t>(end - begin);
  // A run of one is the scalar path, and pays for no batch: a burst that hands
  // one packet, or a batch that splits at every domain boundary, must not cost
  // the batch half's arrays. A raw array, so nothing is constructed for the
  // elements a short run does not use; ResolveFrames writes every position it
  // is given, and the scalar path writes its one.
  route::FrameRoute resolved[bess::PacketBatch::kMaxBurst];
  if (n == 1) {
    bess::PacketRef pkt = batch->packet(begin);
    resolved[0] = route::ResolveFrame(
        *router_, domain, std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()),
        pkt.total_len(), flow_fields_);
  } else {
    std::array<std::span<const uint8_t>, bess::PacketBatch::kMaxBurst> frames;
    std::array<size_t, bess::PacketBatch::kMaxBurst> total_lens;
    for (size_t k = 0; k < n; k++) {
      bess::PacketRef pkt = batch->packet(begin + static_cast<int>(k));
      frames[k] = std::span(pkt.head_data<const uint8_t *>(), pkt.head_len());
      total_lens[k] = pkt.total_len();
    }
    route::ResolveFrames(*router_, domain, std::span(frames.data(), n),
                         std::span(total_lens.data(), n), std::span(resolved, n),
                         flow_fields_);
  }
  for (size_t k = 0; k < n; k++) {
    const int i = begin + static_cast<int>(k);
    const route::FrameRoute &r = resolved[k];
    // IPv6 shares the same domain, next-hop, neighbor, group and gate policy;
    // only its L3 header mutation differs (hop limit, with no header checksum).
    if (r.verdict == route::ForwardVerdict::kNotIpv4) {
      bess::PacketRef pkt = batch->packet(i);
      bess::conntrack::ParsedFlowPacket parsed;
      const auto status = bess::conntrack::ParseFrame(
          std::span(pkt.head_data<const uint8_t *>(), pkt.head_len()), parsed,
          pkt.total_len());
      if (status == bess::conntrack::ParseStatus::kOk &&
          parsed.l3 == bess::conntrack::L3Kind::kIpv6) {
        bess::utils::Ipv6Address dst;
        std::copy(parsed.dst.begin(), parsed.dst.end(), dst.bytes);
        uint32_t src_words[4], dst_words[4];
        std::memcpy(src_words, parsed.src.data(), sizeof(src_words));
        std::memcpy(dst_words, parsed.dst.data(), sizeof(dst_words));
        const uint32_t hash = route::FlowHash(
            src_words[0] ^ src_words[2] ^ src_words[1] ^ src_words[3],
            dst_words[0] ^ dst_words[2] ^ dst_words[1] ^ dst_words[3],
            parsed.src_port, parsed.dst_port, parsed.protocol, flow_fields_);
        const route::NextHop *hop = router_->Resolve(domain, dst, hash);
        route::ForwardVerdict verdict = route::ForwardVerdict::kForward;
        if (hop == nullptr) {
          verdict = route::ForwardVerdict::kNoRoute;
        } else if (hop->egress == bess::dataplane::kInvalidInterfaceId) {
          verdict = route::ForwardVerdict::kNoEgress;
        } else if (hop->neighbor != route::NeighborState::kResolved) {
          verdict = route::ForwardVerdict::kUnresolved;
        } else if (pkt.head_len() <= static_cast<size_t>(parsed.l3_offset) + 7 ||
                   pkt.head_data<const uint8_t *>()[parsed.l3_offset + 7] <= 1) {
          verdict = route::ForwardVerdict::kTtlExpired;
        }
        if (verdict != route::ForwardVerdict::kForward) {
          CountVerdict(verdict);
          const gate_idx_t punt =
              verdict == route::ForwardVerdict::kNoRoute
                  ? punt_miss_gate_
                  : verdict == route::ForwardVerdict::kUnresolved
                        ? punt_unresolved_gate_
                        : DROP_GATE;
          if (punt != DROP_GATE) {
            gates[i] = punt;
          }
          continue;
        }
        const gate_idx_t gate = GateOf(hop->egress);
        if (gate == DROP_GATE) {
          continue;
        }
        bess::PacketHandle handle = pkt.handle();
        if (!bess::packet::EnsureWritable(handle)) {
          header_write_failures_.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        batch->handles()[i] = handle;
        bess::PacketRef writable(handle);
        if (writable.head_len() <= static_cast<size_t>(parsed.l3_offset) + 7) {
          header_write_failures_.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        uint8_t *frame = writable.head_data<uint8_t *>();
        --frame[parsed.l3_offset + 7];
        if (!hop->dst_mac.IsZero() && !hop->src_mac.IsZero()) {
          auto *eth = reinterpret_cast<bess::utils::Ethernet *>(frame);
          eth->dst_addr = hop->dst_mac;
          eth->src_addr = hop->src_mac;
        }
        gates[i] = gate;
        continue;
      }
    }
    if (r.verdict != route::ForwardVerdict::kForward) {
      CountVerdict(r.verdict);
      // A punt gate is the application's slow path (D-162): a frame the
      // destination path cannot forward leaves on it instead of being
      // dropped, and it is counted either way, so the counters stay the
      // record of what happened. A local route's frame is not a failure: it is
      // delivered to this node's local path, and its gate is the one the graph
      // gave for that (D-178).
      const gate_idx_t punt =
          r.verdict == route::ForwardVerdict::kNoRoute
              ? punt_miss_gate_
              : r.verdict == route::ForwardVerdict::kUnresolved
                    ? punt_unresolved_gate_
                    : r.verdict == route::ForwardVerdict::kLocalDelivery
                          ? punt_local_gate_
                          : DROP_GATE;
      if (punt != DROP_GATE) {
        gates[i] = punt;
      }
      continue;
    }
    const gate_idx_t gate = GateOf(r.egress);
    if (gate == DROP_GATE) {
      // A hop whose egress is no gate of this graph: configuration, not
      // traffic, so nothing is counted and nothing is written.
      continue;
    }

    // The TTL is written whatever the hop names, so the header must be
    // writable: a shared payload is copied once (K4) and the copy replaces
    // the batch's handle.
    bess::PacketRef pkt = batch->packet(i);
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
        static_cast<size_t>(r.l3_offset) + sizeof(bess::utils::Ipv4)) {
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (route::WriteForwarded(
            std::span(writable.head_data<uint8_t *>(), writable.head_len()), r.l3_offset,
            *r.hop) != route::TtlResult::kForward) {
      // ResolveFrames refuses a TTL of 0 or 1, so this is unreachable with the
      // same bytes; counting it keeps the boundary honest if a mutation ever
      // rewrote the header between the two calls.
      ttl_expired_frames_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    gates[i] = gate;
  }
}

std::string Router::GetDesc() const {
  std::string desc;
  if (!routes_by_destination()) {
    desc = bess::utils::Format(
        "%zu routes, %zu next hops, %zu groups, %zu header write failures, %zu local, "
        "next-hop id from metadata",
        router_->route_count(), router_->next_hop_count(),
        router_->next_hop_group_count(),
        static_cast<size_t>(header_write_failures()),
        static_cast<size_t>(local_frames()));
  } else {
    desc = bess::utils::Format(
        "%zu routes, %zu next hops, %zu groups, %zu header write failures, routing by "
        "destination (not ipv4: %zu, malformed: %zu, non-initial fragments: %zu, "
        "no route: %zu, unresolved: %zu, ttl expired: %zu, blackholed: %zu, local: %zu)",
        router_->route_count(), router_->next_hop_count(),
        router_->next_hop_group_count(),
        static_cast<size_t>(header_write_failures()),
        static_cast<size_t>(not_ipv4_frames()), static_cast<size_t>(malformed_frames()),
        static_cast<size_t>(non_initial_fragments()), static_cast<size_t>(no_route_frames()),
        static_cast<size_t>(unresolved_frames()),
        static_cast<size_t>(ttl_expired_frames()),
        static_cast<size_t>(blackholed_frames()),
        static_cast<size_t>(local_frames()));
    // A punt is configuration a reader has to know about (D-162): without it,
    // the counters above mean "dropped". Only the destination path has punt
    // gates -- the next-hop-id path forwards what the attribute names and
    // drops everything else -- so only its desc mentions them.
    if (punt_miss_gate_ != DROP_GATE) {
      desc += bess::utils::Format(", a miss punts to gate %d", punt_miss_gate_);
    }
    if (punt_unresolved_gate_ != DROP_GATE) {
      desc += bess::utils::Format(", an unresolved neighbor punts to gate %d",
                                  punt_unresolved_gate_);
    }
    if (punt_local_gate_ != DROP_GATE) {
      desc += bess::utils::Format(", a local address punts to gate %d",
                                  punt_local_gate_);
    }
  }
  return desc;
}

ADD_MODULE(Router, "router",
           "forwards to the egress its next-hop id names, or -- with no "
           "next_hop_id writer -- routes each packet by its destination "
           "address in its route domain; writes a resolved next hop's "
           "addresses into the frame (routes and next hops as transactional "
           "resources)")
