// Copyright (c) 2017, The Regents of the University of California.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "l4_checksum.h"

#include "../utils/checksum.h"
#include "../utils/ether.h"
#include "../utils/ip.h"
#include "../utils/tcp.h"
#include "../utils/udp.h"

enum { FORWARD_GATE = 0, FAIL_GATE };

void L4Checksum::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Tcp;
  using bess::utils::Udp;

  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    Ethernet *eth = pkt.head_data<Ethernet *>();

    // Calculate checksum only for IPv4 packets
    if (eth->ether_type != be16_t(Ethernet::Type::kIpv4)) {
      EmitPacket(ctx, pkt, FORWARD_GATE);
      continue;
    }

    Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);

    if (ip->protocol == Ipv4::Proto::kUdp) {
      size_t ip_bytes = (ip->header_length) << 2;
      Udp *udp =
          reinterpret_cast<Udp *>(reinterpret_cast<uint8_t *>(ip) + ip_bytes);
      if (verify_) {
        EmitPacket(ctx, pkt,
                   (VerifyIpv4UdpChecksum(*ip, *udp)) ? FORWARD_GATE : FAIL_GATE);
      } else {
	udp->checksum = CalculateIpv4UdpChecksum(*ip, *udp);
        EmitPacket(ctx, pkt, FORWARD_GATE);
      }
    } else if (ip->protocol == Ipv4::Proto::kTcp) {
      size_t ip_bytes = (ip->header_length) << 2;
      Tcp *tcp =
          reinterpret_cast<Tcp *>(reinterpret_cast<uint8_t *>(ip) + ip_bytes);
      if (verify_)
        EmitPacket(ctx, pkt,
                   (VerifyIpv4TcpChecksum(*ip, *tcp)) ? FORWARD_GATE : FAIL_GATE);
      else
	tcp->checksum = CalculateIpv4TcpChecksum(*ip, *tcp);
    }
  }
}

CommandResponse L4Checksum::Init(const bess::pb::L4ChecksumArg &arg) {
  verify_ = arg.verify();
  return CommandSuccess();
}

ADD_MODULE(L4Checksum, "l4_checksum",
           "recomputes the TCP/Ipv4 and UDP/IPv4 checksum")
