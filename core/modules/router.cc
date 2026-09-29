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

#include "router.h"

#include <array>
#include <cstdint>
#include <limits>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "../control/runtime_state.h"
#include "../dataplane/resource_codec.h"
#include "../dataplane/transaction_engine.h"
#include "../utils/endian.h"
#include "../utils/ether.h"
#include "../utils/format.h"
#include "../utils/ip.h"

namespace dataplane = bess::dataplane;
namespace route = bess::route;

const Commands Router::cmds = {};

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

}  // namespace

CommandResponse Router::Init(const bess::pb::RouterArg &arg) {
  if (arg.max_routes() > std::numeric_limits<uint32_t>::max() ||
      arg.max_tbl8s() > std::numeric_limits<uint32_t>::max() ||
      arg.max_next_hops() > route::LpmRouteTable::kMaxValue) {
    return CommandFailure(EINVAL, "router capacity exceeds backend limits");
  }
  route::LpmRouteTable::Config config;
  config.max_routes = arg.max_routes() ? arg.max_routes() : 4096;
  config.tbl8_groups = arg.max_tbl8s() ? arg.max_tbl8s() : 256;
  config.socket = 0;
  const size_t max_next_hops =
      arg.max_next_hops() ? static_cast<size_t>(arg.max_next_hops()) : 4096;

  auto router = route::Router::Create(name(), config, max_next_hops,
                                      bess::control::runtime().rcu());
  if (!router) {
    return CommandFailure(route::RouteErrno(router.error()), "router: %s",
                          route::RouteErrorName(router.error()));
  }
  router_ = std::move(*router);
  if (auto enrolled = router_->Enroll(bess::control::runtime().transactions());
      !enrolled) {
    router_.reset();
    return CommandFailure(EINVAL, "%s", enrolled.error().c_str());
  }

  // Typed keys and values over the RPC (D-025).
  router_->next_hops_resource_object()->SetCodec(
      std::make_shared<dataplane::TypedCodec<bess::pb::RouterNextHopIdKey,
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
            hop.egress = static_cast<gate_idx_t>(value.egress_gate());
            auto neighbor = NeighborFromPb(value.neighbor());
            if (!neighbor) {
              return std::unexpected(neighbor.error());
            }
            hop.neighbor = *neighbor;
            hop.dst_mac = *dst;
            hop.src_mac = *src;
            return std::any(hop);
          }));
  router_->routes_resource_object()->SetCodec(
      std::make_shared<dataplane::TypedCodec<bess::pb::RouterRouteKey,
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
            return route::Router::RouteKey(*prefix);
          },
          [](const bess::pb::RouterRouteValue &value)
              -> std::expected<std::any, std::string> {
            if (value.next_hop_id() == 0) {
              return std::unexpected("next hop id 0 is invalid");
            }
            return std::any(route::NextHopId(value.next_hop_id()));
          }));

  next_hop_id_attr_ = AddMetadataAttr("next_hop_id",
                                      sizeof(bess::utils::be32_t),
                                      bess::metadata::Attribute::AccessMode::kRead);
  if (next_hop_id_attr_ < 0) {
    auto released = router_->Release();
    CHECK(released) << released.error();
    router_.reset();
    return CommandFailure(-next_hop_id_attr_, "add_metadata_attr() failed");
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
  router_.reset();
}

int Router::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  // Metadata offsets are assigned just before this: an attribute with no
  // valid offset means the packet path will drop everything, which is worth
  // saying once, here, rather than never.
  if (!bess::metadata::IsValidOffset(attr_offset(next_hop_id_attr_))) {
    LOG(ERROR) << "Router '" << name()
               << "': the 'next_hop_id' attribute is unreadable (is an "
                  "ActionTable writing it?); dropping packets";
  }
  return 0;
}

void Router::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  const bess::metadata::mt_offset_t offset = attr_offset(next_hop_id_attr_);
  if (!bess::metadata::IsValidOffset(offset)) {
    // Fail closed: an unresolved id would send the packet to a guessed gate.
    // OnEvent(PreResume) is where this is reported.
    for (int i = 0; i < cnt; i++) {
      EmitPacket(ctx, batch->packet(i), DROP_GATE);
    }
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
  for (int i = 0; i < cnt; i++) {
    const route::NextHop *hop =
        (resolved & (uint64_t{1} << i)) != 0 ? hops[i] : nullptr;
    const gate_idx_t gate =
        hop != nullptr && hop->neighbor == route::NeighborState::kResolved
            ? hop->egress
            : DROP_GATE;
    EmitPacket(ctx, batch->packet(i), gate);
  }
}

std::string Router::GetDesc() const {
  return bess::utils::Format("%zu routes, %zu next hops",
                             router_->route_count(), router_->next_hop_count());
}

ADD_MODULE(Router, "router",
           "forwards to the egress its next-hop id names (routes and next "
           "hops as transactional resources)")
