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
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

#include "modules/tx_checksum_profile.h"
#include "packet.h"
#include "packet_checksum.h"
#include "packet_pool.h"
#include "packet_tx_checksum.h"

namespace {

using bess::PacketBatch;
using bess::PacketClone;
using bess::PacketFree;
using bess::PacketHandle;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::packet::ApplySoftwareChecksums;
using bess::packet::BindTxFinalizationProfile;
using bess::packet::BoundTxFinalizationProfile;
using bess::packet::ChecksumPlan;
using bess::packet::FinalizeTxPacket;
using bess::packet::FinalizeTxPacketBatch;
using bess::packet::IpVersion;
using bess::packet::NetworkChecksum;
using bess::packet::TransportChecksum;
using bess::packet::TxChecksumBackend;
using bess::packet::TxChecksumCapabilities;
using bess::packet::TxEncapsulationKind;
using bess::packet::TxFinalizationProfile;

constexpr std::array<uint8_t, 28> kIpv4Udp = {
    0x45, 0x00, 0x00, 0x1c, 0x12, 0x34, 0x40, 0x00, 0x40, 0x11,
    0x12, 0x34, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02,
    0x04, 0xd2, 0x16, 0x2e, 0x00, 0x08, 0x56, 0x78};

constexpr std::array<uint8_t, 48> kIpv6Udp = {
    0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x11, 0x40, 0x20, 0x01, 0x0d, 0xb8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x04, 0xd2, 0x16, 0x2e, 0x00, 0x08, 0x34, 0x56};

std::array<uint8_t, 40> Ipv4Tcp() {
  std::array<uint8_t, 40> bytes{};
  bytes[0] = 0x45;
  bytes[2] = 0;
  bytes[3] = 40;
  bytes[8] = 64;
  bytes[9] = 6;
  bytes[12] = 192;
  bytes[14] = 2;
  bytes[15] = 1;
  bytes[16] = 198;
  bytes[17] = 51;
  bytes[18] = 100;
  bytes[19] = 2;
  bytes[20] = 0x04;
  bytes[21] = 0xd2;
  bytes[22] = 0x00;
  bytes[23] = 0x50;
  bytes[32] = 0x50;
  bytes[33] = 0x02;
  bytes[36] = 0xab;
  bytes[37] = 0xcd;
  return bytes;
}

std::array<uint8_t, 60> Ipv6Tcp() {
  std::array<uint8_t, 60> bytes{};
  bytes[0] = 0x60;
  bytes[4] = 0;
  bytes[5] = 20;
  bytes[6] = 6;
  bytes[7] = 64;
  bytes[8] = 0x20;
  bytes[9] = 0x01;
  bytes[10] = 0x0d;
  bytes[11] = 0xb8;
  bytes[23] = 1;
  bytes[24] = 0x20;
  bytes[25] = 0x01;
  bytes[26] = 0x0d;
  bytes[27] = 0xb8;
  bytes[39] = 2;
  bytes[40] = 0x04;
  bytes[41] = 0xd2;
  bytes[42] = 0x00;
  bytes[43] = 0x50;
  bytes[52] = 0x50;
  bytes[53] = 0x02;
  bytes[56] = 0x9a;
  bytes[57] = 0xbc;
  return bytes;
}

void Store16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value >> 8);
  bytes[offset + 1] = static_cast<uint8_t>(value);
}

uint16_t Load16(const std::vector<uint8_t> &bytes, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) |
                               bytes[offset + 1]);
}

void InitIpv4(std::span<uint8_t> bytes, size_t offset, uint16_t length,
              uint8_t protocol) {
  bytes[offset] = 0x45;
  Store16(bytes, offset + 2, length);
  bytes[offset + 8] = 64;
  bytes[offset + 9] = protocol;
  bytes[offset + 10] = 0x12;
  bytes[offset + 11] = 0x34;
  bytes[offset + 12] = 192;
  bytes[offset + 13] = 0;
  bytes[offset + 14] = 2;
  bytes[offset + 15] = 1;
  bytes[offset + 16] = 198;
  bytes[offset + 17] = 51;
  bytes[offset + 18] = 100;
  bytes[offset + 19] = 2;
}

std::array<uint8_t, 56> TunnelIpv4Udp() {
  std::array<uint8_t, 56> bytes{};
  InitIpv4(bytes, 0, bytes.size(), 17);
  bytes[20] = 0x13;
  bytes[21] = 0x88;
  bytes[22] = 0x17;
  bytes[23] = 0x70;
  Store16(bytes, 24, 36);
  Store16(bytes, 26, 0x5566);
  std::copy(kIpv4Udp.begin(), kIpv4Udp.end(), bytes.begin() + 28);
  return bytes;
}

std::array<uint8_t, 68> TunnelIpv4Tcp() {
  std::array<uint8_t, 68> bytes{};
  InitIpv4(bytes, 0, bytes.size(), 17);
  bytes[20] = 0x13;
  bytes[21] = 0x88;
  bytes[22] = 0x17;
  bytes[23] = 0x70;
  Store16(bytes, 24, 48);
  Store16(bytes, 26, 0x5566);
  const auto inner = Ipv4Tcp();
  std::copy(inner.begin(), inner.end(), bytes.begin() + 28);
  return bytes;
}

PacketHandle BuildChain(PlainPacketPool &pool,
                        std::span<const size_t> segment_lengths,
                        std::span<const uint8_t> bytes) {
  PacketHandle head = nullptr;
  PacketHandle previous = nullptr;
  size_t offset = 0;
  for (size_t length : segment_lengths) {
    PacketHandle segment = pool.Alloc(length);
    if (segment == nullptr) {
      if (head != nullptr) {
        PacketFree(head);
      }
      return nullptr;
    }
    if (head == nullptr) {
      head = segment;
    } else {
      previous->next = segment;
    }
    if (length != 0) {
      std::memcpy(PacketRef(segment).head_data(), bytes.data() + offset,
                  length);
    }
    offset += length;
    previous = segment;
  }
  if (head == nullptr || offset != bytes.size()) {
    if (head != nullptr) {
      PacketFree(head);
    }
    return nullptr;
  }
  head->pkt_len = static_cast<uint32_t>(bytes.size());
  head->nb_segs = static_cast<uint16_t>(segment_lengths.size());
  return head;
}

template <size_t N>
PacketHandle Build(PlainPacketPool &pool,
                   const std::array<uint8_t, N> &bytes) {
  const size_t length = N;
  return BuildChain(pool, std::span<const size_t>(&length, 1), bytes);
}

PacketHandle Build(PlainPacketPool &pool, std::span<const uint8_t> bytes) {
  const size_t length = bytes.size();
  return BuildChain(pool, std::span<const size_t>(&length, 1), bytes);
}

std::vector<uint8_t> Bytes(PacketHandle packet) {
  std::vector<uint8_t> result(packet->pkt_len);
  size_t offset = 0;
  for (PacketHandle segment = packet; segment != nullptr;
       segment = segment->next) {
    std::memcpy(result.data() + offset, PacketRef(segment).head_data(),
                segment->data_len);
    offset += segment->data_len;
  }
  return result;
}

ChecksumPlan V4UdpPlan(bool network = false, size_t network_offset = 0,
                       size_t transport_offset = 20) {
  return {.network_offset = network_offset,
          .transport_offset = transport_offset,
          .ip_version = IpVersion::kIpv4,
          .network = network ? NetworkChecksum::kIpv4Header
                             : NetworkChecksum::kNone,
          .transport = TransportChecksum::kUdp};
}

ChecksumPlan V4TcpPlan(size_t network_offset = 0,
                       size_t transport_offset = 20) {
  return {.network_offset = network_offset,
          .transport_offset = transport_offset,
          .ip_version = IpVersion::kIpv4,
          .network = NetworkChecksum::kNone,
          .transport = TransportChecksum::kTcp};
}

ChecksumPlan V6UdpPlan() {
  return {.network_offset = 0,
          .transport_offset = 40,
          .ip_version = IpVersion::kIpv6,
          .network = NetworkChecksum::kNone,
          .transport = TransportChecksum::kUdp};
}

ChecksumPlan V6TcpPlan() {
  return {.network_offset = 0,
          .transport_offset = 40,
          .ip_version = IpVersion::kIpv6,
          .network = NetworkChecksum::kNone,
          .transport = TransportChecksum::kTcp};
}

TxChecksumCapabilities AllHardware() {
  return {.ipv4_header = true,
          .udp = true,
          .tcp = true,
          .outer_ipv4_header = true,
          .outer_udp = true,
          .ip_tunnel = true,
          .udp_tunnel = true,
          .multi_segment_tx = true};
}

TxFinalizationProfile V4Profile(ChecksumPlan plan) {
  TxFinalizationProfile profile;
  profile.outer = plan;
  return profile;
}

TxFinalizationProfile TunnelProfile(ChecksumPlan outer, ChecksumPlan inner,
                                    TxEncapsulationKind kind =
                                        TxEncapsulationKind::kUdp) {
  TxFinalizationProfile profile;
  profile.encapsulation = {.kind = kind,
                           .outer_ip_version = IpVersion::kIpv4,
                           .outer_network_offset = 0};
  profile.outer = outer;
  profile.inner = inner;
  return profile;
}

rte_be16_t Ipv4Seed(PacketHandle packet, size_t offset) {
  rte_ipv4_hdr ip{};
  std::memcpy(&ip, PacketRef(packet).head_data<const void *>(
                       static_cast<uint16_t>(offset)),
              sizeof(ip));
  ip.hdr_checksum = 0;
  return rte_ipv4_phdr_cksum(&ip, 0);
}

rte_be16_t Ipv6Seed(PacketHandle packet, size_t offset) {
  rte_ipv6_hdr ip{};
  std::memcpy(&ip, PacketRef(packet).head_data<const void *>(
                       static_cast<uint16_t>(offset)),
              sizeof(ip));
  return rte_ipv6_phdr_cksum(&ip, 0);
}

TEST(PacketTxChecksumTest, EmptyProfileLeavesPacketAndMetadataUntouched) {
  PlainPacketPool pool(8);
  PacketHandle packet = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  packet->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM |
                     RTE_MBUF_F_TX_TUNNEL_UDP;
  packet->l2_len = 17;
  packet->l3_len = 23;
  packet->l4_len = 11;
  packet->outer_l2_len = 7;
  packet->outer_l3_len = 9;
  const auto bytes = Bytes(packet);
  const uint64_t flags = packet->ol_flags;

  PacketBatch batch;
  batch.clear();
  batch.add(packet);
  const auto result = FinalizeTxPacketBatch(batch, {});

  EXPECT_EQ(result.rejected, 0);
  ASSERT_EQ(batch.cnt(), 1);
  EXPECT_EQ(batch.handles()[0], packet);
  EXPECT_EQ(Bytes(packet), bytes);
  EXPECT_EQ(packet->ol_flags, flags);
  EXPECT_EQ(packet->l2_len, 17);
  EXPECT_EQ(packet->l3_len, 23);
  EXPECT_EQ(packet->l4_len, 11);
  EXPECT_EQ(packet->outer_l2_len, 7);
  EXPECT_EQ(packet->outer_l3_len, 9);
  PacketFree(packet);
}

TEST(PacketTxChecksumTest, BindsEachChecksumComponentIndependently) {
  TxChecksumCapabilities capabilities{};
  capabilities.ipv4_header = true;
  const auto bound = BindTxFinalizationProfile(
      V4Profile(V4UdpPlan(true)), capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_TRUE(bound->outer.has_value());
  EXPECT_EQ(bound->outer->network_backend, TxChecksumBackend::kHardware);
  EXPECT_EQ(bound->outer->transport_backend, TxChecksumBackend::kSoftware);

  auto tunnel_profile = TunnelProfile(
      V4UdpPlan(false, 0, 20), V4TcpPlan(28, 48));
  capabilities = {};
  capabilities.tcp = true;
  capabilities.udp_tunnel = true;
  const auto tunnel_bound =
      BindTxFinalizationProfile(tunnel_profile, capabilities);
  ASSERT_TRUE(tunnel_bound.has_value());
  ASSERT_TRUE(tunnel_bound->outer.has_value());
  ASSERT_TRUE(tunnel_bound->inner.has_value());
  EXPECT_EQ(tunnel_bound->outer->transport_backend,
            TxChecksumBackend::kSoftware);
  EXPECT_EQ(tunnel_bound->inner->transport_backend,
            TxChecksumBackend::kHardware);
}

TEST(PacketTxChecksumTest, SoftwareFallbackMatchesK44aChecksumPrimitive) {
  PlainPacketPool pool(16);
  PacketHandle packet = Build(pool, kIpv4Udp);
  PacketHandle expected = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(expected, nullptr);
  const auto plan = V4UdpPlan(true);
  ASSERT_TRUE(ApplySoftwareChecksums(expected, plan).has_value());
  packet->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM;

  const auto bound = BindTxFinalizationProfile(V4Profile(plan), {});
  ASSERT_TRUE(bound.has_value());
  ASSERT_EQ(bound->outer->network_backend, TxChecksumBackend::kSoftware);
  ASSERT_EQ(bound->outer->transport_backend, TxChecksumBackend::kSoftware);
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  EXPECT_EQ(Bytes(packet), Bytes(expected));
  EXPECT_EQ(packet->ol_flags & (RTE_MBUF_F_TX_IPV4 |
                                RTE_MBUF_F_TX_IP_CKSUM |
                                RTE_MBUF_F_TX_L4_MASK),
            0);
  PacketFree(packet);
  PacketFree(expected);
}

TEST(PacketTxChecksumTest, PreparesIpv4HeaderOnlyHardwareOffload) {
  PlainPacketPool pool(8);
  PacketHandle packet = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  TxChecksumCapabilities capabilities{};
  capabilities.ipv4_header = true;
  auto bound = BindTxFinalizationProfile(
      V4Profile({.network_offset = 0,
                 .transport_offset = 20,
                 .ip_version = IpVersion::kIpv4,
                 .network = NetworkChecksum::kIpv4Header,
                 .transport = TransportChecksum::kNone}),
      capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_EQ(bound->outer->network_backend, TxChecksumBackend::kHardware);
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  const auto bytes = Bytes(packet);
  EXPECT_EQ(bytes[10], 0);
  EXPECT_EQ(bytes[11], 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_IPV4, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_IP_CKSUM, 0);
  EXPECT_EQ(packet->ol_flags & RTE_MBUF_F_TX_L4_MASK, 0);
  EXPECT_EQ(packet->l2_len, 0);
  EXPECT_EQ(packet->l3_len, 20);
  EXPECT_EQ(packet->l4_len, 0);
  PacketFree(packet);
}

TEST(PacketTxChecksumTest, PreparesIpv4UdpHardwarePseudoHeaderSeed) {
  PlainPacketPool pool(8);
  PacketHandle packet = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  const rte_be16_t expected_seed = Ipv4Seed(packet, 0);
  TxChecksumCapabilities capabilities{};
  capabilities.udp = true;
  const auto bound = BindTxFinalizationProfile(
      V4Profile(V4UdpPlan()), capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_EQ(bound->outer->transport_backend, TxChecksumBackend::kHardware);
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  const auto bytes = Bytes(packet);
  uint16_t actual_seed = 0;
  std::memcpy(&actual_seed, bytes.data() + 26, sizeof(actual_seed));
  EXPECT_EQ(actual_seed, expected_seed);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_IPV4, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_UDP_CKSUM, 0);
  EXPECT_EQ(packet->l2_len, 0);
  EXPECT_EQ(packet->l3_len, 20);
  EXPECT_EQ(packet->l4_len, 8);
  PacketFree(packet);
}

TEST(PacketTxChecksumTest, SupportsIndependentIpv4NetworkAndTransportBackends) {
  PlainPacketPool pool(16);
  PacketHandle packet = Build(pool, kIpv4Udp);
  PacketHandle expected = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(expected, nullptr);
  const auto plan = V4UdpPlan(true);
  ASSERT_TRUE(ApplySoftwareChecksums(expected, plan).has_value());
  TxChecksumCapabilities capabilities{};
  capabilities.ipv4_header = true;
  const auto bound = BindTxFinalizationProfile(V4Profile(plan), capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_EQ(bound->outer->network_backend, TxChecksumBackend::kHardware);
  ASSERT_EQ(bound->outer->transport_backend, TxChecksumBackend::kSoftware);
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  const auto bytes = Bytes(packet);
  const auto expected_bytes = Bytes(expected);
  EXPECT_EQ(bytes[10], 0);
  EXPECT_EQ(bytes[11], 0);
  EXPECT_EQ(std::vector<uint8_t>(bytes.begin() + 20, bytes.end()),
            std::vector<uint8_t>(expected_bytes.begin() + 20,
                                 expected_bytes.end()));
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_IP_CKSUM, 0);
  EXPECT_EQ(packet->ol_flags & RTE_MBUF_F_TX_UDP_CKSUM, 0);
  PacketFree(packet);
  PacketFree(expected);
}
TEST(PacketTxChecksumTest, SupportsSoftwareIpv4NetworkWithHardwareUdp) {
  PlainPacketPool pool(8);
  PacketHandle packet = Build(pool, kIpv4Udp);
  PacketHandle expected = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(expected, nullptr);
  const auto plan = V4UdpPlan(true);
  auto expected_network = plan;
  expected_network.transport = TransportChecksum::kNone;
  ASSERT_TRUE(ApplySoftwareChecksums(expected, expected_network).has_value());
  const rte_be16_t expected_seed = Ipv4Seed(packet, 0);

  TxChecksumCapabilities capabilities{};
  capabilities.udp = true;
  const auto bound = BindTxFinalizationProfile(V4Profile(plan), capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_EQ(bound->outer->network_backend, TxChecksumBackend::kSoftware);
  ASSERT_EQ(bound->outer->transport_backend, TxChecksumBackend::kHardware);
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  const auto bytes = Bytes(packet);
  const auto expected_bytes = Bytes(expected);
  EXPECT_EQ(Load16(bytes, 10), Load16(expected_bytes, 10));
  uint16_t actual_seed = 0;
  std::memcpy(&actual_seed, bytes.data() + 26, sizeof(actual_seed));
  EXPECT_EQ(actual_seed, expected_seed);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_UDP_CKSUM, 0);
  EXPECT_EQ(packet->ol_flags & RTE_MBUF_F_TX_IP_CKSUM, 0);
  EXPECT_EQ(packet->l2_len, 0);
  EXPECT_EQ(packet->l3_len, 20);
  EXPECT_EQ(packet->l4_len, 8);
  PacketFree(packet);
  PacketFree(expected);
}


TEST(PacketTxChecksumTest, SupportsIpv6UdpAndTcpHardwareSeeds) {
  PlainPacketPool pool(16);
  TxChecksumCapabilities capabilities{};
  capabilities.udp = true;
  capabilities.tcp = true;

  PacketHandle udp = Build(pool, kIpv6Udp);
  ASSERT_NE(udp, nullptr);
  const rte_be16_t expected_udp_seed = Ipv6Seed(udp, 0);
  auto udp_bound = BindTxFinalizationProfile(V4Profile(V6UdpPlan()),
                                             capabilities);
  ASSERT_TRUE(udp_bound.has_value());
  ASSERT_TRUE(FinalizeTxPacket(udp, *udp_bound).has_value());
  auto udp_bytes = Bytes(udp);
  uint16_t udp_seed = 0;
  std::memcpy(&udp_seed, udp_bytes.data() + 46, sizeof(udp_seed));
  EXPECT_EQ(udp_seed, expected_udp_seed);
  EXPECT_NE(udp->ol_flags & RTE_MBUF_F_TX_IPV6, 0);
  EXPECT_NE(udp->ol_flags & RTE_MBUF_F_TX_UDP_CKSUM, 0);
  PacketFree(udp);

  const auto tcp_bytes = Ipv6Tcp();
  PacketHandle tcp = Build(pool, tcp_bytes);
  ASSERT_NE(tcp, nullptr);
  const rte_be16_t expected_tcp_seed = Ipv6Seed(tcp, 0);
  auto tcp_bound = BindTxFinalizationProfile(V4Profile(V6TcpPlan()),
                                             capabilities);
  ASSERT_TRUE(tcp_bound.has_value());
  ASSERT_TRUE(FinalizeTxPacket(tcp, *tcp_bound).has_value());
  const auto finalized_tcp = Bytes(tcp);
  uint16_t tcp_seed = 0;
  std::memcpy(&tcp_seed, finalized_tcp.data() + 56, sizeof(tcp_seed));
  EXPECT_EQ(tcp_seed, expected_tcp_seed);
  EXPECT_NE(tcp->ol_flags & RTE_MBUF_F_TX_IPV6, 0);
  EXPECT_NE(tcp->ol_flags & RTE_MBUF_F_TX_TCP_CKSUM, 0);
  PacketFree(tcp);
}

TEST(PacketTxChecksumTest, OuterOnlyProfilePreservesInnerChecksumBytes) {
  PlainPacketPool pool(8);
  const auto tunnel = TunnelIpv4Udp();
  PacketHandle packet = Build(pool, tunnel);
  ASSERT_NE(packet, nullptr);
  const auto original = Bytes(packet);
  TxChecksumCapabilities capabilities{};
  capabilities.outer_ipv4_header = true;
  TxFinalizationProfile profile;
  profile.encapsulation = {.kind = TxEncapsulationKind::kUdp,
                           .outer_ip_version = IpVersion::kIpv4,
                           .outer_network_offset = 0};
  profile.outer = ChecksumPlan{.network_offset = 0,
                               .transport_offset = 20,
                               .ip_version = IpVersion::kIpv4,
                               .network = NetworkChecksum::kIpv4Header,
                               .transport = TransportChecksum::kNone};
  const auto bound = BindTxFinalizationProfile(profile, capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  const auto finalized = Bytes(packet);
  EXPECT_EQ(std::vector<uint8_t>(finalized.begin() + 28, finalized.end()),
            std::vector<uint8_t>(original.begin() + 28, original.end()));
  EXPECT_EQ(finalized[10], 0);
  EXPECT_EQ(finalized[11], 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_OUTER_IPV4, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_OUTER_IP_CKSUM, 0);
  EXPECT_EQ(packet->ol_flags & RTE_MBUF_F_TX_TUNNEL_MASK, 0);
  EXPECT_EQ(packet->outer_l2_len, 0);
  EXPECT_EQ(packet->outer_l3_len, 20);
  PacketFree(packet);
}

TEST(PacketTxChecksumTest, InnerOnlyTunnelOffloadSetsGenericUdpMetadata) {
  PlainPacketPool pool(8);
  const auto tunnel = TunnelIpv4Udp();
  PacketHandle packet = Build(pool, tunnel);
  ASSERT_NE(packet, nullptr);
  TxChecksumCapabilities capabilities{};
  capabilities.udp = true;
  capabilities.udp_tunnel = true;
  TxFinalizationProfile profile;
  profile.encapsulation = {.kind = TxEncapsulationKind::kUdp,
                           .outer_ip_version = IpVersion::kIpv4,
                           .outer_network_offset = 0};
  profile.inner = V4UdpPlan(false, 28, 48);
  const auto bound = BindTxFinalizationProfile(profile, capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_FALSE(bound->outer.has_value());
  ASSERT_EQ(bound->inner->transport_backend, TxChecksumBackend::kHardware);
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  const auto bytes = Bytes(packet);
  EXPECT_EQ(Load16(bytes, 26), 0x5566);
  uint16_t actual_seed = 0;
  std::memcpy(&actual_seed, bytes.data() + 54, sizeof(actual_seed));
  EXPECT_EQ(actual_seed, Ipv4Seed(packet, 28));
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_OUTER_IPV4, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_TUNNEL_UDP, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_IPV4, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_UDP_CKSUM, 0);
  EXPECT_EQ(packet->outer_l2_len, 0);
  EXPECT_EQ(packet->outer_l3_len, 20);
  EXPECT_EQ(packet->l2_len, 8);
  EXPECT_EQ(packet->l3_len, 20);
  EXPECT_EQ(packet->l4_len, 8);
  PacketFree(packet);
}

TEST(PacketTxChecksumTest, CombinedTunnelOffloadsSetOuterAndInnerMetadata) {
  PlainPacketPool pool(8);
  PacketHandle packet = Build(pool, TunnelIpv4Tcp());
  ASSERT_NE(packet, nullptr);
  const rte_be16_t inner_seed = Ipv4Seed(packet, 28);
  TxChecksumCapabilities capabilities{};
  capabilities.outer_ipv4_header = true;
  capabilities.outer_udp = true;
  capabilities.tcp = true;
  capabilities.udp_tunnel = true;
  auto profile = TunnelProfile(V4UdpPlan(true), V4TcpPlan(28, 48));
  const auto bound = BindTxFinalizationProfile(profile, capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_EQ(bound->outer->network_backend, TxChecksumBackend::kHardware);
  ASSERT_EQ(bound->outer->transport_backend, TxChecksumBackend::kHardware);
  ASSERT_EQ(bound->inner->transport_backend, TxChecksumBackend::kHardware);
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  const auto bytes = Bytes(packet);
  EXPECT_EQ(bytes[10], 0);
  EXPECT_EQ(bytes[11], 0);
  EXPECT_EQ(bytes[26], 0);
  EXPECT_EQ(bytes[27], 0);
  uint16_t actual_inner_seed = 0;
  std::memcpy(&actual_inner_seed, bytes.data() + 64, sizeof(actual_inner_seed));
  EXPECT_EQ(actual_inner_seed, inner_seed);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_OUTER_IPV4, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_OUTER_IP_CKSUM, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_OUTER_UDP_CKSUM, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_TUNNEL_UDP, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_TCP_CKSUM, 0);
  EXPECT_EQ(packet->outer_l3_len, 20);
  EXPECT_EQ(packet->l2_len, 8);
  EXPECT_EQ(packet->l3_len, 20);
  EXPECT_EQ(packet->l4_len, 20);
  PacketFree(packet);
}

TEST(PacketTxChecksumTest, SoftwareOuterUdpIncludesFinalInnerHardwareChecksum) {
  PlainPacketPool pool(16);
  PacketHandle packet = Build(pool, TunnelIpv4Tcp());
  PacketHandle expected = Build(pool, TunnelIpv4Tcp());
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(expected, nullptr);
  const auto inner = V4TcpPlan(28, 48);
  const auto outer_udp = V4UdpPlan(false, 0, 20);
  ASSERT_TRUE(ApplySoftwareChecksums(expected, inner).has_value());
  ASSERT_TRUE(ApplySoftwareChecksums(expected, outer_udp).has_value());

  TxChecksumCapabilities capabilities{};
  capabilities.tcp = true;
  capabilities.udp_tunnel = true;
  const auto bound = BindTxFinalizationProfile(
      TunnelProfile(outer_udp, inner), capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_EQ(bound->outer->transport_backend, TxChecksumBackend::kSoftware);
  ASSERT_EQ(bound->inner->transport_backend, TxChecksumBackend::kHardware);
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());

  auto finalized = Bytes(packet);
  const auto expected_bytes = Bytes(expected);
  EXPECT_EQ(Load16(finalized, 26), Load16(expected_bytes, 26));
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_TCP_CKSUM, 0);
  EXPECT_EQ(packet->ol_flags & RTE_MBUF_F_TX_OUTER_UDP_CKSUM, 0);
  EXPECT_NE(packet->ol_flags & RTE_MBUF_F_TX_TUNNEL_UDP, 0);

  PacketHandle emulated = Build(pool, finalized);
  ASSERT_NE(emulated, nullptr);
  ASSERT_TRUE(ApplySoftwareChecksums(emulated, inner).has_value());
  EXPECT_EQ(Bytes(emulated), expected_bytes);
  PacketFree(emulated);
  PacketFree(packet);
  PacketFree(expected);
}

TEST(PacketTxChecksumTest, SoftwareProfileHandlesSplitChecksumChains) {
  PlainPacketPool pool(16);
  const size_t split[] = {25, 3};
  PacketHandle packet = BuildChain(pool, split, kIpv4Udp);
  PacketHandle expected = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(expected, nullptr);
  ASSERT_TRUE(ApplySoftwareChecksums(expected, V4UdpPlan(true)).has_value());
  const auto bound =
      BindTxFinalizationProfile(V4Profile(V4UdpPlan(true)), {});
  ASSERT_TRUE(bound.has_value());
  ASSERT_TRUE(FinalizeTxPacket(packet, *bound).has_value());
  EXPECT_EQ(packet->nb_segs, 2);
  EXPECT_EQ(Bytes(packet), Bytes(expected));
  PacketFree(packet);
  PacketFree(expected);
}

TEST(PacketTxChecksumTest, HardwareRequiresSupportedChainsAndContiguousHeaders) {
  PlainPacketPool pool(24);
  const auto tcp = Ipv4Tcp();
  std::array<uint8_t, 42> with_payload{};
  std::copy(tcp.begin(), tcp.end(), with_payload.begin());
  Store16(with_payload, 2, with_payload.size());
  with_payload[40] = 0x5a;
  with_payload[41] = 0xa5;
  const size_t split_headers[] = {10, 32};
  PacketHandle split = BuildChain(pool, split_headers, with_payload);
  ASSERT_NE(split, nullptr);
  const auto original = Bytes(split);
  TxChecksumCapabilities capabilities{};
  capabilities.tcp = true;
  capabilities.multi_segment_tx = true;
  const auto profile = V4Profile(V4TcpPlan());
  auto bound = BindTxFinalizationProfile(profile, capabilities);
  ASSERT_TRUE(bound.has_value());
  auto result = FinalizeTxPacket(split, *bound);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), bess::packet::ChecksumError::kUnsupportedOffloadLayout);
  EXPECT_EQ(Bytes(split), original);
  PacketFree(split);

  const size_t header_contiguous[] = {40, 2};
  PacketHandle eligible = BuildChain(pool, header_contiguous, with_payload);
  ASSERT_NE(eligible, nullptr);
  ASSERT_TRUE(FinalizeTxPacket(eligible, *bound).has_value());
  EXPECT_NE(eligible->ol_flags & RTE_MBUF_F_TX_TCP_CKSUM, 0);
  PacketFree(eligible);

  capabilities.multi_segment_tx = false;
  const auto single_segment_only = BindTxFinalizationProfile(profile, capabilities);
  ASSERT_TRUE(single_segment_only.has_value());
  PacketHandle chain = BuildChain(pool, header_contiguous, with_payload);
  ASSERT_NE(chain, nullptr);
  const auto unsupported = FinalizeTxPacket(chain, *single_segment_only);
  ASSERT_FALSE(unsupported.has_value());
  EXPECT_EQ(unsupported.error(),
            bess::packet::ChecksumError::kUnsupportedOffloadLayout);
  PacketFree(chain);
}

TEST(PacketTxChecksumTest, HardwareSeedWriteCopiesSharedPayloadTopologySafely) {
  PlainPacketPool pool(8);
  PacketHandle original = Build(pool, kIpv4Udp);
  ASSERT_NE(original, nullptr);
  PacketHandle clone = PacketClone(original);
  ASSERT_NE(clone, nullptr);
  const auto original_bytes = Bytes(original);
  TxChecksumCapabilities capabilities{};
  capabilities.udp = true;
  const auto bound =
      BindTxFinalizationProfile(V4Profile(V4UdpPlan()), capabilities);
  ASSERT_TRUE(bound.has_value());
  ASSERT_TRUE(FinalizeTxPacket(clone, *bound).has_value());

  EXPECT_EQ(Bytes(original), original_bytes);
  EXPECT_EQ(clone->nb_segs, 1);
  uint16_t seed = 0;
  const auto clone_bytes = Bytes(clone);
  std::memcpy(&seed, clone_bytes.data() + 26, sizeof(seed));
  EXPECT_EQ(seed, Ipv4Seed(clone, 0));
  PacketFree(clone);
  PacketFree(original);
}

TEST(PacketTxChecksumTest, RejectsInnerIpBeyondOuterEncapsulationLength) {
  PlainPacketPool pool(8);
  std::array<uint8_t, 70> bytes{};
  InitIpv4(bytes, 0, 56, 17);
  Store16(bytes, 24, 36);
  InitIpv4(bytes, 28, 40, 17);
  bytes[48] = 0x13;
  bytes[49] = 0x88;
  bytes[50] = 0x17;
  bytes[51] = 0x70;
  Store16(bytes, 52, 20);
  PacketHandle packet = Build(pool, bytes);
  ASSERT_NE(packet, nullptr);
  const auto original = Bytes(packet);

  const auto profile =
      TunnelProfile(V4UdpPlan(false, 0, 20), V4UdpPlan(false, 28, 48));
  const auto bound = BindTxFinalizationProfile(profile, {});
  ASSERT_TRUE(bound.has_value());
  const auto result = FinalizeTxPacket(packet, *bound);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), bess::packet::ChecksumError::kInvalidPlan);
  EXPECT_EQ(Bytes(packet), original);
  PacketFree(packet);
}

TEST(PacketTxChecksumTest, MismatchedAndTruncatedLayoutsFailClosed) {
  PlainPacketPool pool(16);
  PacketHandle packet = Build(pool, kIpv4Udp);
  ASSERT_NE(packet, nullptr);
  const auto original = Bytes(packet);
  ChecksumPlan mismatch = V4UdpPlan(false, 0, 19);
  const auto bound = BindTxFinalizationProfile(V4Profile(mismatch), {});
  ASSERT_TRUE(bound.has_value());
  const auto result = FinalizeTxPacket(packet, *bound);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), bess::packet::ChecksumError::kInvalidPlan);
  EXPECT_EQ(Bytes(packet), original);
  PacketFree(packet);

  std::array<uint8_t, 16> truncated{};
  truncated[0] = 0x45;
  truncated[2] = 0;
  truncated[3] = 28;
  truncated[9] = 17;
  PacketHandle short_packet = Build(pool, truncated);
  ASSERT_NE(short_packet, nullptr);
  const auto short_result = FinalizeTxPacket(short_packet, *bound);
  ASSERT_FALSE(short_result.has_value());
  EXPECT_EQ(short_result.error(),
            bess::packet::ChecksumError::kLengthOutOfRange);
  PacketFree(short_packet);
}

TEST(PacketTxChecksumTest, BatchRejectsMalformedPacketsAndCompactsSuccesses) {
  PlainPacketPool pool(16);
  std::array<uint8_t, 16> truncated{};
  truncated[0] = 0x45;
  truncated[2] = 0;
  truncated[3] = 28;
  truncated[9] = 17;
  PacketHandle bad = Build(pool, truncated);
  PacketHandle good = Build(pool, kIpv4Udp);
  ASSERT_NE(bad, nullptr);
  ASSERT_NE(good, nullptr);
  const auto bound =
      BindTxFinalizationProfile(V4Profile(V4UdpPlan(true)), {});
  ASSERT_TRUE(bound.has_value());

  PacketBatch batch;
  batch.clear();
  batch.add(bad);
  batch.add(good);
  const auto result = FinalizeTxPacketBatch(batch, *bound);
  EXPECT_EQ(result.rejected, 1);
  ASSERT_EQ(batch.cnt(), 1);
  EXPECT_EQ(batch.handles()[0], good);
  auto expected = Build(pool, kIpv4Udp);
  ASSERT_NE(expected, nullptr);
  ASSERT_TRUE(ApplySoftwareChecksums(expected, V4UdpPlan(true)).has_value());
  EXPECT_EQ(Bytes(good), Bytes(expected));
  PacketFree(good);
  PacketFree(expected);
}

TEST(PacketTxChecksumTest, RejectsInnerDomainWithoutEncapsulation) {
  TxFinalizationProfile profile;
  profile.inner = V4TcpPlan(28, 48);
  const auto result = BindTxFinalizationProfile(profile, AllHardware());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), bess::packet::ChecksumError::kInvalidPlan);
}

TEST(PacketTxChecksumTest, ParsesProfileProtobufWithoutBackendDetails) {
  bess::pb::TxChecksumProfile message;
  auto *encapsulation = message.mutable_encapsulation();
  encapsulation->set_kind(bess::pb::TX_ENCAPSULATION_UDP);
  encapsulation->set_outer_ip_version(bess::pb::CHECKSUM_IP_VERSION_IPV4);
  encapsulation->set_outer_network_offset(0);
  auto *outer = message.mutable_outer();
  outer->set_network_offset(0);
  outer->set_transport_offset(20);
  outer->set_ip_version(bess::pb::CHECKSUM_IP_VERSION_IPV4);
  outer->set_transport(bess::pb::TX_CHECKSUM_TRANSPORT_UDP);
  auto *inner = message.mutable_inner();
  inner->set_network_offset(28);
  inner->set_transport_offset(48);
  inner->set_ip_version(bess::pb::CHECKSUM_IP_VERSION_IPV4);
  inner->set_transport(bess::pb::TX_CHECKSUM_TRANSPORT_TCP);

  const auto parsed = bess::modules::ParseTxChecksumProfile(message);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->encapsulation.kind, TxEncapsulationKind::kUdp);
  ASSERT_TRUE(parsed->outer.has_value());
  ASSERT_TRUE(parsed->inner.has_value());
  EXPECT_EQ(parsed->outer->transport, TransportChecksum::kUdp);
  EXPECT_EQ(parsed->inner->transport, TransportChecksum::kTcp);
  EXPECT_EQ(parsed->inner->network_offset, 28);
}

}  // namespace
