// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "mpls_pop.h"

#include "utils/ether.h"
#include "utils/mpls.h"

using bess::utils::Ethernet;
using bess::utils::Mpls;

// "settings" (MplsPopArg) holds the ether type and header removal; the class
// has no actions.
const Commands MPLSPop::cmds = {};

const ResourceTables MPLSPop::resource_tables = {
    ResourceTable::Singleton<bess::pb::MplsPopArg>("settings", true),
};

// TODO(gsagie) make the next eth type and remove eth header
//              configurable per MPLS label (with default)
MPLSPop::MPLSPop()
    : next_ether_type_(be16_t(Ethernet::Type::kIpv4)),
      remove_eth_header_(false) {
  max_allowed_workers_ = Worker::kMaxWorkers;
}

CommandResponse MPLSPop::Init(const bess::pb::EmptyArg &) {
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      // `set` keeps the low 16 bits; the resource refuses a wider type so
      // what it reads back is what was set.
      [](const bess::pb::MplsPopArg &value)
          -> std::expected<Settings, std::string> {
        if (value.next_eth_type() > 0xffff) {
          return std::unexpected("next_eth_type must be in [0, 65535]");
        }
        return Settings{be16_t(static_cast<uint16_t>(value.next_eth_type())),
                        value.remove_eth_header()};
      },
      [this](Settings &settings) {
        next_ether_type_ = settings.next_ether_type;
        remove_eth_header_ = settings.remove_eth_header;
      },
      [this] {
        bess::pb::MplsPopArg value;
        value.set_remove_eth_header(remove_eth_header_);
        value.set_next_eth_type(next_ether_type_.value());
        return value;
      },
      [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void MPLSPop::DeInit() {
  settings_.Release();
}

void MPLSPop::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  for (bess::PacketRef pkt : batch->packets()) {
    Ethernet *eth = pkt.head_data<Ethernet *>();

    if (eth->ether_type != be16_t(Ethernet::Type::kMpls)) {
      // non MPLS packets are sent to different output gate
      EmitPacket(ctx, pkt, 1);
      continue;
    }

    // TODO(gsagie) save the MPLS label as metadata
    // Mpls *mpls = reinterpret_cast<Mpls *>(eth_header + 1);

    // TODO(gsagie) convert this to be more efficient using Intel instructions
    if (remove_eth_header_) {
      pkt.adj(sizeof(Ethernet) + sizeof(Mpls));
    } else {
      Ethernet::Address src_addr = eth->src_addr;
      Ethernet::Address dst_addr = eth->dst_addr;

      pkt.adj(sizeof(Mpls));
      Ethernet *eth_new = pkt.head_data<Ethernet *>();
      eth_new->src_addr = src_addr;
      eth_new->dst_addr = dst_addr;
      eth_new->ether_type = next_ether_type_;
    }
    EmitPacket(ctx, pkt, 0);
  }
}

ADD_MODULE(MPLSPop, "mpls_pop", "Pop MPLS label")
