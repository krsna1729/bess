// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met.
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor those of the contributors
//   may be used to endorse or promote products derived from this software
//   without specific prior written permission.
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
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <rte_ip.h>

#include "packet.h"
#include "packet_checksum.h"
#include "packet_pool.h"
#include "utils/checksum.h"

namespace {

using bess::PacketFree;
using bess::PacketHandle;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::packet::ApplySoftwareChecksums;
using bess::packet::ChecksumError;
using bess::packet::ChecksumPlan;
using bess::packet::ComputeChecksums;
using bess::packet::IpVersion;
using bess::packet::NetworkChecksum;
using bess::packet::TransportChecksum;

constexpr std::array<uint8_t, 28> kIpv4Udp = {
    0x45, 0x00, 0x00, 0x1c, 0x12, 0x34, 0x40, 0x00, 0x40, 0x11,
    0x00, 0x00, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02,
    0x04, 0xd2, 0x16, 0x2e, 0x00, 0x08, 0x00, 0x00};
constexpr std::array<uint8_t, 48> kIpv6Udp = {
    0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x11, 0x40, 0x20, 0x01, 0x0d, 0xb8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x04, 0xd2, 0x16, 0x2e, 0x00, 0x08, 0x00, 0x00};

PacketHandle BuildChain(PlainPacketPool &pool,
                        std::span<const size_t> segment_lengths,
                        std::span<const uint8_t> bytes) {
  PacketHandle head = nullptr;
  PacketHandle previous = nullptr;
  size_t offset = 0;
  for (size_t len : segment_lengths) {
    PacketHandle segment = pool.Alloc(len);
    if (segment == nullptr) {
      if (head != nullptr)
        PacketFree(head);
      return nullptr;
    }
    if (head == nullptr)
      head = segment;
    else
      previous->next = segment;
    previous = segment;
    if (len != 0)
      std::memcpy(PacketRef(segment).head_data(), bytes.data() + offset, len);
    offset += len;
  }
  if (head == nullptr || offset != bytes.size()) {
    if (head != nullptr)
      PacketFree(head);
    return nullptr;
  }
  head->pkt_len = static_cast<uint32_t>(bytes.size());
  head->nb_segs = static_cast<uint16_t>(segment_lengths.size());
  return head;
}

PacketHandle Build(PlainPacketPool &pool, std::span<const uint8_t> bytes) {
  const size_t size = bytes.size();
  return BuildChain(pool, std::span<const size_t>(&size, 1), bytes);
}

std::vector<uint8_t> Bytes(PacketHandle packet) {
  std::vector<uint8_t> result(packet->pkt_len);
  size_t offset = 0;
  for (PacketHandle seg = packet; seg != nullptr; seg = seg->next) {
    std::memcpy(result.data() + offset, PacketRef(seg).head_data(),
                seg->data_len);
    offset += seg->data_len;
  }
  return result;
}

uint16_t HostValue(bess::utils::be16_t value) {
  return value.value();
}

ChecksumPlan Ipv4UdpPlan() {
  return {.network_offset = 0,
          .transport_offset = 20,
          .ip_version = IpVersion::kIpv4,
          .network = NetworkChecksum::kIpv4Header,
          .transport = TransportChecksum::kUdp};
}

TEST(PacketChecksumTest, FixedIpv4AndIpv6VectorsMatchIndependentChecksums) {
  PlainPacketPool pool(8);
  PacketHandle v4 = Build(pool, kIpv4Udp);
  ASSERT_NE(v4, nullptr);
  auto values = ComputeChecksums(PacketRef(v4), Ipv4UdpPlan());
  ASSERT_TRUE(values.has_value());
  EXPECT_EQ(HostValue(*values->network), 0x3c66);
  EXPECT_EQ(HostValue(*values->transport), 0xf8a6);
  auto *ip4 =
      reinterpret_cast<rte_ipv4_hdr *>(const_cast<uint8_t *>(kIpv4Udp.data()));
  auto *udp4 = reinterpret_cast<struct rte_udp_hdr *>(
      const_cast<uint8_t *>(kIpv4Udp.data() + 20));
  auto *bess_ip4 = reinterpret_cast<const bess::utils::Ipv4 *>(kIpv4Udp.data());
  auto *bess_udp4 =
      reinterpret_cast<const bess::utils::Udp *>(kIpv4Udp.data() + 20);
  EXPECT_EQ(values->network->raw_value(), rte_ipv4_cksum(ip4));
  EXPECT_EQ(values->transport->raw_value(), rte_ipv4_udptcp_cksum(ip4, udp4));
  EXPECT_EQ(values->network->raw_value(),
            bess::utils::CalculateIpv4Checksum(*bess_ip4));
  EXPECT_EQ(values->transport->raw_value(),
            bess::utils::CalculateIpv4UdpChecksum(*bess_ip4, *bess_udp4));
  PacketFree(v4);

  PacketHandle v6 = Build(pool, kIpv6Udp);
  ASSERT_NE(v6, nullptr);
  ChecksumPlan plan{.network_offset = 0,
                    .transport_offset = 40,
                    .ip_version = IpVersion::kIpv6,
                    .network = NetworkChecksum::kNone,
                    .transport = TransportChecksum::kUdp};
  auto values6 = ComputeChecksums(PacketRef(v6), plan);
  ASSERT_TRUE(values6.has_value());
  EXPECT_FALSE(values6->network.has_value());
  EXPECT_EQ(HostValue(*values6->transport), 0x8969);
  auto *ip6 =
      reinterpret_cast<rte_ipv6_hdr *>(const_cast<uint8_t *>(kIpv6Udp.data()));
  auto *udp6 = reinterpret_cast<struct rte_udp_hdr *>(
      const_cast<uint8_t *>(kIpv6Udp.data() + 40));
  EXPECT_EQ(values6->transport->raw_value(), rte_ipv6_udptcp_cksum(ip6, udp6));
  PacketFree(v6);
}
TEST(PacketChecksumTest, EncodesComputedZeroUdpChecksumsAsOnes) {
  PlainPacketPool pool(8);
  std::array<uint8_t, 30> ipv4{};
  std::copy(kIpv4Udp.begin(), kIpv4Udp.end(), ipv4.begin());
  ipv4[2] = 0;
  ipv4[3] = ipv4.size();
  ipv4[24] = 0;
  ipv4[25] = 10;
  ipv4[28] = 0xf8;
  ipv4[29] = 0xa2;
  PacketHandle packet = Build(pool, ipv4);
  ASSERT_NE(packet, nullptr);
  auto values = ComputeChecksums(PacketRef(packet), Ipv4UdpPlan());
  ASSERT_TRUE(values.has_value());
  EXPECT_EQ(HostValue(*values->transport), 0xffff);
  ASSERT_TRUE(ApplySoftwareChecksums(packet, Ipv4UdpPlan()).has_value());
  auto bytes = Bytes(packet);
  EXPECT_EQ(bytes[26], 0xff);
  EXPECT_EQ(bytes[27], 0xff);
  PacketFree(packet);

  std::array<uint8_t, 50> ipv6{};
  std::copy(kIpv6Udp.begin(), kIpv6Udp.end(), ipv6.begin());
  ipv6[4] = 0;
  ipv6[5] = 10;
  ipv6[44] = 0;
  ipv6[45] = 10;
  ipv6[48] = 0x89;
  ipv6[49] = 0x65;
  packet = Build(pool, ipv6);
  ASSERT_NE(packet, nullptr);
  ChecksumPlan plan{.network_offset = 0,
                    .transport_offset = 40,
                    .ip_version = IpVersion::kIpv6,
                    .network = NetworkChecksum::kNone,
                    .transport = TransportChecksum::kUdp};
  values = ComputeChecksums(PacketRef(packet), plan);
  ASSERT_TRUE(values.has_value());
  EXPECT_EQ(HostValue(*values->transport), 0xffff);
  ASSERT_TRUE(ApplySoftwareChecksums(packet, plan).has_value());
  bytes = Bytes(packet);
  EXPECT_EQ(bytes[46], 0xff);
  EXPECT_EQ(bytes[47], 0xff);
  PacketFree(packet);
}

TEST(PacketChecksumTest, Ipv4TcpMatchesBessAndDpdkChecksums) {
  PlainPacketPool pool(8);
  std::array<uint8_t, 40> wire{};
  std::copy(kIpv4Udp.begin(), kIpv4Udp.begin() + 20, wire.begin());
  wire[2] = 0;
  wire[3] = 40;
  wire[9] = 6;
  wire[10] = wire[11] = 0;
  wire[20] = 0x04;
  wire[21] = 0xd2;
  wire[22] = 0x16;
  wire[23] = 0x2e;
  wire[32] = 0x50;
  wire[33] = 0x02;
  wire[34] = 0x20;
  wire[35] = 0x00;
  PacketHandle packet = Build(pool, wire);
  ASSERT_NE(packet, nullptr);
  ChecksumPlan plan = Ipv4UdpPlan();
  plan.transport = TransportChecksum::kTcp;
  auto values = ComputeChecksums(PacketRef(packet), plan);
  ASSERT_TRUE(values.has_value());
  auto *ip = reinterpret_cast<rte_ipv4_hdr *>(wire.data());
  auto *tcp = reinterpret_cast<struct rte_tcp_hdr *>(wire.data() + 20);
  auto *bess_ip = reinterpret_cast<const bess::utils::Ipv4 *>(wire.data());
  auto *bess_tcp = reinterpret_cast<const bess::utils::Tcp *>(wire.data() + 20);
  EXPECT_EQ(values->transport->raw_value(), rte_ipv4_udptcp_cksum(ip, tcp));
  EXPECT_EQ(values->transport->raw_value(),
            bess::utils::CalculateIpv4TcpChecksum(*bess_tcp, bess_ip->src,
                                                  bess_ip->dst, 20));
  PacketFree(packet);
}
TEST(PacketChecksumTest, Ipv6TcpMatchesIndependentChecksum) {
  PlainPacketPool pool(8);
  std::array<uint8_t, 60> wire{};
  std::copy_n(kIpv6Udp.begin(), 40, wire.begin());
  wire[4] = 0;
  wire[5] = 20;
  wire[6] = 6;
  wire[40] = 0x04;
  wire[41] = 0xd2;
  wire[42] = 0x16;
  wire[43] = 0x2e;
  wire[52] = 0x50;
  wire[53] = 0x10;
  PacketHandle packet = Build(pool, wire);
  ASSERT_NE(packet, nullptr);
  ChecksumPlan plan{.network_offset = 0,
                    .transport_offset = 40,
                    .ip_version = IpVersion::kIpv6,
                    .network = NetworkChecksum::kNone,
                    .transport = TransportChecksum::kTcp};
  auto values = ComputeChecksums(PacketRef(packet), plan);
  ASSERT_TRUE(values.has_value());
  auto *ip6 = reinterpret_cast<rte_ipv6_hdr *>(wire.data());
  auto *tcp = reinterpret_cast<struct rte_tcp_hdr *>(wire.data() + 40);
  EXPECT_EQ(values->transport->raw_value(), rte_ipv6_udptcp_cksum(ip6, tcp));
  EXPECT_EQ(values->transport->raw_value(),
            rte_ipv6_udptcp_cksum_mbuf(packet, ip6, 40));
  PacketFree(packet);
}

TEST(PacketChecksumTest, RejectsInconsistentChainWithoutChangingIt) {
  PlainPacketPool pool(8);
  PacketHandle packet = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  packet->nb_segs = 2;
  const auto before = Bytes(packet);
  auto result = ComputeChecksums(PacketRef(packet), Ipv4UdpPlan());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ChecksumError::kMalformedChain);
  auto applied = ApplySoftwareChecksums(packet, Ipv4UdpPlan());
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error(), ChecksumError::kMalformedChain);
  EXPECT_EQ(packet->nb_segs, 2);
  EXPECT_EQ(Bytes(packet), before);
  packet->nb_segs = 1;
  PacketFree(packet);
}

TEST(PacketChecksumTest, ComputesAcrossTwoAndFourSegmentsIncludingSplitFields) {
  PlainPacketPool pool(16);
  const std::array<size_t, 2> two = {21, 7};
  PacketHandle two_seg = BuildChain(pool, two, kIpv4Udp);
  ASSERT_NE(two_seg, nullptr);
  auto result = ComputeChecksums(PacketRef(two_seg), Ipv4UdpPlan());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(HostValue(*result->transport), 0xf8a6);
  PacketFree(two_seg);

  const std::array<size_t, 4> four = {11, 15, 1, 1};
  PacketHandle four_seg = BuildChain(pool, four, kIpv4Udp);
  ASSERT_NE(four_seg, nullptr);
  result = ComputeChecksums(PacketRef(four_seg), Ipv4UdpPlan());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(HostValue(*result->network), 0x3c66);
  EXPECT_EQ(HostValue(*result->transport), 0xf8a6);
  rte_ipv4_hdr ip4{};
  std::memcpy(&ip4, kIpv4Udp.data(), sizeof(ip4));
  EXPECT_EQ(result->transport->raw_value(),
            rte_ipv4_udptcp_cksum_mbuf(four_seg, &ip4, 20));
  PacketFree(four_seg);
}
TEST(PacketChecksumTest, EveryTwoSegmentBoundaryMatchesIndependentChecksums) {
  PlainPacketPool pool(8);
  rte_ipv4_hdr ip4{};
  std::memcpy(&ip4, kIpv4Udp.data(), sizeof(ip4));
  for (size_t boundary = 1; boundary < kIpv4Udp.size(); ++boundary) {
    const std::array<size_t, 2> lengths = {boundary,
                                           kIpv4Udp.size() - boundary};
    PacketHandle packet = BuildChain(pool, lengths, kIpv4Udp);
    ASSERT_NE(packet, nullptr);
    auto values = ComputeChecksums(PacketRef(packet), Ipv4UdpPlan());
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values->network->raw_value(), rte_ipv4_cksum(&ip4));
    EXPECT_EQ(values->transport->raw_value(),
              rte_ipv4_udptcp_cksum_mbuf(packet, &ip4, 20));
    PacketFree(packet);
  }

  ChecksumPlan ipv6_plan{.network_offset = 0,
                         .transport_offset = 40,
                         .ip_version = IpVersion::kIpv6,
                         .network = NetworkChecksum::kNone,
                         .transport = TransportChecksum::kUdp};
  rte_ipv6_hdr ip6{};
  std::memcpy(&ip6, kIpv6Udp.data(), sizeof(ip6));
  for (size_t boundary = 1; boundary < kIpv6Udp.size(); ++boundary) {
    const std::array<size_t, 2> lengths = {boundary,
                                           kIpv6Udp.size() - boundary};
    PacketHandle packet = BuildChain(pool, lengths, kIpv6Udp);
    ASSERT_NE(packet, nullptr);
    auto values = ComputeChecksums(PacketRef(packet), ipv6_plan);
    ASSERT_TRUE(values.has_value());
    EXPECT_EQ(values->transport->raw_value(),
              rte_ipv6_udptcp_cksum_mbuf(packet, &ip6, 40));
    PacketFree(packet);
  }
}

TEST(PacketChecksumTest, ApplyWritesSplitFieldsButDoesNotLinearize) {
  PlainPacketPool pool(8);
  const std::array<size_t, 3> sizes = {11, 16, 1};
  PacketHandle packet = BuildChain(pool, sizes, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  auto *second = packet->next;
  ASSERT_NE(second, nullptr);
  ASSERT_NE(ApplySoftwareChecksums(packet, Ipv4UdpPlan()).has_value(), false);
  EXPECT_EQ(packet->nb_segs, 3);
  EXPECT_EQ(packet->next, second);
  const auto bytes = Bytes(packet);
  EXPECT_EQ(bytes[10], 0x3c);
  EXPECT_EQ(bytes[11], 0x66);
  EXPECT_EQ(bytes[26], 0xf8);
  EXPECT_EQ(bytes[27], 0xa6);
  PacketFree(packet);
}

TEST(PacketChecksumTest, HonorsIpLengthsAndComputesIpv4Options) {
  PlainPacketPool pool(8);
  std::array<uint8_t, 32> with_options{};
  std::copy(kIpv4Udp.begin(), kIpv4Udp.begin() + 20, with_options.begin());
  with_options[0] = 0x46;
  with_options[2] = 0;
  with_options[3] = 32;
  with_options[10] = with_options[11] = 0;
  with_options[20] = 1;
  with_options[21] = 1;
  with_options[22] = 1;
  with_options[23] = 1;
  std::copy(kIpv4Udp.begin() + 20, kIpv4Udp.end(), with_options.begin() + 24);
  PacketHandle packet = Build(pool, with_options);
  ASSERT_NE(packet, nullptr);
  ChecksumPlan plan = Ipv4UdpPlan();
  plan.transport_offset = 24;
  auto values = ComputeChecksums(PacketRef(packet), plan);
  ASSERT_TRUE(values.has_value());
  auto *ip = reinterpret_cast<rte_ipv4_hdr *>(with_options.data());
  auto *udp = reinterpret_cast<struct rte_udp_hdr *>(with_options.data() + 24);
  EXPECT_EQ(values->network->raw_value(), rte_ipv4_cksum(ip));
  EXPECT_EQ(values->transport->raw_value(), rte_ipv4_udptcp_cksum(ip, udp));
  auto *bess_ip =
      reinterpret_cast<const bess::utils::Ipv4 *>(with_options.data());
  auto *bess_udp =
      reinterpret_cast<const bess::utils::Udp *>(with_options.data() + 24);
  EXPECT_EQ(values->network->raw_value(),
            bess::utils::CalculateIpv4Checksum(*bess_ip));
  EXPECT_EQ(values->transport->raw_value(),
            bess::utils::CalculateIpv4UdpChecksum(*bess_ip, *bess_udp));
  PacketFree(packet);
}
TEST(PacketChecksumTest, BoundsChecksumsToIpDeclaredLength) {
  PlainPacketPool pool(8);
  std::array<uint8_t, 56> bytes{};
  std::copy(kIpv4Udp.begin(), kIpv4Udp.end(), bytes.begin());
  bytes[2] = 0;
  bytes[3] = 36;
  bytes[24] = 0;
  bytes[25] = 16;
  for (size_t i = 28; i < 36; ++i) {
    bytes[i] = static_cast<uint8_t>(i * 7);
  }
  std::array<uint8_t, 36> declared_packet{};
  std::copy_n(bytes.begin(), declared_packet.size(), declared_packet.begin());
  auto *ip = reinterpret_cast<rte_ipv4_hdr *>(declared_packet.data());
  auto *udp =
      reinterpret_cast<struct rte_udp_hdr *>(declared_packet.data() + 20);
  const uint16_t expected_transport = rte_ipv4_udptcp_cksum(ip, udp);
  const uint16_t expected_network = rte_ipv4_cksum(ip);

  PacketHandle packet = Build(pool, bytes);
  ASSERT_NE(packet, nullptr);
  auto values = ComputeChecksums(PacketRef(packet), Ipv4UdpPlan());
  ASSERT_TRUE(values.has_value());
  EXPECT_EQ(values->network->raw_value(), expected_network);
  EXPECT_EQ(values->transport->raw_value(), expected_transport);
  PacketFree(packet);

  std::fill(bytes.begin() + 36, bytes.end(), 0xa5);
  packet = Build(pool, bytes);
  ASSERT_NE(packet, nullptr);
  values = ComputeChecksums(PacketRef(packet), Ipv4UdpPlan());
  ASSERT_TRUE(values.has_value());
  EXPECT_EQ(values->network->raw_value(), expected_network);
  EXPECT_EQ(values->transport->raw_value(), expected_transport);
  PacketFree(packet);
}

TEST(PacketChecksumTest, RejectsTruncatedHeadersAndFragmentsWithoutMutation) {
  PlainPacketPool pool(12);
  std::array<uint8_t, 19> short_ip{};
  short_ip[0] = 0x45;
  PacketHandle packet = Build(pool, short_ip);
  ASSERT_NE(packet, nullptr);
  auto bad = ComputeChecksums(PacketRef(packet), Ipv4UdpPlan());
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), ChecksumError::kLengthOutOfRange);
  PacketFree(packet);

  std::array<uint8_t, 27> short_udp{};
  std::copy_n(kIpv4Udp.begin(), short_udp.size(), short_udp.begin());
  short_udp[2] = 0;
  short_udp[3] = sizeof(short_udp);
  packet = Build(pool, short_udp);
  ASSERT_NE(packet, nullptr);
  auto before = Bytes(packet);
  bad = ComputeChecksums(PacketRef(packet), Ipv4UdpPlan());
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), ChecksumError::kInvalidTransportHeader);
  EXPECT_FALSE(ApplySoftwareChecksums(packet, Ipv4UdpPlan()).has_value());
  EXPECT_EQ(Bytes(packet), before);
  PacketFree(packet);

  std::array<uint8_t, 28> fragmented = kIpv4Udp;
  fragmented[6] = 0x20;
  packet = Build(pool, fragmented);
  ASSERT_NE(packet, nullptr);
  bad = ComputeChecksums(PacketRef(packet), Ipv4UdpPlan());
  EXPECT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), ChecksumError::kFragmentedDatagram);
  PacketFree(packet);
}

TEST(PacketChecksumTest, RejectsIpv6ExtensionsAndJumbograms) {
  PlainPacketPool pool(8);
  auto extended = kIpv6Udp;
  extended[6] = 0;  // Hop-by-hop options, rather than UDP.
  PacketHandle packet = Build(pool, extended);
  ASSERT_NE(packet, nullptr);
  ChecksumPlan plan{.network_offset = 0,
                    .transport_offset = 40,
                    .ip_version = IpVersion::kIpv6,
                    .network = NetworkChecksum::kNone,
                    .transport = TransportChecksum::kUdp};
  auto result = ComputeChecksums(PacketRef(packet), plan);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ChecksumError::kUnsupportedExtensionHeader);
  PacketFree(packet);
  auto fragmented = kIpv6Udp;
  fragmented[6] = 44;
  packet = Build(pool, fragmented);
  ASSERT_NE(packet, nullptr);
  result = ComputeChecksums(PacketRef(packet), plan);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ChecksumError::kFragmentedDatagram);
  PacketFree(packet);

  auto jumbo = kIpv6Udp;
  jumbo[4] = jumbo[5] = 0;
  packet = Build(pool, jumbo);
  ASSERT_NE(packet, nullptr);
  result = ComputeChecksums(PacketRef(packet), plan);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ChecksumError::kUnsupportedJumbogram);
  PacketFree(packet);
}

TEST(PacketChecksumTest, CloneCowsOnlyWhenTargetBytesAreShared) {
  PlainPacketPool pool(12);
  PacketHandle original = Build(pool, kIpv4Udp);
  ASSERT_NE(original, nullptr);
  PacketHandle clone = bess::PacketClone(original);
  ASSERT_NE(clone, nullptr);
  const auto original_bytes = Bytes(original);
  ASSERT_TRUE(ApplySoftwareChecksums(clone, Ipv4UdpPlan()).has_value());
  EXPECT_NE(clone->buf_addr, original->buf_addr);
  EXPECT_EQ(Bytes(original), original_bytes);
  auto clone_values = ComputeChecksums(PacketRef(clone), Ipv4UdpPlan());
  ASSERT_TRUE(clone_values.has_value());
  EXPECT_EQ(HostValue(*clone_values->network), 0x3c66);
  PacketFree(clone);
  PacketFree(original);
}
TEST(PacketChecksumTest, SharedChecksumClonePreservesSegmentTopology) {
  PlainPacketPool pool(16);
  const std::array<size_t, 2> lengths = {14, 14};
  PacketHandle original = BuildChain(pool, lengths, kIpv4Udp);
  ASSERT_NE(original, nullptr);
  PacketHandle clone = bess::PacketClone(original);
  ASSERT_NE(clone, nullptr);
  const PacketHandle old_clone = clone;
  const auto original_bytes = Bytes(original);

  ASSERT_TRUE(ApplySoftwareChecksums(clone, Ipv4UdpPlan()).has_value());
  EXPECT_NE(clone, old_clone);
  EXPECT_EQ(clone->nb_segs, 2);
  ASSERT_NE(clone->next, nullptr);
  EXPECT_EQ(clone->data_len, 14);
  EXPECT_EQ(clone->next->data_len, 14);
  EXPECT_EQ(clone->next->next, nullptr);
  EXPECT_EQ(Bytes(original), original_bytes);
  PacketFree(clone);
  PacketFree(original);
}
struct ExternalOwner {
  int *free_count;
};

void FreeExternal(void *address, void *opaque) {
  auto *owner = static_cast<ExternalOwner *>(opaque);
  ++*owner->free_count;
  delete[] static_cast<uint8_t *>(address);
  delete owner;
}

TEST(PacketChecksumTest, ExternalBufferCloneDetachesBeforeChecksumWrite) {
  PlainPacketPool pool(12);
  int free_count = 0;
  auto *buffer = new uint8_t[4096];
  std::memcpy(buffer + RTE_PKTMBUF_HEADROOM, kIpv4Udp.data(), kIpv4Udp.size());
  uint16_t buffer_len = 4096;
  auto *owner = new ExternalOwner{&free_count};
  auto *shinfo = rte_pktmbuf_ext_shinfo_init_helper(buffer, &buffer_len,
                                                    FreeExternal, owner);
  if (shinfo == nullptr) {
    delete[] buffer;
    delete owner;
    FAIL() << "external shared-info allocation failed";
  }
  PacketHandle packet = pool.AllocExternal(buffer, RTE_BAD_IOVA, buffer_len,
                                           shinfo, kIpv4Udp.size());
  if (packet == nullptr) {
    delete[] buffer;
    delete owner;
    FAIL() << "external packet allocation failed";
  }
  PacketHandle clone = bess::PacketClone(packet);
  if (clone == nullptr) {
    PacketFree(packet);
    FAIL() << "packet clone allocation failed";
  }
  const auto before = Bytes(packet);
  ASSERT_TRUE(ApplySoftwareChecksums(clone, Ipv4UdpPlan()).has_value());
  EXPECT_EQ(Bytes(packet), before);
  PacketFree(clone);
  EXPECT_EQ(free_count, 0);
  PacketFree(packet);
  EXPECT_EQ(free_count, 1);
}
TEST(PacketChecksumTest, OversizedSharedSegmentFailureIsTransactional) {
  constexpr size_t kPacketLength = 100;
  PlainPacketPool pool(8, -1, 64);
  int free_count = 0;
  constexpr uint16_t kBufferLength = 512;
  auto *buffer = new uint8_t[kBufferLength];
  std::fill_n(buffer + RTE_PKTMBUF_HEADROOM, kPacketLength, 0);
  std::memcpy(buffer + RTE_PKTMBUF_HEADROOM, kIpv4Udp.data(), kIpv4Udp.size());
  uint16_t buffer_len = kBufferLength;
  auto *owner = new ExternalOwner{&free_count};
  auto *shinfo = rte_pktmbuf_ext_shinfo_init_helper(buffer, &buffer_len,
                                                    FreeExternal, owner);
  if (shinfo == nullptr) {
    delete[] buffer;
    delete owner;
    FAIL() << "external shared-info allocation failed";
  }
  PacketHandle packet = pool.AllocExternal(buffer, RTE_BAD_IOVA, buffer_len,
                                           shinfo, kPacketLength);
  if (packet == nullptr) {
    delete[] buffer;
    delete owner;
    FAIL() << "external packet allocation failed";
  }
  PacketHandle clone = bess::PacketClone(packet);
  if (clone == nullptr) {
    PacketFree(packet);
    FAIL() << "packet clone allocation failed";
  }
  const auto before = Bytes(packet);
  const void *const shared_buffer = clone->buf_addr;
  const uint16_t refcnt_before = rte_mbuf_ext_refcnt_read(shinfo);

  auto applied = ApplySoftwareChecksums(clone, Ipv4UdpPlan());
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error(), ChecksumError::kInsufficientWritableCapacity);
  EXPECT_EQ(Bytes(packet), before);
  EXPECT_EQ(Bytes(clone), before);
  EXPECT_EQ(clone->buf_addr, shared_buffer);
  EXPECT_EQ(clone->pkt_len, kPacketLength);
  EXPECT_EQ(clone->nb_segs, 1);
  EXPECT_EQ(clone->next, nullptr);
  EXPECT_EQ(rte_mbuf_ext_refcnt_read(shinfo), refcnt_before);
  PacketFree(clone);
  EXPECT_EQ(free_count, 0);
  PacketFree(packet);
  EXPECT_EQ(free_count, 1);
}

TEST(PacketChecksumTest, WritableHeadWithSharedTailAvoidsUnrelatedCow) {
  PlainPacketPool pool(12);
  PacketHandle tail_owner = pool.Alloc(8);
  ASSERT_NE(tail_owner, nullptr);
  std::memcpy(PacketRef(tail_owner).head_data(), kIpv4Udp.data() + 20, 8);
  PacketHandle packet = pool.Alloc(20);
  ASSERT_NE(packet, nullptr);
  std::memcpy(PacketRef(packet).head_data(), kIpv4Udp.data(), 20);
  PacketHandle shared_tail = bess::PacketClone(tail_owner);
  ASSERT_NE(shared_tail, nullptr);
  packet->next = shared_tail;
  packet->nb_segs = 2;
  packet->pkt_len = 28;

  ChecksumPlan plan = Ipv4UdpPlan();
  plan.transport = TransportChecksum::kNone;
  ASSERT_TRUE(ApplySoftwareChecksums(packet, plan).has_value());
  EXPECT_EQ(packet->data_len, 20);
  EXPECT_EQ(packet->next, shared_tail);
  EXPECT_EQ(shared_tail->buf_addr, tail_owner->buf_addr);
  EXPECT_EQ(Bytes(tail_owner),
            std::vector<uint8_t>(kIpv4Udp.begin() + 20, kIpv4Udp.end()));
  PacketFree(packet);
  PacketFree(tail_owner);
}

TEST(PacketChecksumTest, SharedFieldCOWAllocationFailureIsTransactional) {
  // The packet and its indirect clone exhaust this pool; COW cannot allocate a
  // replacement head. Both owners remain valid and byte-identical on failure.
  PlainPacketPool pool(2);
  PacketHandle original = Build(pool, kIpv4Udp);
  ASSERT_NE(original, nullptr);
  PacketHandle clone = bess::PacketClone(original);
  ASSERT_NE(clone, nullptr);
  clone->port = 17;
  clone->packet_type = 23;
  clone->tx_offload = 0x12345678;
  clone->hash.rss = 0x87654321;
  clone->ol_flags |= RTE_MBUF_F_RX_RSS_HASH;
  std::memset(rte_mbuf_to_priv(clone), 0x5a, bess::kPacketPrivateSize);
  std::array<uint8_t, bess::kPacketPrivateSize> private_before{};
  std::memcpy(private_before.data(), rte_mbuf_to_priv(clone),
              private_before.size());
  const auto before_original = Bytes(original);
  const auto before_clone = Bytes(clone);
  const void *const buf_addr_before = clone->buf_addr;
  const uint16_t data_off_before = clone->data_off;
  const uint16_t data_len_before = clone->data_len;
  const uint16_t nb_segs_before = clone->nb_segs;
  const uint32_t pkt_len_before = clone->pkt_len;
  const uint64_t ol_flags_before = clone->ol_flags;
  const uint64_t tx_offload_before = clone->tx_offload;
  const uint32_t rss_before = clone->hash.rss;
  const PacketHandle next_before = clone->next;
  const uint16_t refcnt_before = rte_mbuf_refcnt_read(clone);
  const size_t available_before = pool.Size();

  auto applied = ApplySoftwareChecksums(clone, Ipv4UdpPlan());
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error(), ChecksumError::kAllocationFailed);
  EXPECT_EQ(Bytes(original), before_original);
  EXPECT_EQ(Bytes(clone), before_clone);
  EXPECT_EQ(clone->buf_addr, buf_addr_before);
  EXPECT_EQ(clone->data_off, data_off_before);
  EXPECT_EQ(clone->data_len, data_len_before);
  EXPECT_EQ(clone->nb_segs, nb_segs_before);
  EXPECT_EQ(clone->pkt_len, pkt_len_before);
  EXPECT_EQ(clone->ol_flags, ol_flags_before);
  EXPECT_EQ(clone->tx_offload, tx_offload_before);
  EXPECT_EQ(clone->hash.rss, rss_before);
  EXPECT_EQ(clone->next, next_before);
  EXPECT_EQ(rte_mbuf_refcnt_read(clone), refcnt_before);
  EXPECT_EQ(std::memcmp(rte_mbuf_to_priv(clone), private_before.data(),
                        private_before.size()),
            0);
  EXPECT_EQ(pool.Size(), available_before);
  EXPECT_EQ(original->next, nullptr);
  EXPECT_EQ(clone->next, nullptr);
  PacketFree(clone);
  PacketFree(original);
}

}  // namespace
