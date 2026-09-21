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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#include "pcap.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>
#include <pcap/pcap.h>

#include "../packet_pool.h"

TEST(PCAPPortTest, OversizedChainedPacketIsNotReportedSent) {
  pcap_t *dead_handle = pcap_open_dead(DLT_EN10MB, 65535);
  ASSERT_NE(dead_handle, nullptr);

  PCAPPort port;
  port.pcap_handle_ = PcapHandle(dead_handle);

  bess::PlainPacketPool pool(32, -1, 4096);
  std::vector<uint8_t> payload(65536, 0xa5);
  bess::PacketHandle pkt = pool.AllocCopy(payload.data(), payload.size());
  ASSERT_NE(pkt, nullptr);
  ASSERT_GT(pkt->nb_segs, 1);

  // A dead libpcap handle rejects sends. The regression was that the old
  // PCAP_SNAPLEN branch skipped this call and still reported success.
  const int sent = port.SendPackets(0, &pkt, 1);
  EXPECT_EQ(sent, 0);

  if (sent == 0) {
    bess::PacketFree(pkt);
  }
  EXPECT_EQ(pool.Size(), pool.Capacity());
}
