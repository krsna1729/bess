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

const Commands VLANPush::cmds = {
    {"set_tci", "VLANPushArg", MODULE_CMD_FUNC(&VLANPush::CommandSetTci),
     Command::THREAD_UNSAFE},
};

CommandResponse VLANPush::Init(const bess::pb::VLANPushArg &arg) {
  return CommandSetTci(arg);
}

CommandResponse VLANPush::CommandSetTci(const bess::pb::VLANPushArg &arg) {
  uint16_t tci = arg.tci();
  vlan_tag_ = be32_t((Ethernet::Type::kVlan << 16) | tci);
  qinq_tag_ = be32_t((Ethernet::Type::kQinQ << 16) | tci);
  return CommandSuccess();
}

// the behavior is undefined if a packet is already double tagged
void VLANPush::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();

  be32_t vlan_tag = vlan_tag_;
  be32_t qinq_tag = qinq_tag_;

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    char *new_head;

    if ((new_head = static_cast<char *>(pkt.prepend(4))) != nullptr) {
      const be16_t tpid =
          reinterpret_cast<const Ethernet *>(new_head + 4)->ether_type;
      const be32_t tag =
          (tpid == be16_t(Ethernet::Type::kVlan)) ? qinq_tag : vlan_tag;
      bess::arch::InsertVlanTag(new_head, tag.raw_value());
    }
  }

  RunNextModule(ctx, batch);
}

std::string VLANPush::GetDesc() const {
  uint32_t vlan_tag = vlan_tag_.value();

  return bess::utils::Format("PCP=%u DEI=%u VID=%u", (vlan_tag >> 13) & 0x0007,
                             (vlan_tag >> 12) & 0x0001, vlan_tag & 0x0fff);
}

ADD_MODULE(VLANPush, "vlan_push", "adds 802.1Q/802.11ad VLAN tag")
