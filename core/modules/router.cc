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
#include "runtime/runtime_state.h"
#include "utils/endian.h"
#include "utils/ether.h"
#include "utils/format.h"
#include "utils/ip.h"

namespace dataplane = bess::dataplane;
namespace route = bess::route;

// DPDK 25.11's rte_ring_elem.h has no `extern "C"` of its own (rte_ring.h
// includes it before opening its block), so its out-of-line functions name
// C++-mangled symbols the library does not define; as handoff.cc and
// packet_store.cc do, this reaches the C function under another name. The
// element ring's enqueue/dequeue are inline and need no alias.
extern "C" rte_ring *BessRouterRingCreateElem(const char *name, unsigned esize,
                                              unsigned count, int socket_id,
                                              unsigned flags)
    __asm__("rte_ring_create_elem");

// The tables are resources (D-129); get_neighbors is the class's one action,
// a read of control-side resolver state, which the control plane's lock
// serialises with the maintenance tick -- so it needs no paused workers.
const Commands Router::cmds = {
    {"get_neighbors", "EmptyArg", MODULE_CMD_FUNC(&Router::CommandGetNeighbors),
     Command::THREAD_SAFE, "RouterCommandGetNeighborsResponse"},
};

const ResourceTables Router::resource_tables = {
    ResourceTable::Map<bess::pb::RouterNextHopIdKey,
                       bess::pb::RouterNextHopValue>("next_hops"),
    ResourceTable::Map<bess::pb::RouterNextHopIdKey,
                       bess::pb::RouterNeighborBinding>("neighbors"),
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

// One RouterInterface address: "<address>/<prefix length>", IPv4 or IPv6. The
// prefix length is required (a host address is /32 or /128), so an on-link
// prefix is never guessed; the resolver refuses what an interface cannot hold.
std::expected<route::NeighborResolver::InterfaceAddress, std::string>
InterfaceAddressFromPb(const std::string &text) {
  const size_t slash = text.find('/');
  if (slash == std::string::npos || slash + 1 == text.size() ||
      text.size() - slash - 1 > 3) {
    return std::unexpected("address '" + text +
                           "' is not in CIDR form (address/prefix length)");
  }
  const std::string address = text.substr(0, slash);
  unsigned length = 0;
  for (size_t i = slash + 1; i < text.size(); i++) {
    if (text[i] < '0' || text[i] > '9') {
      return std::unexpected("address '" + text + "' has an invalid prefix length");
    }
    length = length * 10 + static_cast<unsigned>(text[i] - '0');
  }
  route::NeighborResolver::InterfaceAddress out;
  bess::utils::be32_t ipv4;
  if (bess::utils::ParseIpv4Address(address, &ipv4)) {
    out.address = route::NeighborAddress::Ipv4(ipv4.value());
    if (length > 32) {
      return std::unexpected("address '" + text + "' has an invalid prefix length");
    }
  } else if (const auto ipv6 = bess::utils::ParseIpv6Address(address)) {
    out.address = route::NeighborAddress::Ipv6(*ipv6);
    if (length > 128) {
      return std::unexpected("address '" + text + "' has an invalid prefix length");
    }
  } else {
    return std::unexpected("invalid address '" + text + "'");
  }
  out.prefix_length = static_cast<uint8_t>(length);
  return out;
}

// A RouterNeighborBinding as the library's key: the interface (16 bits, as
// InterfaceOf maps gates) and exactly one address.
std::expected<route::NeighborKey, std::string> NeighborKeyFromPb(
    const bess::pb::RouterNeighborBinding &value) {
  if (value.interface_id() == 0 || value.interface_id() > 0xffffu) {
    return std::unexpected("invalid interface id " +
                           std::to_string(value.interface_id()));
  }
  const dataplane::InterfaceId interface(static_cast<uint16_t>(value.interface_id()));
  if (value.ipv4().empty() == value.ipv6().empty()) {
    return std::unexpected("a neighbor binding sets exactly one of ipv4 or ipv6");
  }
  if (!value.ipv4().empty()) {
    bess::utils::be32_t address;
    if (!bess::utils::ParseIpv4Address(value.ipv4(), &address)) {
      return std::unexpected("invalid neighbor IPv4 address '" + value.ipv4() + "'");
    }
    return route::NeighborKey(interface, address.value());
  }
  const auto address = bess::utils::ParseIpv6Address(value.ipv6());
  if (!address) {
    return std::unexpected("invalid neighbor IPv6 address '" + value.ipv6() + "'");
  }
  return route::NeighborKey(interface, *address);
}

bess::pb::RouterNeighborBinding NeighborKeyToPb(const route::NeighborKey &key) {
  bess::pb::RouterNeighborBinding value;
  value.set_interface_id(key.interface.value());
  if (key.address.is_ipv4()) {
    value.set_ipv4(bess::utils::ToIpv4Address(bess::utils::be32_t(key.address.ipv4())));
  } else {
    value.set_ipv6(bess::utils::ToIpv6Address(key.address.ipv6()));
  }
  return value;
}

bess::pb::RouterNeighborState NeighborStateToPb(route::NeighborState state) {
  switch (state) {
    case route::NeighborState::kResolved:
      return bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED;
    case route::NeighborState::kIncomplete:
      return bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE;
    case route::NeighborState::kUnreachable:
      return bess::pb::ROUTER_NEIGHBOR_STATE_UNREACHABLE;
  }
  return bess::pb::ROUTER_NEIGHBOR_STATE_UNSPECIFIED;
}

}  // namespace

using bess::modules::router::GateOf;
using bess::modules::router::InterfaceOf;

CommandResponse Router::Init(const bess::pb::RouterArg &arg) {
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
      !bess::IsValidGateValue(arg.punt_local_gate()) ||
      !bess::IsValidGateValue(arg.punt_neighbor_gate())) {
    return CommandFailure(EINVAL, "invalid punt gate %llu/%llu/%llu/%llu",
                          static_cast<unsigned long long>(arg.punt_miss_gate()),
                          static_cast<unsigned long long>(arg.punt_unresolved_gate()),
                          static_cast<unsigned long long>(arg.punt_local_gate()),
                          static_cast<unsigned long long>(arg.punt_neighbor_gate()));
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
  // Unlike the others, 0 is not "drop": it follows the local gate, where
  // neighbor requests went before they had a gate of their own.
  punt_neighbor_gate_ = arg.punt_neighbor_gate() == 0
                            ? punt_local_gate_
                            : static_cast<gate_idx_t>(arg.punt_neighbor_gate());
  // The flow hash's fields (D-163): the proto default is the whole flow, and an
  // unknown value (an arg from a newer binary) keeps that default too.
  flow_fields_ = arg.flow_hash() == bess::pb::FLOW_HASH_L3 ? route::FlowHashFields::kL3
                                                           : route::FlowHashFields::kL3L4;
  // Address families (D-183). A next-hop-id router has no FIB: it forwards
  // what the id names, of either family, so it takes no family setting and
  // refuses every field that only a FIB (routes, groups, domains, the hash a
  // group route uses, the punts of a lookup) would ever read.
  route::Router::Config config;
  if (!routes_by_destination()) {
    constexpr char kNoFib[] =
        "%s: a ROUTE_MODE_NEXT_HOP_ID router has no FIB";
    if (arg.has_max_ipv4_routes() || arg.has_max_ipv4_tbl8s() ||
        arg.has_max_ipv6_routes() || arg.has_max_ipv6_tbl8s()) {
      return CommandFailure(EINVAL, kNoFib,
                            "max_ipv4_routes, max_ipv4_tbl8s, max_ipv6_routes and "
                            "max_ipv6_tbl8s size the FIBs of ROUTE_MODE_DESTINATION");
    }
    if (arg.backend() != bess::pb::ROUTE_BACKEND_LPM) {
      return CommandFailure(EINVAL, kNoFib,
                            "backend selects the IPv4 FIB of ROUTE_MODE_DESTINATION");
    }
    if (arg.max_groups() != 0) {
      return CommandFailure(EINVAL, kNoFib,
                            "max_groups sizes the groups routes name in "
                            "ROUTE_MODE_DESTINATION");
    }
    if (arg.max_domains() > 1) {
      return CommandFailure(EINVAL, kNoFib,
                            "max_domains sizes the route domains of "
                            "ROUTE_MODE_DESTINATION");
    }
    if (arg.flow_hash() != bess::pb::FLOW_HASH_L3_L4) {
      return CommandFailure(EINVAL, kNoFib,
                            "flow_hash picks a group member for a route in "
                            "ROUTE_MODE_DESTINATION");
    }
    if (!arg.domain_attr().empty()) {
      return CommandFailure(EINVAL, kNoFib,
                            "domain_attr names the route domain "
                            "ROUTE_MODE_DESTINATION looks up");
    }
    if (arg.punt_miss_gate() != 0 || arg.punt_unresolved_gate() != 0) {
      return CommandFailure(EINVAL, kNoFib,
                            "punt_miss_gate and punt_unresolved_gate take what a "
                            "ROUTE_MODE_DESTINATION lookup cannot forward");
    }
    // No route value to fit: the id is the 32-bit metadata value.
    if (arg.max_next_hops() > route::Router::kMaxNextHopsWithoutFib) {
      return CommandFailure(EINVAL, "router capacity exceeds backend limits");
    }
  } else {
    // A FIB value is 24 bits and holds max_next_hops + group id (D-065).
    if (arg.max_next_hops() > route::LpmRouteTable::kMaxValue ||
        arg.max_groups() > route::LpmRouteTable::kMaxValue -
                               (arg.max_next_hops() ? arg.max_next_hops() : 4096)) {
      return CommandFailure(EINVAL, "router capacity exceeds backend limits");
    }
    // A family is enabled exactly when its route capacity is set, and sized
    // independently of the other. A disabled family has no FIB in any
    // domain, so it costs nothing.
    std::string family_error;
    auto capacity = [&family_error](bool has_routes, uint64_t routes, bool has_tbl8s,
                                     uint64_t tbl8s, uint64_t default_tbl8s,
                                     const char *family)
        -> std::optional<std::pair<uint32_t, uint32_t>> {
      if (!has_routes) {
        if (has_tbl8s) {
          family_error = bess::utils::Format(
              "max_%s_tbl8s needs max_%s_routes (unset routes disable %s)", family,
              family, family);
        }
        return std::nullopt;
      }
      if (routes == 0 || (has_tbl8s && tbl8s == 0)) {
        family_error = bess::utils::Format(
            "max_%s_routes and max_%s_tbl8s must be nonzero (leave max_%s_routes "
            "unset to disable %s)",
            family, family, family, family);
        return std::nullopt;
      }
      const uint64_t groups = has_tbl8s ? tbl8s : default_tbl8s;
      if (routes > std::numeric_limits<uint32_t>::max() ||
          groups > std::numeric_limits<uint32_t>::max()) {
        family_error = bess::utils::Format("%s capacity exceeds backend limits", family);
        return std::nullopt;
      }
      return std::make_pair(static_cast<uint32_t>(routes), static_cast<uint32_t>(groups));
    };
    const auto ipv4 = capacity(arg.has_max_ipv4_routes(), arg.max_ipv4_routes(),
                               arg.has_max_ipv4_tbl8s(), arg.max_ipv4_tbl8s(),
                               kDefaultIpv4Tbl8s, "ipv4");
    const auto ipv6 = capacity(arg.has_max_ipv6_routes(), arg.max_ipv6_routes(),
                               arg.has_max_ipv6_tbl8s(), arg.max_ipv6_tbl8s(),
                               kDefaultIpv6Tbl8s, "ipv6");
    if (!family_error.empty()) {
      return CommandFailure(EINVAL, "%s", family_error.c_str());
    }
    if (!ipv4 && !ipv6) {
      return CommandFailure(EINVAL,
                            "no address family is enabled: ROUTE_MODE_DESTINATION "
                            "needs max_ipv4_routes, max_ipv6_routes or both");
    }
    if (ipv4) {
      route::LpmRouteTable::Config table;
      table.max_routes = ipv4->first;
      table.tbl8_groups = ipv4->second;
      switch (arg.backend()) {
        case bess::pb::ROUTE_BACKEND_LPM:
          table.backend = route::RouteBackend::kRteLpm;
          break;
        case bess::pb::ROUTE_BACKEND_FIB:
          table.backend = route::RouteBackend::kRteFib;
          break;
        default:
          return CommandFailure(EINVAL, "unknown route backend %d", arg.backend());
      }
      config.ipv4 = table;
    } else if (arg.backend() != bess::pb::ROUTE_BACKEND_LPM) {
      return CommandFailure(EINVAL,
                            "backend selects the IPv4 table, and IPv4 is not enabled");
    }
    if (ipv6) {
      route::Ipv6RouteTable::Config table;
      table.max_routes = ipv6->first;
      table.tbl8_groups = ipv6->second;
      config.ipv6 = table;
    }
  }
  const size_t max_next_hops =
      arg.max_next_hops() ? static_cast<size_t>(arg.max_next_hops()) : 4096;
  const size_t max_groups =
      arg.max_groups() ? static_cast<size_t>(arg.max_groups()) : 0;

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
  if (arg.interfaces_size() != 0) {
    // A binding is a next hop, so there are never more than max_next_hops:
    // the default is 1024 or that, whichever is smaller.
    if (arg.max_neighbors() > max_next_hops) {
      return CommandFailure(EINVAL,
                            "max_neighbors is out of range (at most max_next_hops)");
    }
    const size_t resolver_capacity =
        arg.max_neighbors() == 0 ? std::min<size_t>(1024, max_next_hops)
                                 : static_cast<size_t>(arg.max_neighbors());
    // The resolver runs on the maintenance loop's ticks; with the loop off
    // (--maintenance_interval_us=0) no neighbor would ever be solicited and
    // every bound hop would stay unresolved, so refuse rather than blackhole.
    const std::chrono::microseconds tick =
        init_context().ticks().interval();
    if (tick.count() <= 0) {
      return CommandFailure(EINVAL,
                            "neighbor resolution needs the maintenance loop, "
                            "which is disabled (--maintenance_interval_us=0)");
    }
    // Durations are configured in milliseconds and kept in ticks: rounded up,
    // so a nonzero duration is at least one tick and never less than
    // configured. The rounding is a quotient plus a remainder test, which
    // cannot overflow where adding tick_us - 1 to the microseconds could.
    const uint64_t tick_us = static_cast<uint64_t>(tick.count());
    std::string timing_error;
    auto ms_of = [&](bool present, uint64_t ms, uint64_t absent_ms,
                     bool zero_allowed, const char *field) -> uint64_t {
      const uint64_t value = present ? ms : absent_ms;
      if (!timing_error.empty()) {
        return value;  // the first error is the one reported
      }
      if (value == 0 && !zero_allowed) {
        timing_error = std::string(field) + " must be nonzero";
      } else if (value > std::numeric_limits<uint64_t>::max() / 1000) {
        timing_error = std::string(field) + " is out of range";
      }
      return value;
    };
    auto to_ticks = [tick_us](uint64_t ms) -> uint64_t {
      const uint64_t us = ms * 1000;  // ms_of bounded it
      return us / tick_us + (us % tick_us != 0 ? 1 : 0);
    };
    const uint64_t first_probe_ms =
        ms_of(arg.has_neighbor_first_probe_ms(), arg.neighbor_first_probe_ms(),
              1000, true, "neighbor_first_probe_ms");
    const uint64_t probe_interval_ms = ms_of(
        arg.has_neighbor_probe_interval_ms(), arg.neighbor_probe_interval_ms(),
        1000, false, "neighbor_probe_interval_ms");
    const uint64_t max_probe_interval_ms =
        ms_of(arg.has_neighbor_max_probe_interval_ms(),
              arg.neighbor_max_probe_interval_ms(), 8000, false,
              "neighbor_max_probe_interval_ms");
    const uint64_t reachable_lifetime_ms =
        ms_of(arg.has_neighbor_reachable_lifetime_ms(),
              arg.neighbor_reachable_lifetime_ms(), 30000, true,
              "neighbor_reachable_lifetime_ms");
    const uint64_t unreachable_retry_ms =
        ms_of(arg.has_neighbor_unreachable_retry_ms(),
              arg.neighbor_unreachable_retry_ms(), 8000, true,
              "neighbor_unreachable_retry_ms");
    if (!timing_error.empty()) {
      return CommandFailure(EINVAL, "%s", timing_error.c_str());
    }
    // Compared as configured: rounding both up to ticks could make two
    // different values equal, and hide an inverted pair.
    if (max_probe_interval_ms < probe_interval_ms) {
      return CommandFailure(EINVAL,
                            "neighbor_max_probe_interval_ms is below "
                            "neighbor_probe_interval_ms");
    }
    const uint64_t first_probe = to_ticks(first_probe_ms);
    const uint64_t probe_interval = to_ticks(probe_interval_ms);
    const uint64_t max_probe_interval = to_ticks(max_probe_interval_ms);
    const uint64_t reachable_lifetime = to_ticks(reachable_lifetime_ms);
    const uint64_t unreachable_retry = to_ticks(unreachable_retry_ms);
    const uint64_t max_probes =
        arg.has_neighbor_max_probes() ? arg.neighbor_max_probes() : 3;
    if (max_probes == 0 || max_probes > std::numeric_limits<uint32_t>::max()) {
      return CommandFailure(EINVAL, "neighbor_max_probes is out of range");
    }
    route::NeighborResolver::Config resolver_config;
    resolver_config.capacity = resolver_capacity;
    resolver_config.timing = {
        .first_probe = first_probe,
        .probe_interval = probe_interval,
        .max_probe_interval = max_probe_interval,
        .max_probes = static_cast<uint32_t>(max_probes),
        // An explicit 0 is the library's "disabled" (no aging; no retry
        // after a failure).
        .reachable_lifetime = reachable_lifetime,
        .unreachable_retry = unreachable_retry,
    };
    // The resolver's clock is the maintenance tick count, which runs from
    // daemon start: start it at the current tick.
    const uint64_t now = init_context().ticks().current_tick();
    auto resolver = route::NeighborResolver::Create(resolver_config, now);
    if (!resolver) {
      return CommandFailure(EINVAL, "%s", resolver.error().c_str());
    }
    neighbor_resolver_ = std::move(*resolver);
    // The interfaces are configuration, fixed here; the bindings are the
    // "<module>/neighbors" resource, which the policy below admits.
    for (const auto &entry : arg.interfaces()) {
      if (!bess::IsValidGateValue(entry.egress_gate())) {
        return CommandFailure(EINVAL, "interface egress_gate %u is invalid",
                              entry.egress_gate());
      }
      // 0 derives the id from the gate; any other value must agree with it.
      const uint32_t id = InterfaceOf(entry.egress_gate()).value();
      if ((entry.id() != 0 && entry.id() != id) ||
          resolver_gates_.contains(id)) {
        return CommandFailure(EINVAL,
                              "interface id must be unique and equal "
                              "to egress_gate + 1 (or 0 to derive it)");
      }
      bess::utils::Ethernet::Address mac;
      if (!mac.FromString(entry.mac_addr())) {
        return CommandFailure(EINVAL, "invalid interface mac_addr '%s'",
                              entry.mac_addr().c_str());
      }
      if (mac.IsZero()) {
        return CommandFailure(EINVAL, "interface mac_addr must be nonzero");
      }
      route::NeighborResolver::Interface interface{dataplane::InterfaceId(id), mac,
                                                   {}};
      for (const std::string &text : entry.addresses()) {
        auto address = InterfaceAddressFromPb(text);
        if (!address) {
          return CommandFailure(EINVAL, "interface %u: %s", id,
                                address.error().c_str());
        }
        const bool v4 = address->address.is_ipv4();
        // Destination mode routes only its enabled families (D-183); the
        // next-hop-id mode forwards both, so it takes addresses of either.
        if (routes_by_destination() &&
            (v4 ? !router_->has_ipv4() : !router_->has_ipv6())) {
          return CommandFailure(EINVAL,
                                "interface %u: address '%s' is of a family the "
                                "router does not enable (set max_%s_routes)",
                                id, text.c_str(), v4 ? "ipv4" : "ipv6");
        }
        (v4 ? arp_input_ : nd_input_) = true;
        interface.addresses.push_back(*address);
      }
      if (interface.addresses.empty()) {
        return CommandFailure(EINVAL, "interface %u needs at least one address",
                              id);
      }
      auto configured = neighbor_resolver_->ConfigureInterface(interface, now);
      if (!configured) {
        return CommandFailure(EINVAL, "interface %u: %s", id,
                              configured.error().c_str());
      }
      resolver_gates_.emplace(id, static_cast<gate_idx_t>(entry.egress_gate()));
    }
    // A binding must be one the resolver would take (its interface, an
    // address of its family there, not one of the interface's own): checked
    // when a transaction reserves it, so the tick that takes it up cannot be
    // refused. The capacity is max_neighbors bindings, so the resolver's own
    // table, sized the same, never fills.
    bess::route::NeighborBindingPolicy policy;
    policy.capacity = resolver_capacity;
    policy.validate = [this](const route::NeighborKey &key)
        -> std::expected<void, std::string> {
      if (routes_by_destination() &&
          (key.address.is_ipv4() ? !router_->has_ipv4() : !router_->has_ipv6())) {
        return std::unexpected(key.address.is_ipv4()
                                   ? "an IPv4 neighbor on a router without IPv4"
                                   : "an IPv6 neighbor on a router without IPv6");
      }
      if (neighbor_resolver_ == nullptr) {
        return std::unexpected("neighbor resolution is shut down");
      }
      return neighbor_resolver_->CheckBind(key);
    };
    if (auto set = router_->SetNeighborBindingPolicy(std::move(policy)); !set) {
      return CommandFailure(route::RouteErrno(set.error()), "%s",
                            route::RouteErrorName(set.error()));
    }
    ingress_interface_attr_name_ = arg.ingress_interface_attr().empty()
                                       ? "vif_id"
                                       : arg.ingress_interface_attr();
  }
  if (auto enrolled = router_->Enroll(init_context().resources());
      !enrolled) {
    next_hops_binding_.Reset();
    neighbors_binding_.Reset();
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
          // The read-back is desired state (D-129): the static next hops. A
          // bound one is "<module>/neighbors"' entry (its key and its
          // binding read back there), and its state is learned, not desired
          // (D-183), so it is not listed here: a list of static hops neither
          // erases nor reports it, and one that names it is refused.
          [this](const auto &visit) {
            router_->ForEachNextHop(
                [this, &visit](route::NextHopId id, const route::NextHop &hop) {
                  if (router_->IsNeighborBound(id)) {
                    return;
                  }
                  bess::pb::RouterNextHopIdKey key;
                  key.set_id(id.value());
                  visit(key, NextHopToPb(hop));
                });
          }));
  neighbors_binding_ = init_context().codecs().Bind(
      *router_->neighbors_resource_object(),
      std::make_shared<bess::framework::TypedCodec<bess::pb::RouterNextHopIdKey,
                                             bess::pb::RouterNeighborBinding>>(
          [](const bess::pb::RouterNextHopIdKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            if (key.id() == 0) {
              return std::unexpected("next hop id 0 is invalid");
            }
            return dataplane::EncodeKey(route::NextHopId(key.id()));
          },
          [](const bess::pb::RouterNeighborBinding &value)
              -> std::expected<std::any, std::string> {
            return NeighborKeyFromPb(value).transform(
                [](const route::NeighborKey &key) { return std::any(key); });
          },
          // The bindings, as written (D-129): the state the router learned
          // for each is the get_neighbors command's, not desired state.
          [this](const auto &visit) {
            router_->ForEachNeighborBinding(
                [&visit](route::NextHopId id, const route::NeighborKey &neighbor) {
                  bess::pb::RouterNextHopIdKey key;
                  key.set_id(id.value());
                  visit(key, NeighborKeyToPb(neighbor));
                });
          }));
  groups_binding_ = init_context().codecs().Bind(
      *router_->groups_resource_object(),
      std::make_shared<bess::framework::TypedCodec<bess::pb::RouterGroupIdKey,
                                             bess::pb::RouterGroupValue>>(
          [destination = routes_by_destination()](const bess::pb::RouterGroupIdKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            // Only a route names a group, and the next-hop-id mode has no
            // routes (D-183): a group there could never be reached.
            if (!destination) {
              return std::unexpected(
                  "groups are not used in ROUTE_MODE_NEXT_HOP_ID (only a route "
                  "names a group, and the router has no FIB); use "
                  "ROUTE_MODE_DESTINATION");
            }
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
          [destination = routes_by_destination()](const bess::pb::RouterRouteKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            // The next-hop-id mode holds no FIB (D-183): a route there is a
            // configuration error, said so rather than "family disabled".
            if (!destination) {
              return std::unexpected(
                  "routes are not used in ROUTE_MODE_NEXT_HOP_ID (the router "
                  "has no FIB); route by destination with ROUTE_MODE_DESTINATION");
            }
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

  // Each mode declares only the attribute it reads (D-183). A declared read is
  // a requirement on every upstream path: a destination-mode Router that
  // declared `next_hop_id` would invalidate that attribute's scope for the
  // other readers in the graph wherever its own input (ARP/ND, a VIF) has no
  // writer, and a next-hop-mode Router has no use for the route domain.
  next_hop_attr_name_ = arg.next_hop_attr().empty() ? "next_hop_id" : arg.next_hop_attr();
  domain_attr_name_ = arg.domain_attr().empty() ? "route_domain" : arg.domain_attr();
  if (!routes_by_destination()) {
    next_hop_id_attr_ = AddMetadataAttr(next_hop_attr_name_,
                                        sizeof(bess::utils::be32_t),
                                        bess::metadata::Attribute::AccessMode::kRead);
    if (next_hop_id_attr_ < 0) {
      auto released = router_->Release();
      CHECK(released) << released.error();
      next_hops_binding_.Reset();
      neighbors_binding_.Reset();
      groups_binding_.Reset();
      routes_binding_.Reset();
      router_.reset();
      return CommandFailure(-next_hop_id_attr_, "add_metadata_attr() failed");
    }
  } else {
    // Optional (D-155): the route domain a packet arrived in, which the
    // graph's VIF writes. No writer means domain 0, the default domain -- the
    // same attribute name the VIF uses, so a graph gets the packet's own
    // domain with no configuration on either side. Declared with the default
    // contract: the framework resolves it at PreResume and the packet path
    // reads the constant without touching packet metadata when no provider
    // exists.
    static constexpr std::array<uint8_t, sizeof(bess::utils::be32_t)>
        kDefaultRouteDomain{};
    route_domain_attr_ = AddMetadataAttrWithDefault(
        domain_attr_name_, sizeof(bess::utils::be32_t),
        bess::metadata::Attribute::AccessMode::kRead, kDefaultRouteDomain);
    if (route_domain_attr_ < 0) {
      auto released = router_->Release();
      CHECK(released) << released.error();
      next_hops_binding_.Reset();
      neighbors_binding_.Reset();
      groups_binding_.Reset();
      routes_binding_.Reset();
      router_.reset();
      return CommandFailure(-route_domain_attr_, "add_metadata_attr() failed");
    }
  }
  if (neighbor_resolver_ != nullptr) {
    static constexpr std::array<uint8_t, sizeof(bess::utils::be32_t)>
        kNoNeighborInterface{};
    ingress_interface_attr_ = AddMetadataAttrWithDefault(
        ingress_interface_attr_name_, sizeof(bess::utils::be32_t),
        bess::metadata::Attribute::AccessMode::kRead, kNoNeighborInterface);
    if (ingress_interface_attr_ < 0) {
      return CommandFailure(-ingress_interface_attr_,
                            "add_metadata_attr() failed");
    }
    static std::atomic<uint32_t> next_ring_id{0};
    const std::string ring_suffix =
        std::to_string(next_ring_id.fetch_add(1, std::memory_order_relaxed));
    const std::string rx_name = "rrx" + ring_suffix;
    const std::string arp_name = "rarp" + ring_suffix;
    const std::string tx_name = "rtx" + ring_suffix;
    neighbor_rx_ring_ = BessRouterRingCreateElem(
        rx_name.c_str(), sizeof(NeighborFrame), 1024, SOCKET_ID_ANY, 0);
    // Requests have their own ring, so a busy segment's requests cannot take
    // the room replies need.
    neighbor_arp_ring_ = BessRouterRingCreateElem(
        arp_name.c_str(), sizeof(ArpRequestCopy), 256, SOCKET_ID_ANY, 0);
    neighbor_tx_ring_ =
        rte_ring_create(tx_name.c_str(), 1024, SOCKET_ID_ANY, 0);
    if (neighbor_rx_ring_ == nullptr || neighbor_arp_ring_ == nullptr ||
        neighbor_tx_ring_ == nullptr) {
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
    NeighborFrame frame{};
    while (rte_ring_dequeue_elem(neighbor_rx_ring_, &frame,
                                 sizeof(NeighborFrame)) == 0) {
      bess::PacketFree(frame.packet);
    }
    rte_ring_free(neighbor_rx_ring_);
    neighbor_rx_ring_ = nullptr;
  }
  if (neighbor_arp_ring_ != nullptr) {
    rte_ring_free(neighbor_arp_ring_);
    neighbor_arp_ring_ = nullptr;
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
  neighbors_binding_.Reset();
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
          attr_offset(ingress_interface_attr_))) {
    LOG(ERROR) << "Router '" << name() << "': the '"
               << ingress_interface_attr_name_
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

// Resolver input is ARP replies and Neighbor Advertisements, and a copy of
// each ARP request. A reply or advertisement goes to the resolver whole. A
// request (an ARP request or a Neighbor Solicitation) is this node's
// responder's business: it goes to the local punt gate, where a graph puts
// one, or is dropped, and is counted either way. An ARP request is also copied
// (its 42 bytes, inline on a ring of its own, so a busy segment's requests
// cannot take the room replies need): one from a bound neighbor -- a
// gratuitous announcement included -- moves that neighbor to the MAC it
// carries (RFC 826's merge, NeighborResolver::OnArp). Everything else stays in
// the batch and is routed. A family no configured interface has an address
// of has no neighbor protocol here (D-183): its ARP or ND frames stay in the
// batch like any other frame of that kind.
void Router::DivertNeighborFrames(Context *ctx, bess::PacketBatch *batch) {
  constexpr size_t kEth = sizeof(bess::utils::Ethernet);
  const bool arp_input = arp_input_;
  const bool nd_input = nd_input_;
  // The interface a frame arrived on is read here, with the offsets of the
  // graph that delivered it, and travels with the frame (NeighborFrame).
  const bess::metadata::mt_offset_t interface_offset =
      attr_offset(ingress_interface_attr_);
  // 0 (no interface) when nothing readable writes the attribute.
  const auto ReadInterface = [](bess::PacketRef pkt,
                                bess::metadata::mt_offset_t offset) -> uint32_t {
    return bess::metadata::IsValidOffset(offset)
               ? get_attr_with_offset<bess::utils::be32_t>(offset, pkt).value()
               : 0;
  };
  std::array<bess::PacketHandle, bess::PacketBatch::kMaxBurst> routed;
  size_t routed_count = 0;
  const int count = batch->cnt();
  for (int i = 0; i < count; i++) {
    bess::PacketRef pkt = batch->packet(i);
    const uint8_t *frame = pkt.head_data<const uint8_t *>();
    const size_t len = pkt.head_len();
    enum class Kind { kRouted, kReply, kRequest, kUnusable } kind = Kind::kRouted;
    bool arp_request = false;
    if (len >= kEth) {
      const auto *eth = reinterpret_cast<const bess::utils::Ethernet *>(frame);
      if (arp_input && eth->ether_type.value() == bess::utils::Ethernet::kArp) {
        if (len < kEth + sizeof(bess::utils::Arp)) {
          kind = Kind::kUnusable;
        } else {
          const auto *arp =
              reinterpret_cast<const bess::utils::Arp *>(frame + kEth);
          kind = arp->opcode.value() == bess::utils::Arp::kReply
                     ? Kind::kReply
                     : Kind::kRequest;
          arp_request = arp->opcode.value() == bess::utils::Arp::kRequest;
        }
      } else if (nd_input && eth->ether_type.value() == bess::utils::Ethernet::kIpv6 &&
                 len > kEth + route::kIpv6HeaderSize &&
                 frame[kEth + 6] == route::kIpv6NextHeaderIcmpv6) {
        const uint8_t type = frame[kEth + route::kIpv6HeaderSize];
        if (type == static_cast<uint8_t>(route::NdType::kNeighborAdvertisement)) {
          kind = Kind::kReply;
        } else if (type ==
                   static_cast<uint8_t>(route::NdType::kNeighborSolicitation)) {
          kind = Kind::kRequest;
        }
      }
    }
    switch (kind) {
      case Kind::kRouted:
        routed[routed_count++] = pkt.handle();
        continue;
      case Kind::kRequest:
        neighbor_requests_.fetch_add(1, std::memory_order_relaxed);
        if (arp_request) {
          ArpRequestCopy copy;
          copy.interface = ReadInterface(pkt, interface_offset);
          std::memcpy(copy.frame.data(), frame, kEth + sizeof(bess::utils::Arp));
          if (copy.interface == 0 || !resolver_gates_.contains(copy.interface) ||
              rte_ring_enqueue_elem(neighbor_arp_ring_, &copy,
                                    sizeof(ArpRequestCopy)) != 0) {
            neighbor_input_drops_.fetch_add(1, std::memory_order_relaxed);
          }
        }
        if (punt_neighbor_gate_ != DROP_GATE) {
          EmitPacket(ctx, pkt, punt_neighbor_gate_);
        } else {
          bess::PacketFree(pkt.handle());
        }
        continue;
      case Kind::kReply: {
        const uint32_t id = ReadInterface(pkt, interface_offset);
        // Bounded input: an unknown interface or a full ring fails closed
        // rather than routing an ARP/ND control frame as IP.
        NeighborFrame queued{pkt.handle(), id, 0};
        if (id != 0 && resolver_gates_.contains(id) &&
            rte_ring_enqueue_elem(neighbor_rx_ring_, &queued,
                                  sizeof(NeighborFrame)) == 0) {
          continue;
        }
        break;
      }
      case Kind::kUnusable:
        break;
    }
    neighbor_input_drops_.fetch_add(1, std::memory_order_relaxed);
    bess::PacketFree(pkt.handle());
  }
  batch->clear();
  for (size_t i = 0; i < routed_count; i++) {
    batch->add(bess::PacketRef(routed[i]));
  }
}

void Router::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (neighbor_resolver_ != nullptr) {
    DivertNeighborFrames(ctx, batch);
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

// A neighbor change is a next-hop write, applied through the process's
// transaction engine like any other (D-183) -- the module is in-tree, so it
// uses the engine directly; plugins keep the registry's narrower face (D-074).
// At most kMaxNeighborUpdatesPerTick go in one transaction, so a burst of
// changes (an interface with many bindings changing identity) is published
// over several ticks rather than as one transaction the engine's retire budget
// refuses on every tick. A transaction that is not applied (the engine is
// busy reclaiming) leaves its updates pending for the next tick; the warning
// for that is rate-limited, since the tick runs every millisecond.
void Router::PublishNeighborUpdates(uint64_t now) {
  if (pending_neighbor_updates_.empty()) {
    return;
  }
  const size_t take =
      std::min(pending_neighbor_updates_.size(), kMaxNeighborUpdatesPerTick);
  std::vector<dataplane::Op> ops;
  ops.reserve(take);
  for (size_t i = 0; i < take; i++) {
    const auto &[id, hop] = pending_neighbor_updates_[i];
    ops.push_back(router_->SetNeighborManagedNextHopOp(id, hop));
  }
  const dataplane::TransactionEngine::Result result =
      bess::runtime::runtime().transactions().Apply(ops);
  if (result.outcome == dataplane::TransactionEngine::Outcome::kApplied) {
    pending_neighbor_updates_.erase(
        pending_neighbor_updates_.begin(),
        pending_neighbor_updates_.begin() + static_cast<ptrdiff_t>(take));
    neighbor_warned_ = false;
    return;
  }
  if (neighbor_warned_ && now - neighbor_warned_tick_ < kNeighborWarningInterval) {
    return;
  }
  neighbor_warned_ = true;
  neighbor_warned_tick_ = now;
  std::string why = "the engine is busy";
  for (const auto &op : result.ops) {
    if (!op.error.empty()) {
      why = op.error;
      break;
    }
  }
  LOG(WARNING) << "Router '" << name() << "': "
               << pending_neighbor_updates_.size()
               << " neighbor change(s) not yet published (" << why
               << "); retrying";
}

// Every binding, in id order, with the resolver's view of its neighbor. A
// neighbor the resolver does not know yet (bound since the last tick) is
// incomplete with no probes, as its published hop is.
CommandResponse Router::CommandGetNeighbors(const bess::pb::EmptyArg &) {
  bess::pb::RouterCommandGetNeighborsResponse response;
  router_->ForEachNeighborBinding(
      [&](route::NextHopId id, const route::NeighborKey &key) {
        const bess::pb::RouterNeighborBinding binding = NeighborKeyToPb(key);
        bess::pb::RouterNeighborStatus *status = response.add_neighbors();
        status->set_next_hop_id(id.value());
        status->set_interface_id(binding.interface_id());
        status->set_ipv4(binding.ipv4());
        status->set_ipv6(binding.ipv6());
        status->set_state(bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE);
        const auto known = neighbor_resolver_ == nullptr
                               ? std::nullopt
                               : neighbor_resolver_->StatusOf(key);
        if (!known) {
          return;
        }
        status->set_state(NeighborStateToPb(known->state));
        if (!known->mac.IsZero()) {
          status->set_mac_addr(known->mac.ToString());
        }
        status->set_reconfirming(known->reconfirming);
        status->set_probes(known->probes);
      });
  return CommandSuccess(response);
}

// Takes up the bindings transactions published since the last tick (D-183).
// Only each id's final binding matters -- several transactions may have
// changed one id since the last tick, and one transaction publishes its binds
// before its unbinds -- so the changes are coalesced per id first. Then, with
// forgetting deferred (a neighbor whose last binding goes releases its timer
// and room at once but keeps what was learned):
//   1. a final binding whose neighbor the resolver knows is installed, so a
//      neighbor moved to another id -- in a chain, a swap or a rotation, in
//      any id order -- is taken up resolved, with its MAC, and not probed;
//   2. every resolver binding that is gone or replaced is released;
// then every neighbor no id names any more is forgotten, which frees the
// room for
//   3. the remaining bindings, installed fresh (an unbind next to a bind at
//      max_neighbors finds the room the unbind freed).
// A bound id is published as the resolver has it; an unbound one stops, and
// any of its updates still pending are dropped, since its key is gone. The
// policy checked each binding when it was reserved, so Bind refuses only for
// a full timer wheel or table, which max_neighbors sizes; such a binding
// stays incomplete and is logged.
void Router::ApplyNeighborBindingChanges(uint64_t now) {
  std::vector<route::NeighborBindingChange> changes =
      router_->TakeNeighborBindingChanges();
  if (changes.empty()) {
    return;
  }
  // Last change per id wins; stable, so equal ids keep publication order.
  std::stable_sort(changes.begin(), changes.end(),
                   [](const auto &a, const auto &b) { return a.id < b.id; });
  size_t out = 0;
  for (size_t i = 0; i < changes.size(); i++) {
    if (out != 0 && changes[out - 1].id == changes[i].id) {
      changes[out - 1] = changes[i];
    } else {
      changes[out++] = changes[i];
    }
  }
  changes.resize(out);
  std::erase_if(pending_neighbor_updates_, [&changes](const auto &pending) {
    return std::binary_search(
        changes.begin(), changes.end(), route::NeighborBindingChange{pending.first, {}},
        [](const auto &a, const auto &b) { return a.id < b.id; });
  });
  const auto install = [&](const route::NeighborBindingChange &change, bool log) {
    auto hop = neighbor_resolver_->Bind(change.id, *change.neighbor, now);
    if (!hop) {
      if (log) {
        LOG(ERROR) << "Router '" << name() << "': next hop " << change.id.value()
                   << " stays unresolved: " << hop.error();
      }
      return false;
    }
    const std::pair<route::NextHopId, route::NextHop> update{change.id, *hop};
    QueueNeighborUpdates(std::span(&update, 1));
    return true;
  };
  std::vector<bool> installed(changes.size(), false);
  {
    // Ends the deferral however the passes leave (Bind may throw).
    struct Deferred {
      route::NeighborResolver &resolver;
      ~Deferred() { resolver.ForgetUnbound(); }
    } deferred{*neighbor_resolver_};
    neighbor_resolver_->DeferForgetting();
    for (size_t i = 0; i < changes.size(); i++) {
      const route::NeighborBindingChange &change = changes[i];
      if (change.neighbor && neighbor_resolver_->StatusOf(*change.neighbor)) {
        installed[i] = install(change, false);
      }
    }
    for (size_t i = 0; i < changes.size(); i++) {
      if (!installed[i] &&
          neighbor_resolver_->BindingOf(changes[i].id) != changes[i].neighbor) {
        neighbor_resolver_->Unbind(changes[i].id);
      }
    }
  }
  for (size_t i = 0; i < changes.size(); i++) {
    if (!installed[i] && changes[i].neighbor) {
      install(changes[i], true);
    }
  }
}

void Router::ResolverTick(bess::framework::TickContext &ctx) {
  if (neighbor_resolver_ == nullptr) {
    return;
  }
  const uint64_t now = ctx.tick();
  ApplyNeighborBindingChanges(now);
  constexpr unsigned kResolverWorkBudget = 64;
  for (unsigned n = 0; n < kResolverWorkBudget && !ctx.Expired(); n++) {
    NeighborFrame queued{};
    if (rte_ring_dequeue_elem(neighbor_rx_ring_, &queued,
                              sizeof(NeighborFrame)) != 0) {
      break;
    }
    // The interface was checked against resolver_gates_ when the frame was
    // queued; interface ids are 16 bits (InterfaceOf), so it fits.
    const bess::PacketRef pkt(queued.packet);
    const auto *frame = pkt.head_data<const uint8_t *>();
    const std::span<const uint8_t> bytes(frame, pkt.head_len());
    const dataplane::InterfaceId ingress(
        static_cast<uint16_t>(queued.interface));
    const auto *eth = reinterpret_cast<const bess::utils::Ethernet *>(frame);
    const route::NeighborResolver::Progress progress =
        eth->ether_type.value() == bess::utils::Ethernet::kArp
            ? neighbor_resolver_->OnArp(now, ingress, bytes)
            : neighbor_resolver_->OnNd(now, ingress, bytes);
    QueueNeighborUpdates(progress.updates);
    bess::PacketFree(queued.packet);
  }
  // ARP requests after replies: a reply answers what was asked, a request
  // only says where its sender is now.
  for (unsigned n = 0; n < kResolverWorkBudget && !ctx.Expired(); n++) {
    ArpRequestCopy copy;
    if (rte_ring_dequeue_elem(neighbor_arp_ring_, &copy,
                              sizeof(ArpRequestCopy)) != 0) {
      break;
    }
    const route::NeighborResolver::Progress progress = neighbor_resolver_->OnArp(
        now, dataplane::InterfaceId(static_cast<uint16_t>(copy.interface)),
        std::span<const uint8_t>(copy.frame.data(),
                                 sizeof(bess::utils::Ethernet) +
                                     sizeof(bess::utils::Arp)));
    QueueNeighborUpdates(progress.updates);
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
  PublishNeighborUpdates(now);
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
    // about a hop-count boundary or about which addresses a hop writes
    // (D-153): the IPv4 TTL or IPv6 hop limit first, then the addresses when
    // the hop names both (route::WriteForwarded). A frame that is not IP has
    // no hop count to decrement: it keeps what it arrived with and, as
    // before, still gets the hop's addresses. A header the parser cannot read
    // is dropped and counted, never forwarded. Both families are forwarded:
    // this mode has no FIB and so no family setting (D-183), and the hop
    // count written is the packet's own family's.
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
    // A non-initial fragment still carries its IP header; only its L4 header
    // is missing. So the hop count is every IP frame's, fragments included:
    // the destination mode drops a fragment because its group hash needs
    // ports, while this mode routes by id and forwards it with the hop count
    // written.
    const bool ip = status != bess::conntrack::ParseStatus::kNotIp;
    const route::L3Family family = parsed.l3 == bess::conntrack::L3Kind::kIpv6
                                       ? route::L3Family::kIpv6
                                       : route::L3Family::kIpv4;
    bess::PacketHandle handle = pkt.handle();
    if (!bess::packet::EnsureWritable(handle)) {
      gates[i] = DROP_GATE;
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    batch->handles()[i] = handle;
    bess::PacketRef writable(handle);
    if (!ip) {
      if (!route::RewriteL2(writable, *hop)) {
        gates[i] = DROP_GATE;
        header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    // The parse ran before EnsureWritable, and the copy may have resegmented
    // the frame: where the header is is re-established, not assumed. Writing
    // it through a stale offset would touch bytes outside this segment.
    if (!HeaderFits(writable, family, parsed.l3_offset)) {
      gates[i] = DROP_GATE;
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (route::WriteForwarded(
            std::span(writable.head_data<uint8_t *>(), writable.head_len()), family,
            parsed.l3_offset, *hop) != route::TtlResult::kForward) {
      gates[i] = DROP_GATE;
      ttl_expired_frames_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  EmitBatch(ctx, batch, gates);
}

// One counter per verdict the chain reports (route/forward.h). kForward is not
// a drop; kLocalDelivery is a delivery (D-178); kNoEgress is the blackhole
// shape (D-160), counted apart from a miss because the route exists and chose
// that hop; kLinkScope is IPv6 that is never forwarded (D-183).
void Router::CountVerdict(route::ForwardVerdict verdict) {
  switch (verdict) {
    case route::ForwardVerdict::kLocalDelivery:
      local_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kNotIp:
      not_ip_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kFamilyDisabled:
      family_disabled_frames_.fetch_add(1, std::memory_order_relaxed);
      return;
    case route::ForwardVerdict::kLinkScope:
      link_scope_frames_.fetch_add(1, std::memory_order_relaxed);
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

// A link-scoped IPv6 frame is the node's own business, as a local route's is:
// both take the local punt gate (D-178, D-183).
gate_idx_t Router::PuntGateOf(route::ForwardVerdict verdict) const {
  switch (verdict) {
    case route::ForwardVerdict::kNoRoute:
      return punt_miss_gate_;
    case route::ForwardVerdict::kUnresolved:
      return punt_unresolved_gate_;
    case route::ForwardVerdict::kLocalDelivery:
    case route::ForwardVerdict::kLinkScope:
      return punt_local_gate_;
    default:
      return DROP_GATE;
  }
}

bool Router::HeaderFits(bess::PacketRef pkt, route::L3Family family,
                        size_t l3_offset) {
  const size_t header = family == route::L3Family::kIpv6
                            ? route::kIpv6HeaderSize
                            : sizeof(bess::utils::Ipv4);
  return pkt.head_len() >= l3_offset + header;
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
    if (r.verdict != route::ForwardVerdict::kForward) {
      CountVerdict(r.verdict);
      // A punt gate is the application's slow path (D-162): a frame the
      // destination path cannot forward leaves on it instead of being
      // dropped, and it is counted either way, so the counters stay the
      // record of what happened. A local route's frame is not a failure: it is
      // delivered to this node's local path, and its gate is the one the graph
      // gave for that (D-178).
      gates[i] = PuntGateOf(r.verdict);
      continue;
    }
    const gate_idx_t gate = GateOf(r.egress);
    if (gate == DROP_GATE) {
      // A hop whose egress is no gate of this graph: configuration, not
      // traffic, so nothing is counted and nothing is written.
      continue;
    }

    // The hop count is written whatever the hop names, so the header must be
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
    // The copy may have resegmented the frame, so where the IP header is is
    // re-established rather than assumed: writing it through a stale pointer
    // would touch bytes that are not in this segment.
    if (!HeaderFits(writable, r.family, r.l3_offset)) {
      header_write_failures_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (route::WriteForwarded(
            std::span(writable.head_data<uint8_t *>(), writable.head_len()), r.family,
            r.l3_offset, *r.hop) != route::TtlResult::kForward) {
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
    // No FIB and no family setting (D-183): both families are forwarded.
    desc = bess::utils::Format(
        "%zu next hops, %zu header write failures, %zu local, ttl expired: %zu, "
        "next-hop id from metadata (no FIB)",
        router_->next_hop_count(), static_cast<size_t>(header_write_failures()),
        static_cast<size_t>(local_frames()),
        static_cast<size_t>(ttl_expired_frames()));
  } else {
    // Which families the router routes (D-183): the other family's frames
    // are dropped and counted as family disabled.
    const char *families = router_->has_ipv4() && router_->has_ipv6() ? "IPv4+IPv6"
                           : router_->has_ipv4()                     ? "IPv4"
                                                                     : "IPv6";
    desc = bess::utils::Format(
        "%s, %zu routes, %zu next hops, %zu groups, %zu header write failures, "
        "routing by destination (not ip: %zu, family disabled: %zu, malformed: %zu, "
        "non-initial fragments: %zu, no route: %zu, unresolved: %zu, ttl expired: %zu, "
        "blackholed: %zu, local: %zu, link scope: %zu)",
        families, router_->route_count(), router_->next_hop_count(),
        router_->next_hop_group_count(),
        static_cast<size_t>(header_write_failures()),
        static_cast<size_t>(not_ip_frames()),
        static_cast<size_t>(family_disabled_frames()),
        static_cast<size_t>(malformed_frames()),
        static_cast<size_t>(non_initial_fragments()), static_cast<size_t>(no_route_frames()),
        static_cast<size_t>(unresolved_frames()),
        static_cast<size_t>(ttl_expired_frames()),
        static_cast<size_t>(blackholed_frames()),
        static_cast<size_t>(local_frames()),
        static_cast<size_t>(link_scope_frames()));
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
  if (neighbor_resolver_ != nullptr) {
    desc += bess::utils::Format(
        ", %zu neighbors (requests: %zu, input drops: %zu, unpublished changes: %zu)",
        neighbor_resolver_->size(), static_cast<size_t>(neighbor_requests()),
        static_cast<size_t>(neighbor_input_drops()), pending_neighbor_updates());
    // ARP requests and Neighbor Solicitations take the neighbor punt gate
    // (by default the local one) in either mode: they are this node's, not a
    // route's.
    if (punt_neighbor_gate_ != DROP_GATE) {
      desc += bess::utils::Format(", neighbor requests punt to gate %d",
                                  punt_neighbor_gate_);
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
