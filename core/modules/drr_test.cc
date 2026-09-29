// SPDX-License-Identifier: BSD-3-Clause

// DRR's flow admission (D-019): a new flow ends up both in the flow map and
// on the round-robin ring, or in neither, and every packet is either queued or
// freed -- under a failure at each step (deterministic fault injection), not
// only on the stress path.

#include "modules/drr.h"

#include <gtest/gtest.h>

#include <cstdlib>
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
  static void SetQuantum(DRR &drr, uint32_t q) {
    ASSERT_EQ(drr.SetQuantumSize(q).error().code(), 0);
  }
  static uint32_t NextBatch(DRR &drr, bess::PacketBatch *batch) {
    int err = 0;
    batch->clear();
    const uint32_t bytes = drr.GetNextBatch(batch, &err);
    EXPECT_GE(err, 0);
    return bytes;
  }
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

// A packet of flow `flow`, `len` bytes long.
bess::PacketHandle SizedFlowPacket(bess::PlainPacketPool &pool, uint8_t flow,
                                   size_t len) {
  bess::PacketHandle pkt = FlowPacket(pool, flow);
  if (pkt != nullptr) {
    rte_pktmbuf_append(pkt, static_cast<uint16_t>(len - 60));
  }
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

// A flow whose queue fills is grown; if that allocation fails, the flow keeps
// its queue and packets and only the new packet is dropped (the queue used to
// become null and leak).
TEST(DrrQueueTest, FailedGrowthKeepsTheFlowsQueue) {
  constexpr int kPackets = DRR::kFlowQueueSize + 64;
  bess::PlainPacketPool pool(kPackets + 64);
  const size_t capacity = pool.Size();
  {
    DRR drr;
    DrrTestPeer::Setup(drr);
    DrrTestPeer::faults(drr).resize_alloc = true;
    for (int sent = 0; sent < kPackets; sent += 32) {
      bess::PacketBatch batch;
      batch.clear();
      for (int i = 0; i < 32; i++) {
        batch.add(bess::PacketRef(FlowPacket(pool, 1)));
      }
      drr.ProcessBatch(nullptr, &batch);
      DrrTestPeer::Drain(drr);
    }
    EXPECT_EQ(DrrTestPeer::Mapped(drr), 1u);
    // The initial ring holds size - 1 packets; the rest were dropped.
    EXPECT_EQ(pool.Size(), capacity - (DRR::kFlowQueueSize - 1));

    // With growth working again the queue grows past its first size.
    DrrTestPeer::faults(drr).resize_alloc = false;
    bess::PacketBatch batch;
    batch.clear();
    for (int i = 0; i < 32; i++) {
      batch.add(bess::PacketRef(FlowPacket(pool, 1)));
    }
    drr.ProcessBatch(nullptr, &batch);
    DrrTestPeer::Drain(drr);
    EXPECT_EQ(pool.Size(), capacity - (DRR::kFlowQueueSize - 1) - 32);
  }
  EXPECT_EQ(pool.Size(), capacity) << "destroying DRR freed every packet";
}

// Deficit round robin: backlogged flows get equal bytes whatever their packet
// sizes, to within one quantum plus one packet.
TEST(DrrSchedulingTest, BackloggedFlowsGetEqualBytes) {
  bess::PlainPacketPool pool(1024, -1, 2048);
  const size_t capacity = pool.Size();
  {
    DRR drr;
    DrrTestPeer::Setup(drr);
    DrrTestPeer::SetQuantum(drr, 1000);
    // Flow 1: 1000-byte packets; flow 2: 100-byte packets. Both stay
    // backlogged for the whole measurement.
    for (int round = 0; round < 10; round++) {
      bess::PacketBatch batch;
      batch.clear();
      for (int i = 0; i < 8; i++) {
        batch.add(bess::PacketRef(SizedFlowPacket(pool, 1, 1000)));
      }
      for (int i = 0; i < 24; i++) {
        batch.add(bess::PacketRef(SizedFlowPacket(pool, 2, 100)));
      }
      drr.ProcessBatch(nullptr, &batch);
      DrrTestPeer::Drain(drr);
    }
    size_t bytes[3] = {0, 0, 0};
    size_t total = 0;
    while (total < 20000) {
      bess::PacketBatch out;
      const uint32_t got = DrrTestPeer::NextBatch(drr, &out);
      ASSERT_GT(got, 0u) << "a backlogged DRR produced nothing";
      for (int i = 0; i < out.cnt(); i++) {
        bess::PacketRef pkt = out.packet(i);
        const uint8_t flow = pkt.head_data<uint8_t *>()[29];
        ASSERT_TRUE(flow == 1 || flow == 2);
        bytes[flow] += pkt.total_len();
        total += pkt.total_len();
      }
      bess::PacketFreeBatch(&out);
    }
    const long diff = static_cast<long>(bytes[1]) - static_cast<long>(bytes[2]);
    EXPECT_LE(std::abs(diff), 1000 + 1000)
        << "flow 1: " << bytes[1] << " bytes, flow 2: " << bytes[2];
  }
  EXPECT_EQ(pool.Size(), capacity);
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
