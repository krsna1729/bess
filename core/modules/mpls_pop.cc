// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "mpls_pop.h"

#include <cstring>

#include "packet_mutation.h"
#include "packet_reshape.h"
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

// A labelled frame is popped or dropped, never forwarded still labelled
// (D-172): its first segment must hold the Ethernet header and the label,
// 18 bytes (a runt, or a chain split inside them, is dropped). A frame too
// short for an Ethernet header has no EtherType to steer by and is dropped,
// as VLANPop drops it. Removing the Ethernet header and the label only moves
// the packet's start, which is valid on a shared payload; keeping the
// Ethernet header rewrites it in place, so a shared payload is copied first
// (D-153) and a frame whose copy fails is dropped.
void MPLSPop::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  constexpr size_t kLabelled = sizeof(Ethernet) + sizeof(Mpls);

  for (bess::PacketRef pkt : batch->packets()) {
    if (unlikely(pkt.head_len() < sizeof(Ethernet))) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    if (pkt.head_data<const Ethernet *>()->ether_type !=
        be16_t(Ethernet::Type::kMpls)) {
      // non MPLS packets are sent to different output gate
      EmitPacket(ctx, pkt, 1);
      continue;
    }

    // TODO(gsagie) save the MPLS label as metadata

    if (remove_eth_header_) {
      if (unlikely(!bess::packet::RemovePrefixInPlace(pkt, kLabelled))) {
        EmitPacket(ctx, pkt, DROP_GATE);
        continue;
      }
      EmitPacket(ctx, pkt, 0);
      continue;
    }

    bess::PacketHandle handle = pkt.handle();
    const auto head = bess::packet::WritableHead(handle, kLabelled);
    if (unlikely(!head)) {
      EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
      continue;
    }
    // The addresses move over the label, and the configured EtherType goes
    // after them.
    std::byte *old_head = head->data();
    std::memmove(old_head + sizeof(Mpls), old_head,
                 sizeof(Ethernet) - sizeof(be16_t));
    const be16_t ether_type = next_ether_type_;
    std::memcpy(old_head + kLabelled - sizeof(be16_t), &ether_type,
                sizeof(ether_type));
    // Cannot fail: the first segment holds kLabelled bytes.
    bess::PacketRef(handle).adj(sizeof(Mpls));
    EmitPacket(ctx, bess::PacketRef(handle), 0);
  }
}

ADD_MODULE(MPLSPop, "mpls_pop", "Pop MPLS label")
