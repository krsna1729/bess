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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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

// DRR's flow admission (D-019): a new flow ends up both in the flow map and
// on the round-robin ring, or in neither, and every packet is either queued or
// freed -- under a failure at each step (deterministic fault injection), not
// only on the stress path.

#include "modules/drr.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <vector>

#include "packet_pool.h"

class DrrTestPeer {
 public:
  using Faults = DRR::Faults;

  static void Setup(DRR &drr) {
    drr.max_number_flows_ = 64;
    ASSERT_EQ(drr.AllocateRings().error().code(), 0);
  }
  static Faults &faults(DRR &drr) { return drr.faults_; }
  using FlowId = DRR::FlowId;
  static FlowId IdOf(DRR &drr, bess::PacketRef pkt) { return drr.GetId(pkt); }
  static void Drain(DRR &drr) { drr.DrainIngress(DRR::kIngressPerRun); }
  static size_t Mapped(const DRR &drr) { return drr.flows_.Count(); }
  static size_t Scheduled(const DRR &drr) {
    return rte_ring_count(drr.flow_ring_) + (drr.current_flow_ ? 1 : 0);
  }
  // Every mapped flow is on the round-robin ring exactly once, and nothing
  // else is.
  static bool MapMatchesRing(DRR &drr) {
    std::set<const void *> mapped;
    for (const auto &entry : drr.flows_) {
      mapped.insert(entry.second);
    }
    std::vector<void *> ring(rte_ring_count(drr.flow_ring_));
    const unsigned n = rte_ring_sc_dequeue_bulk(drr.flow_ring_, ring.data(),
                                                ring.size(), nullptr);
    std::set<const void *> scheduled(ring.begin(), ring.begin() + n);
    rte_ring_sp_enqueue_bulk(drr.flow_ring_, ring.data(), n, nullptr);
    return n == ring.size() && scheduled.size() == n && mapped == scheduled;
  }
};

namespace {

// Ethernet + IPv4 + UDP, one flow per `flow`.
bess::PacketHandle FlowPacket(bess::PlainPacketPool &pool, uint8_t flow) {
  bess::PacketHandle pkt = pool.Alloc(60);
  if (pkt == nullptr) {
    return nullptr;
  }
  uint8_t *p = bess::PacketRef(pkt).head_data<uint8_t *>();
  std::memset(p, 0, 60);
  p[12] = 0x08;           // IPv4
  p[14] = 0x45;           // version 4, 20-byte header
  p[16] = 0; p[17] = 46;  // total length
  p[23] = 17;             // UDP
  p[26] = 10; p[29] = flow;          // source 10.0.0.<flow>
  p[30] = 10; p[31] = 1; p[33] = 1;  // destination 10.1.0.1
  p[35] = 53; p[37] = 53;            // ports
  return pkt;
}

void Send(DRR &drr, bess::PlainPacketPool &pool, uint8_t first, int n) {
  bess::PacketBatch batch;
  batch.clear();
  for (int i = 0; i < n; i++) {
    bess::PacketHandle pkt = FlowPacket(pool, static_cast<uint8_t>(first + i));
    ASSERT_NE(pkt, nullptr);
    batch.add(bess::PacketRef(pkt));
  }
  drr.ProcessBatch(nullptr, &batch);
  DrrTestPeer::Drain(drr);
}

struct Case {
  const char *name;
  bool DrrTestPeer::Faults::*fault;
  bool flow_survives;  // only the first packet is lost
};

class DrrAdmissionTest : public ::testing::TestWithParam<Case> {};

TEST_P(DrrAdmissionTest, FailedStepLeavesNoHalfCreatedFlowAndNoLeak) {
  bess::PlainPacketPool pool(256);
  const size_t capacity = pool.Size();
  {
    DRR drr;
    DrrTestPeer::Setup(drr);
    DrrTestPeer::faults(drr).*GetParam().fault = true;
    Send(drr, pool, 1, 4);
    const size_t expected = GetParam().flow_survives ? 4 : 0;
    EXPECT_EQ(DrrTestPeer::Mapped(drr), expected);
    EXPECT_EQ(DrrTestPeer::Scheduled(drr), expected);
    EXPECT_TRUE(DrrTestPeer::MapMatchesRing(drr));
    EXPECT_EQ(pool.Size(), capacity) << "a packet of a failed admission leaked";

    // With the fault cleared the same flows are admitted normally.
    DrrTestPeer::faults(drr).*GetParam().fault = false;
    Send(drr, pool, 1, 4);
    EXPECT_EQ(DrrTestPeer::Mapped(drr), 4u);
    EXPECT_EQ(DrrTestPeer::Scheduled(drr), 4u);
    EXPECT_TRUE(DrrTestPeer::MapMatchesRing(drr));
    EXPECT_EQ(pool.Size(), capacity - 4) << "four packets queued";
  }
  EXPECT_EQ(pool.Size(), capacity) << "destroying DRR freed every packet";
}

INSTANTIATE_TEST_SUITE_P(
    EachStep, DrrAdmissionTest,
    ::testing::Values(Case{"QueueAlloc", &DrrTestPeer::Faults::queue_alloc, false},
                      Case{"MapInsert", &DrrTestPeer::Faults::map_insert, false},
                      Case{"RingEnqueue", &DrrTestPeer::Faults::ring_enqueue, false},
                      Case{"FirstPacket", &DrrTestPeer::Faults::first_enqueue, true}),
    [](const ::testing::TestParamInfo<Case> &info) {
      return std::string(info.param.name);
    });

bess::PacketHandle Bytes(bess::PlainPacketPool &pool,
                        const std::vector<uint8_t> &bytes) {
  bess::PacketHandle pkt = pool.Alloc(bytes.size());
  if (pkt != nullptr && !bytes.empty()) {
    std::memcpy(bess::PacketRef(pkt).head_data<uint8_t *>(), bytes.data(),
                bytes.size());
  }
  return pkt;
}

// The same bytes as a two-segment chain split at `at`.
bess::PacketHandle Chained(bess::PlainPacketPool &pool,
                          const std::vector<uint8_t> &bytes, size_t at) {
  bess::PacketHandle head = Bytes(
      pool, std::vector<uint8_t>(bytes.begin(), bytes.begin() + at));
  bess::PacketHandle tail =
      Bytes(pool, std::vector<uint8_t>(bytes.begin() + at, bytes.end()));
  if (head == nullptr || tail == nullptr || rte_pktmbuf_chain(head, tail)) {
    return nullptr;
  }
  return head;
}

std::vector<uint8_t> Ipv4Udp(uint8_t ihl = 5, uint16_t frag = 0x4000,
                             uint8_t proto = 17) {
  std::vector<uint8_t> p(14 + ihl * 4 + 8, 0);
  p[12] = 0x08;
  p[14] = static_cast<uint8_t>(0x40 | ihl);
  p[20] = static_cast<uint8_t>(frag >> 8);
  p[21] = static_cast<uint8_t>(frag);
  p[23] = proto;
  p[26] = 10; p[29] = 7;
  p[30] = 10; p[33] = 9;
  const size_t l4 = 14 + ihl * 4;
  p[l4 + 1] = 53;
  p[l4 + 3] = 80;
  return p;
}

bool Same(const DrrTestPeer::FlowId &a, const DrrTestPeer::FlowId &b) {
  return DRR::EqualTo()(a, b);
}

TEST(DrrFlowIdTest, WellFormedIpv4GivesTheFiveTuple) {
  bess::PlainPacketPool pool(64);
  DRR drr;
  for (uint8_t ihl : {5, 6, 15}) {
    bess::PacketHandle pkt = Bytes(pool, Ipv4Udp(ihl));
    const auto id = DrrTestPeer::IdOf(drr, bess::PacketRef(pkt));
    EXPECT_EQ(id.src_ip, 0x0a000007u);
    EXPECT_EQ(id.dst_ip, 0x0a000009u);
    EXPECT_EQ(id.protocol, 17);
    EXPECT_EQ(id.src_port, 53u) << "ihl " << int(ihl);
    EXPECT_EQ(id.dst_port, 80u);
    bess::PacketFree(pkt);
  }
}

TEST(DrrFlowIdTest, MalformedOrForeignPacketsShareTheFallbackFlow) {
  bess::PlainPacketPool pool(64);
  DRR drr;
  auto ip = Ipv4Udp();
  std::vector<std::vector<uint8_t>> cases = {
      {},                                            // empty
      std::vector<uint8_t>(ip.begin(), ip.begin() + 13),  // no EtherType
      std::vector<uint8_t>(ip.begin(), ip.begin() + 30),  // IPv4 cut short
  };
  auto arp = ip;
  arp[13] = 0x06;
  cases.push_back(arp);
  auto bad_ihl = ip;
  bad_ihl[14] = 0x44;
  cases.push_back(bad_ihl);
  auto v6 = ip;
  v6[14] = 0x65;
  cases.push_back(v6);
  for (size_t i = 0; i < cases.size(); i++) {
    bess::PacketHandle pkt = Bytes(pool, cases[i]);
    ASSERT_NE(pkt, nullptr);
    EXPECT_TRUE(Same(DrrTestPeer::IdOf(drr, bess::PacketRef(pkt)),
                     DrrTestPeer::FlowId{}))
        << "case " << i;
    bess::PacketFree(pkt);
  }
}

TEST(DrrFlowIdTest, PortsOnlyForFirstFragmentsThatCarryThem) {
  bess::PlainPacketPool pool(64);
  DRR drr;
  // Non-first fragment: no transport header, ports 0.
  bess::PacketHandle frag = Bytes(pool, Ipv4Udp(5, 0x00b9));
  auto id = DrrTestPeer::IdOf(drr, bess::PacketRef(frag));
  EXPECT_EQ(id.src_ip, 0x0a000007u);
  EXPECT_EQ(id.src_port, 0u);
  EXPECT_EQ(id.dst_port, 0u);
  // Truncated inside the UDP ports.
  auto cut = Ipv4Udp();
  cut.resize(14 + 20 + 3);
  bess::PacketHandle shortl4 = Bytes(pool, cut);
  id = DrrTestPeer::IdOf(drr, bess::PacketRef(shortl4));
  EXPECT_EQ(id.dst_ip, 0x0a000009u);
  EXPECT_EQ(id.src_port, 0u);
  // ICMP: no ports.
  bess::PacketHandle icmp = Bytes(pool, Ipv4Udp(5, 0x4000, 1));
  id = DrrTestPeer::IdOf(drr, bess::PacketRef(icmp));
  EXPECT_EQ(id.protocol, 1);
  EXPECT_EQ(id.src_port, 0u);
  bess::PacketFree(frag);
  bess::PacketFree(shortl4);
  bess::PacketFree(icmp);
}

TEST(DrrFlowIdTest, ChainedPacketsGiveTheSameFlowAtEverySplit) {
  bess::PlainPacketPool pool(64);
  DRR drr;
  const auto bytes = Ipv4Udp(6);
  bess::PacketHandle whole = Bytes(pool, bytes);
  const auto expected = DrrTestPeer::IdOf(drr, bess::PacketRef(whole));
  bess::PacketFree(whole);
  for (size_t at = 1; at < bytes.size(); at++) {
    bess::PacketHandle pkt = Chained(pool, bytes, at);
    ASSERT_NE(pkt, nullptr);
    EXPECT_TRUE(Same(DrrTestPeer::IdOf(drr, bess::PacketRef(pkt)), expected))
        << "split at " << at;
    bess::PacketFree(pkt);
  }
}

TEST(DrrIngressTest, FullIngressDropsAndFreesTheExcess) {
  bess::PlainPacketPool pool(DRR::kIngressSize + 64);
  const size_t capacity = pool.Size();
  {
    DRR drr;
    DrrTestPeer::Setup(drr);
    // Fill the ingress without draining it.
    for (int sent = 0; sent < DRR::kIngressSize + 32; sent += 32) {
      bess::PacketBatch batch;
      batch.clear();
      for (int i = 0; i < 32; i++) {
        batch.add(bess::PacketRef(FlowPacket(pool, 1)));
      }
      drr.ProcessBatch(nullptr, &batch);
    }
    // rte_ring holds size - 1 entries.
    EXPECT_EQ(pool.Size(), capacity - (DRR::kIngressSize - 1));
  }
  EXPECT_EQ(pool.Size(), capacity);
}

}  // namespace
