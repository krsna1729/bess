// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#include "ip_lookup.h"

#include <cerrno>
#include <string>

#include "../control/runtime_state.h"
#include "../utils/bits.h"
#include "../utils/ether.h"
#include "../utils/format.h"
#include "../utils/ip.h"

using bess::route::Ipv4Prefix;
using bess::route::RouteError;

static inline int is_valid_gate(gate_idx_t gate) {
  return (gate < MAX_GATES || gate == DROP_GATE);
}

// THREAD_SAFE: route changes are applied to the live rte_lpm by its single
// serialized writer while workers keep looking up (K7; DPDK's lock-free
// reader design plus the runtime's QSBR for tbl8 reclamation), so workers are
// never paused for a route change.
const Commands IPLookup::cmds = {
    {"add", "IPLookupCommandAddArg", MODULE_CMD_FUNC(&IPLookup::CommandAdd),
     Command::THREAD_SAFE},
    {"delete", "IPLookupCommandDeleteArg",
     MODULE_CMD_FUNC(&IPLookup::CommandDelete), Command::THREAD_SAFE},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&IPLookup::CommandClear),
     Command::THREAD_SAFE}};

static int RouteErrno(RouteError error) {
  switch (error) {
    case RouteError::kTableFull:
      return ENOSPC;
    case RouteError::kNotFound:
      return ENOENT;
    case RouteError::kBackendFailure:
      return EIO;
    default:
      return EINVAL;
  }
}

CommandResponse IPLookup::Init(const bess::pb::IPLookupArg &arg) {
  bess::route::LpmRouteTable::Config config;
  config.max_routes = arg.max_rules() ? arg.max_rules() : 1024;
  config.tbl8_groups = arg.max_tbl8s() ? arg.max_tbl8s() : 128;
  config.socket = 0;

  auto table = bess::route::RouteTable<GateRoute>::Create(
      name(), config, bess::control::runtime().rcu());
  if (!table) {
    return CommandFailure(RouteErrno(table.error()), "route table: %s",
                          bess::route::RouteErrorName(table.error()));
  }
  routes_ = std::move(*table);
  return CommandSuccess();
}

void IPLookup::DeInit() {
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
  for (int i = 0; i < cnt; i++) {
    const gate_idx_t gate = (hits >> i) & 1
                                ? static_cast<gate_idx_t>(gates[i])
                                : static_cast<gate_idx_t>(DROP_GATE);
    EmitPacket(ctx, batch->packet(i), gate);
  }
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

CommandResponse IPLookup::CommandAdd(
    const bess::pb::IPLookupCommandAddArg &arg) {
  gate_idx_t gate = arg.gate();
  uint64_t prefix_len = arg.prefix_len();
  ParsedPrefix prefix = ParseIpv4Prefix(arg.prefix(), prefix_len);
  if (std::get<0>(prefix)) {
    return CommandFailure(std::get<0>(prefix), "%s",
                          std::get<1>(prefix).c_str());
  }

  if (!is_valid_gate(gate)) {
    return CommandFailure(EINVAL, "Invalid gate: %hu", gate);
  }

  const auto p = Ipv4Prefix::Make(std::get<2>(prefix).value(),
                                  static_cast<uint8_t>(prefix_len));
  if (!p) {
    return CommandFailure(EINVAL, "%s", bess::route::RouteErrorName(p.error()));
  }
  // An existing prefix is re-pointed, as rte_lpm_add() always did.
  if (auto added = routes_->Upsert(*p, GateRoute(gate)); !added) {
    return CommandFailure(RouteErrno(added.error()), "%s",
                          bess::route::RouteErrorName(added.error()));
  }
  return CommandSuccess();
}

CommandResponse IPLookup::CommandDelete(
    const bess::pb::IPLookupCommandDeleteArg &arg) {
  uint64_t prefix_len = arg.prefix_len();
  ParsedPrefix prefix = ParseIpv4Prefix(arg.prefix(), prefix_len);
  if (std::get<0>(prefix)) {
    return CommandFailure(std::get<0>(prefix), "%s",
                          std::get<1>(prefix).c_str());
  }

  const auto p = Ipv4Prefix::Make(std::get<2>(prefix).value(),
                                  static_cast<uint8_t>(prefix_len));
  if (!p) {
    return CommandFailure(EINVAL, "%s", bess::route::RouteErrorName(p.error()));
  }
  auto erased = routes_->Erase(*p);
  if (!erased) {
    if (prefix_len == 0 && erased.error() == RouteError::kNotFound) {
      return CommandSuccess();  // deleting an unset default always succeeded
    }
    if (erased.error() == RouteError::kNotFound) {
      return CommandFailure(ENOENT, "no such rule");
    }
    return CommandFailure(RouteErrno(erased.error()), "%s",
                          bess::route::RouteErrorName(erased.error()));
  }
  return CommandSuccess();
}

CommandResponse IPLookup::CommandClear(const bess::pb::EmptyArg &) {
  // Rules go, the default gate stays -- what rte_lpm_delete_all() did.
  if (auto cleared = routes_->Clear(); !cleared) {
    return CommandFailure(RouteErrno(cleared.error()), "%s",
                          bess::route::RouteErrorName(cleared.error()));
  }
  return CommandSuccess();
}

ADD_MODULE(IPLookup, "ip_lookup",
           "performs Longest Prefix Match on IPv4 packets")
