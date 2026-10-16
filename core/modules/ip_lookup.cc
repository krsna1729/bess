// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "ip_lookup.h"

#include <cerrno>
#include <string>

#include "dataplane/transaction_engine.h"
#include "utils/bits.h"
#include "utils/ether.h"
#include "utils/format.h"
#include "utils/ip.h"

using bess::route::Ipv4Prefix;
using bess::route::RouteErrno;
using bess::route::RouteError;

static inline int is_valid_gate(gate_idx_t gate) {
  return (gate < MAX_GATES || gate == DROP_GATE);
}

// THREAD_SAFE: route changes are applied to the live table by its single
// serialized writer while workers keep looking up (K7; DPDK's lock-free
// reader design plus the runtime's QSBR for tbl8 reclamation), so workers are
// never paused for a route change.
// "<name>/routes" holds the routes; the class has no actions.
const Commands IPLookup::cmds = {};

const ResourceTables IPLookup::resource_tables = {
    ResourceTable::Map<bess::pb::IPLookupRouteKey, bess::pb::IPLookupRouteValue>(
        "routes")};

CommandResponse IPLookup::Init(const bess::pb::IPLookupArg &arg) {
  bess::route::LpmRouteTable::Config config;
  config.max_routes = arg.max_rules() ? arg.max_rules() : 1024;
  config.tbl8_groups = arg.max_tbl8s() ? arg.max_tbl8s() : 128;
  switch (arg.backend()) {
    case bess::pb::ROUTE_BACKEND_LPM:
      config.backend = bess::route::RouteBackend::kRteLpm;
      break;
    case bess::pb::ROUTE_BACKEND_FIB:
      config.backend = bess::route::RouteBackend::kRteFib;
      break;
    default:
      return CommandFailure(EINVAL, "unknown route backend %d", arg.backend());
  }

  auto table = bess::route::RouteTable<GateRoute>::Create(
      name(), config, init_context().rcu());
  if (!table) {
    return CommandFailure(RouteErrno(table.error()), "route table: %s",
                          bess::route::RouteErrorName(table.error()));
  }
  routes_ = std::move(*table);

  // The routes as a transaction resource: a prefix that misses today is
  // placed at DROP_GATE meanwhile, which is what a miss does here.
  resource_ = std::make_unique<bess::route::LpmTableResource>(
      name() + "/routes", routes_->untyped(), DROP_GATE);
  binding_ = init_context().codecs().Bind(
      *resource_,
      std::make_shared<bess::framework::TypedCodec<bess::pb::IPLookupRouteKey,
                                                   bess::pb::IPLookupRouteValue>>(
          [this](const bess::pb::IPLookupRouteKey &key)
              -> std::expected<bess::dataplane::ResourceKey, std::string> {
            ParsedPrefix parsed = ParseIpv4Prefix(key.prefix(), key.prefix_len());
            if (std::get<0>(parsed)) {
              return std::unexpected(std::get<1>(parsed));
            }
            const auto p = Ipv4Prefix::Make(std::get<2>(parsed).value(),
                                            static_cast<uint8_t>(key.prefix_len()));
            if (!p) {
              return std::unexpected(bess::route::RouteErrorName(p.error()));
            }
            return bess::route::LpmTableResource::Key(*p);
          },
          [](const bess::pb::IPLookupRouteValue &value)
              -> std::expected<std::any, std::string> {
            if (!bess::IsValidGateValue(value.gate()) ||
                !is_valid_gate(static_cast<gate_idx_t>(value.gate()))) {
              return std::unexpected("invalid gate " + std::to_string(value.gate()));
            }
            return std::any(static_cast<uint32_t>(value.gate()));
          },
          // Read back as a client sends a route: the network address in
          // dotted decimal (the key refuses host bits, so it is the only
          // spelling), "0.0.0.0" with prefix_len 0 for the default route.
          [this](const auto &visit) {
            routes_->ForEach([&visit](Ipv4Prefix prefix, GateRoute gate) {
              bess::pb::IPLookupRouteKey key;
              key.set_prefix(bess::utils::ToIpv4Address(be32_t(prefix.addr())));
              key.set_prefix_len(prefix.length());
              bess::pb::IPLookupRouteValue value;
              value.set_gate(gate.value());
              visit(key, value);
            });
          }));
  if (auto registered = init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void IPLookup::DeInit() {
  if (resource_ != nullptr) {
    // Cannot be refused: routes reference nothing and nothing references
    // them, and this runs under the control-plane lock transactions take.
    auto unregistered = init_context().resources().Unregister(resource_->name());
    CHECK(unregistered) << unregistered.error();
    binding_.Reset();
    resource_.reset();
  }
  // The control plane pauses workers before deleting a module, so no batch
  // can be using the table.
  routes_.reset();
}

void IPLookup::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (routes_ == nullptr) {
    return;
  }
  // One acquire load per batch; valid for this invocation only.
  const auto table = routes_->Read();

  const int cnt = batch->cnt();
  uint32_t dst[bess::PacketBatch::kMaxBurst];
  uint32_t gates[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    const auto *eth = batch->packet(i).head_data<bess::utils::Ethernet *>();
    const auto *ip = reinterpret_cast<const bess::utils::Ipv4 *>(eth + 1);
    dst[i] = ip->dst.value();
  }

  const uint64_t hits = table.LookupBatch(std::span(dst, cnt),
                                          std::span(gates, cnt));
  gate_idx_t out[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    out[i] = (hits >> i) & 1 ? static_cast<gate_idx_t>(gates[i])
                             : static_cast<gate_idx_t>(DROP_GATE);
  }
  EmitBatch(ctx, batch, out);
}

ParsedPrefix IPLookup::ParseIpv4Prefix(const std::string &prefix,
                                       uint64_t prefix_len) {
  using bess::utils::Format;
  be32_t net_addr;
  be32_t net_mask;

  if (!prefix.length()) {
    return std::make_tuple(EINVAL, "prefix' is missing", be32_t(0));
  }
  if (!bess::utils::ParseIpv4Address(prefix, &net_addr)) {
    return std::make_tuple(
        EINVAL, Format("Invalid IP prefix: %s", prefix.c_str()), be32_t(0));
  }

  if (prefix_len > 32) {
    return std::make_tuple(
        EINVAL, Format("Invalid prefix length: %" PRIu64, prefix_len),
        be32_t(0));
  }

  net_mask = be32_t(bess::utils::SetBitsLow<uint32_t>(prefix_len));
  if ((net_addr & ~net_mask).value()) {
    return std::make_tuple(
        EINVAL,
        Format("Invalid IP prefix %s/%" PRIu64 " %x %x", prefix.c_str(),
               prefix_len, net_addr.value(), net_mask.value()),
        be32_t(0));
  }
  return std::make_tuple(0, "", net_addr);
}

ADD_MODULE(IPLookup, "ip_lookup",
           "performs Longest Prefix Match on IPv4 packets")
