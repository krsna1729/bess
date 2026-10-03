// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "ip_checksum.h"

#include "utils/checksum.h"
#include "utils/ether.h"
#include "utils/ip.h"

enum { FORWARD_GATE = 0, FAIL_GATE };

void IPChecksum::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Vlan;

  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    Ethernet *eth = pkt.head_data<Ethernet *>();
    void *data = eth + 1;
    Ipv4 *ip;

    be16_t ether_type = eth->ether_type;

    if (ether_type == be16_t(Ethernet::Type::kQinQ)) {
      Vlan *qinq = reinterpret_cast<Vlan *>(data);
      data = qinq + 1;
      ether_type = qinq->ether_type;
      if (ether_type != be16_t(Ethernet::Type::kVlan)) {
        EmitPacket(ctx, pkt, FORWARD_GATE);
	continue;
      }
    }

    if (ether_type == be16_t(Ethernet::Type::kVlan)) {
      Vlan *vlan = reinterpret_cast<Vlan *>(data);
      data = vlan + 1;
      ether_type = vlan->ether_type;
    }

    if (ether_type == be16_t(Ethernet::Type::kIpv4)) {
      ip = reinterpret_cast<Ipv4 *>(data);
    } else {
      EmitPacket(ctx, pkt, FORWARD_GATE);
      continue;
    }

    if (verify_) {
      EmitPacket(ctx, pkt, (VerifyIpv4Checksum(*ip)) ? FORWARD_GATE : FAIL_GATE);
    } else {
      ip->checksum = CalculateIpv4Checksum(*ip);
      EmitPacket(ctx, pkt, FORWARD_GATE);
    }
  }
}

CommandResponse IPChecksum::Init(const bess::pb::IPChecksumArg &arg) {
  verify_ = arg.verify();
  return CommandSuccess();
}

ADD_MODULE(IPChecksum, "ip_checksum", "recomputes the IPv4 checksum")
