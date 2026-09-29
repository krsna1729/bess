// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "pcap.h"

#include <limits>
#include <string>
#include <vector>

#include "../utils/pcap.h"

CommandResponse PCAPPort::Init(const bess::pb::PCAPPortArg& arg) {
  if (pcap_handle_.is_initialized()) {
    return CommandFailure(EINVAL, "Device already initialized.");
  }

  const std::string dev = arg.dev();
  pcap_handle_ = PcapHandle(dev);

  if (!pcap_handle_.is_initialized()) {
    return CommandFailure(EINVAL, "Error initializing device.");
  }

  if (pcap_handle_.SetBlocking(false)) {
    return CommandFailure(EINVAL, "Error initializing device.");
  }

  return CommandSuccess();
}

void PCAPPort::DeInit() {
  pcap_handle_.Reset();
}

int PCAPPort::RecvPackets(queue_t qid, bess::PacketHandle *pkts, int cnt) {
  if (!pcap_handle_.is_initialized()) {
    return 0;
  }

  int recv_cnt = 0;

  DCHECK_EQ(qid, 0);

  while (recv_cnt < cnt) {
    int caplen = 0;
    const u_char* packet = pcap_handle_.RecvPacket(&caplen);
    if (!packet) {
      break;
    }

    bess::PacketHandle pkt =
        current_worker.packet_pool()->AllocCopy(packet, caplen);
    if (pkt == nullptr) {
      break;
    }

    pkts[recv_cnt] = pkt;
    recv_cnt++;
  }

  return recv_cnt;
}

int PCAPPort::SendPackets(queue_t, bess::PacketHandle *pkts, int cnt) {
  if (!pcap_handle_.is_initialized()) {
    CHECK(0);  // raise an error
  }

  int sent = 0;
  std::vector<unsigned char> tx_pcap_data;

  while (sent < cnt) {
    bess::PacketRef sbuf(pkts[sent]);
    const uint32_t total_len = sbuf.handle()->pkt_len;

    // PcapHandle::SendPacket() and pcap_sendpacket() accept an int length.
    if (total_len > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
      break;
    }

    const u_char *data = sbuf.head_data<const u_char *>();
    if (sbuf.nb_segs() != 1) {
      tx_pcap_data.resize(total_len);
      GatherData(tx_pcap_data.data(), sbuf);
      data = tx_pcap_data.data();
    }

    if (pcap_handle_.SendPacket(data, static_cast<int>(total_len)) != 0) {
      break;
    }

    sent++;
  }

  bess::PacketFreeBulk(pkts, sent);
  return sent;
}

void PCAPPort::GatherData(unsigned char* data, bess::PacketRef pkt) {
  while (pkt.handle()) {
    bess::utils::CopyInlined(data, pkt.head_data(), pkt.head_len());

    data += pkt.head_len();
    pkt = pkt.next();
  }
}

ADD_DRIVER(PCAPPort, "pcap_port", "libpcap live packet capture from Linux port")
