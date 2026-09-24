// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
//   contributors may be used to endorse or promote products derived from this
//   software without specific prior written permission.
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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "control/runtime_state.h"
#include "modules/queue_out.h"
#include "packet_checksum.h"
#include "packet_pool.h"

namespace {

using bess::PacketBatch;
using bess::PacketFree;
using bess::PacketHandle;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::packet::ApplySoftwareChecksums;
using bess::packet::ChecksumPlan;
using bess::packet::IpVersion;
using bess::packet::NetworkChecksum;
using bess::packet::TransportChecksum;

constexpr std::array<uint8_t, 28> kIpv4Udp = {
    0x45, 0x00, 0x00, 0x1c, 0x12, 0x34, 0x40, 0x00, 0x40, 0x11,
    0x12, 0x34, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02,
    0x04, 0xd2, 0x16, 0x2e, 0x00, 0x08, 0x56, 0x78};

constexpr ChecksumPlan kUdpPlan = {
    .network_offset = 0,
    .transport_offset = 20,
    .ip_version = IpVersion::kIpv4,
    .network = NetworkChecksum::kIpv4Header,
    .transport = TransportChecksum::kUdp,
};

std::vector<uint8_t> PacketBytes(PacketHandle packet) {
  std::vector<uint8_t> bytes(packet->pkt_len);
  size_t offset = 0;
  for (PacketHandle segment = packet; segment != nullptr;
       segment = segment->next) {
    std::memcpy(bytes.data() + offset, PacketRef(segment).head_data(),
                segment->data_len);
    offset += segment->data_len;
  }
  return bytes;
}

template <size_t N>
PacketHandle Build(PlainPacketPool &pool,
                   const std::array<uint8_t, N> &bytes) {
  PacketHandle packet = pool.Alloc(N);
  if (packet != nullptr) {
    std::memcpy(PacketRef(packet).head_data(), bytes.data(), N);
  }
  return packet;
}

PacketHandle BuildTruncated(PlainPacketPool &pool) {
  std::array<uint8_t, 16> bytes{};
  bytes[0] = 0x45;
  bytes[2] = 0;
  bytes[3] = 28;
  bytes[9] = 17;
  return Build(pool, bytes);
}

class QueueOutChecksumTestPort final : public Port {
 public:
  CommandResponse Init(const google::protobuf::Any &) {
    return CommandSuccess();
  }
  void DeInit() override {}
  int RecvPackets(queue_t, PacketHandle *, int) override { return 0; }
  int SendPackets(queue_t, PacketHandle *packets, int count) override {
    send_calls++;
    attempted_packets = count;
    sent_packets = std::max(0, count - driver_drops);
    sent_packet = sent_packets == 0 ? nullptr : packets[0];
    sent_bytes = sent_packet == nullptr ? std::vector<uint8_t>{}
                                        : PacketBytes(sent_packet);
    sent_flags = sent_packet == nullptr ? 0 : sent_packet->ol_flags;
    sent_l2_len = sent_packet == nullptr ? 0 : sent_packet->l2_len;
    sent_l3_len = sent_packet == nullptr ? 0 : sent_packet->l3_len;
    sent_l4_len = sent_packet == nullptr ? 0 : sent_packet->l4_len;
    return sent_packets;
  }

  int send_calls = 0;
  int attempted_packets = 0;
  int sent_packets = 0;
  int driver_drops = 0;
  PacketHandle sent_packet = nullptr;
  std::vector<uint8_t> sent_bytes;
  uint64_t sent_flags = 0;
  uint16_t sent_l2_len = 0;
  uint16_t sent_l3_len = 0;
  uint16_t sent_l4_len = 0;
};

ADD_DRIVER(QueueOutChecksumTestPort, "queue_out_checksum_test",
           "QueueOut checksum test port");

class QueueOutChecksumTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto &drivers = PortBuilder::all_port_builders();
    const auto builder = drivers.find("QueueOutChecksumTestPort");
    ASSERT_NE(builder, drivers.end());
    std::unique_ptr<Port> port(builder->second.CreatePort("queue-out-test"));
    port->num_queues[PACKET_DIR_OUT] = 1;
    port_ = static_cast<QueueOutChecksumTestPort *>(port.get());
    ASSERT_TRUE(bess::control::runtime().ports().Add(std::move(port)));
  }

  void TearDown() override { bess::control::runtime().ports().Clear(); }

  QueueOutChecksumTestPort *port_ = nullptr;
};

TEST_F(QueueOutChecksumTest, FinalizesBeforeSendAndSeparatesPreparationDrops) {
  QueueOut output;
  bess::pb::QueueOutArg arg;
  arg.set_port("queue-out-test");
  arg.set_qid(0);
  auto *domain = arg.mutable_tx_checksum_profile()->mutable_outer();
  domain->set_network_offset(kUdpPlan.network_offset);
  domain->set_transport_offset(kUdpPlan.transport_offset);
  domain->set_ip_version(bess::pb::CHECKSUM_IP_VERSION_IPV4);
  domain->set_network(bess::pb::TX_CHECKSUM_NETWORK_IPV4_HEADER);
  domain->set_transport(bess::pb::TX_CHECKSUM_TRANSPORT_UDP);
  ASSERT_FALSE(output.Init(arg).has_error());

  PlainPacketPool pool(8);
  PacketHandle malformed = BuildTruncated(pool);
  PacketHandle sent = Build(pool, kIpv4Udp);
  PacketHandle driver_drop = Build(pool, kIpv4Udp);
  PacketHandle expected = Build(pool, kIpv4Udp);
  ASSERT_NE(malformed, nullptr);
  ASSERT_NE(sent, nullptr);
  ASSERT_NE(driver_drop, nullptr);
  ASSERT_NE(expected, nullptr);
  ASSERT_TRUE(ApplySoftwareChecksums(expected, kUdpPlan).has_value());

  PacketBatch batch;
  batch.clear();
  batch.add(malformed);
  batch.add(sent);
  batch.add(driver_drop);
  port_->driver_drops = 1;
  output.ProcessBatch(nullptr, &batch);
  output.DeInit();

  EXPECT_EQ(port_->send_calls, 1);
  EXPECT_EQ(port_->attempted_packets, 2);
  EXPECT_EQ(port_->sent_packets, 1);
  EXPECT_EQ(port_->sent_packet, sent);
  EXPECT_EQ(port_->sent_bytes, PacketBytes(expected));
  EXPECT_EQ(port_->sent_bytes, PacketBytes(sent));
  EXPECT_EQ(port_->sent_flags, sent->ol_flags);
  EXPECT_EQ(port_->sent_l2_len, sent->l2_len);
  EXPECT_EQ(port_->sent_l3_len, sent->l3_len);
  EXPECT_EQ(port_->sent_l4_len, sent->l4_len);

  const Port::PortStats stats = port_->GetPortStats();
  EXPECT_EQ(stats.out.packets, 1);
  EXPECT_EQ(stats.out.dropped, 2);
  EXPECT_EQ(stats.out.tx_prepare_errors, 1);
  PacketFree(sent);
  PacketFree(expected);
}

TEST_F(QueueOutChecksumTest, NoProfilePreservesPacketBytesAndMetadata) {
  QueueOut output;
  bess::pb::QueueOutArg arg;
  arg.set_port("queue-out-test");
  arg.set_qid(0);
  ASSERT_FALSE(output.Init(arg).has_error());

  PlainPacketPool pool(8);
  PacketHandle packet = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  packet->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM |
                     RTE_MBUF_F_TX_TUNNEL_UDP;
  packet->l2_len = 17;
  packet->l3_len = 23;
  packet->l4_len = 11;
  const auto original_bytes = PacketBytes(packet);
  const uint64_t original_flags = packet->ol_flags;

  PacketBatch batch;
  batch.clear();
  batch.add(packet);
  output.ProcessBatch(nullptr, &batch);
  output.DeInit();

  EXPECT_EQ(port_->send_calls, 1);
  EXPECT_EQ(port_->attempted_packets, 1);
  EXPECT_EQ(port_->sent_packet, packet);
  EXPECT_EQ(port_->sent_bytes, original_bytes);
  EXPECT_EQ(PacketBytes(packet), original_bytes);
  EXPECT_EQ(port_->sent_flags, original_flags);
  EXPECT_EQ(port_->sent_l2_len, 17);
  EXPECT_EQ(port_->sent_l3_len, 23);
  EXPECT_EQ(port_->sent_l4_len, 11);
  const Port::PortStats stats = port_->GetPortStats();
  EXPECT_EQ(stats.out.tx_prepare_errors, 0);
  PacketFree(packet);
}

}  // namespace
