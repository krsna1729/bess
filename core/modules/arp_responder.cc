// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "arp_responder.h"

#include "../utils/arp.h"

using bess::utils::Arp;
using bess::utils::be16_t;

const Commands ArpResponder::cmds = {
    {"add", "ArpResponderArg", MODULE_CMD_FUNC(&ArpResponder::CommandAdd),
     Command::THREAD_UNSAFE}};

CommandResponse ArpResponder::Init(const bess::pb::ArpResponderArg &arg) {
  if (arg.ip().empty() && arg.mac_addr().empty()) {
    return CommandSuccess();
  }
  return CommandAdd(arg);
}

CommandResponse ArpResponder::CommandAdd(const bess::pb::ArpResponderArg &arg) {
  be32_t ip_addr;
  arp_entry entry;

  if (!arg.ip().length()) {
    return CommandFailure(EINVAL, "IP address is missing");
  }
  if (!bess::utils::ParseIpv4Address(arg.ip(), &ip_addr)) {
    return CommandFailure(EINVAL, "Invalid IP Address: %s", arg.ip().c_str());
  }

  if (!entry.mac_addr.FromString(arg.mac_addr())) {
    return CommandFailure(EINVAL, "Invalid MAC Address: %s",
                          arg.mac_addr().c_str());
  }

  entry.ip_addr = ip_addr;
  entries_[ip_addr] = entry;
  return CommandSuccess();
}

void ArpResponder::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    // we should drop-or-emit each packet
    bess::PacketRef pkt = batch->packet(i);

    Ethernet *eth = pkt.head_data<Ethernet *>();
    if (eth->ether_type != be16_t(Ethernet::Type::kArp)) {
      // Currently drop all non ARP packets
      DropPacket(ctx, pkt);
      continue;
    }

    Arp *arp = reinterpret_cast<Arp *>(eth + 1);
    if (arp->opcode == be16_t(Arp::Opcode::kRequest)) {
      // TODO(galsagie) When learn is added, learn SRC MAC here

      // Try to find target IP in cache, if exists convert request to reply
      auto it = entries_.find(arp->target_ip_addr);
      if (it != entries_.end()) {
        const struct arp_entry &entry = it->second;
        arp->opcode = be16_t(Arp::Opcode::kReply);

        eth->dst_addr = eth->src_addr;
        eth->src_addr = entry.mac_addr;

        arp->target_hw_addr = arp->sender_hw_addr;
        arp->sender_hw_addr = entry.mac_addr;

        arp->target_ip_addr = arp->sender_ip_addr;
        arp->sender_ip_addr = entry.ip_addr;
        EmitPacket(ctx, pkt, 0);
      } else {
        // Did not find an ARP entry in cache, drop packet
        // TODO(galsagie) Optinally emit packet to next module here
        DropPacket(ctx, pkt);
      }
    } else if (arp->opcode == be16_t(Arp::Opcode::kReply)) {
      // TODO(galsagie) When learn is added, learn SRC MAC here
      DropPacket(ctx, pkt);
    } else {
      // TODO(galsagie) Other opcodes are not handled yet.
      DropPacket(ctx, pkt);
    }
  }
}

ADD_MODULE(ArpResponder, "arp_responder",
           "Respond to ARP requests and learns new MAC's")
