// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DRIVERS_PCAP_H_
#define BESS_DRIVERS_PCAP_H_

#include "../port.h"
#include <gtest/gtest_prod.h>


#include <glog/logging.h>

#include "../utils/pcap_handle.h"

// Port to connect to a device via PCAP.
// (Not recommended because PCAP is slow :-)
// Captured packets are copied into one or more native mbuf segments.
class PCAPPort final : public Port {
 public:
  CommandResponse Init(const bess::pb::PCAPPortArg &arg);

  void DeInit() override;
  // PCAP has no notion of queue so unlike parent (port.cc) quid is ignored.
  int SendPackets(queue_t qid, bess::PacketHandle *pkts, int cnt) override;
  // Ditto above: quid is ignored.
  int RecvPackets(queue_t qid, bess::PacketHandle *pkts, int cnt) override;

 private:
  FRIEND_TEST(PCAPPortTest, OversizedChainedPacketIsNotReportedSent);

  void GatherData(unsigned char *data, bess::PacketRef pkt);
  PcapHandle pcap_handle_;
};

#endif  // BESS_DRIVERS_PCAP_H_
