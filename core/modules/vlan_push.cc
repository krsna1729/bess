// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "vlan_push.h"

#include <cstring>

#include "arch/vlan.h"
#include "utils/ether.h"
#include "utils/format.h"

using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::Ethernet;

// "settings" (VLANPushArg) holds the TCI; the class has no actions.
const Commands VLANPush::cmds = {};

const ResourceTables VLANPush::resource_tables = {
    ResourceTable::Singleton<bess::pb::VLANPushArg>("settings", true),
};

CommandResponse VLANPush::Init(const bess::pb::VLANPushArg &arg) {
  vlan_tag_ = be32_t((Ethernet::Type::kVlan << 16) | static_cast<uint16_t>(arg.tci()));
  qinq_tag_ = be32_t((Ethernet::Type::kQinQ << 16) | static_cast<uint16_t>(arg.tci()));
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      // set_tci keeps the low 16 bits; the resource refuses a wider TCI so
      // what it reads back is what was set.
      [](const bess::pb::VLANPushArg &value)
          -> std::expected<uint16_t, std::string> {
        if (value.tci() > 0xffff) {
          return std::unexpected("tci must be in [0, 65535]");
        }
        return static_cast<uint16_t>(value.tci());
      },
      [this](uint16_t &tci) {
        vlan_tag_ = be32_t((Ethernet::Type::kVlan << 16) | tci);
        qinq_tag_ = be32_t((Ethernet::Type::kQinQ << 16) | tci);
      },
      [this] {
        bess::pb::VLANPushArg value;
        value.set_tci(vlan_tag_.value() & 0xffff);
        return value;
      },
      [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void VLANPush::DeInit() {
  settings_.Release();
}

// the behavior is undefined if a packet is already double tagged
void VLANPush::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  be32_t vlan_tag = vlan_tag_;
  be32_t qinq_tag = qinq_tag_;

  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    // The tag decision reads the frame's EtherType: a frame too short to have
    // one cannot be tagged, and must not leave untagged or read past its head.
    if (unlikely(pkt.head_len() < sizeof(Ethernet))) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    char *new_head = static_cast<char *>(pkt.prepend(4));
    if (unlikely(new_head == nullptr)) {
      // No headroom: the frame cannot carry the tag the graph asked for, so it
      // is dropped rather than forwarded bare.
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    const be16_t tpid =
        reinterpret_cast<const Ethernet *>(new_head + 4)->ether_type;
    const be32_t tag =
        (tpid == be16_t(Ethernet::Type::kVlan)) ? qinq_tag : vlan_tag;
    bess::arch::InsertVlanTag(new_head, tag.raw_value());
    batch->handles()[out_cnt++] = pkt.handle();
  }
  batch->set_cnt(out_cnt);

  RunNextModule(ctx, batch);
}

std::string VLANPush::GetDesc() const {
  uint32_t vlan_tag = vlan_tag_.value();

  return bess::utils::Format("PCP=%u DEI=%u VID=%u", (vlan_tag >> 13) & 0x0007,
                             (vlan_tag >> 12) & 0x0001, vlan_tag & 0x0fff);
}

ADD_MODULE(VLANPush, "vlan_push", "adds 802.1Q/802.11ad VLAN tag")
