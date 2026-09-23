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

#include <benchmark/benchmark.h>
#include <glog/logging.h>
#include <rte_ip.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "packet.h"
#include "packet_checksum.h"
#include "packet_pool.h"
#include "utils/checksum.h"

namespace {
namespace utils = bess::utils;

bess::PlainPacketPool &GetChecksumPool() {
  static bess::PlainPacketPool pool(4096, -1, 8192);
  return pool;
}

constexpr size_t kIpOffset = 14;
constexpr size_t kIpHeaderLength = 20;
constexpr size_t kUdpOffset = kIpOffset + kIpHeaderLength;
constexpr size_t kUdpLength = 8;
constexpr size_t kTcpLength = 20;
constexpr size_t kPacketHeaderLength = kUdpOffset + kUdpLength;
constexpr size_t kTcpPacketHeaderLength = kUdpOffset + kTcpLength;
constexpr size_t kWorkingSet = 128;

enum class Protocol : int64_t {
  kTcp = 6,
  kUdp = 17,
};

enum class ChecksumIntent : int64_t {
  kNetworkOnly = 0,
  kTransportOnly = 1,
  kBoth = 2,
};

const bess::packet::ChecksumPlan kUdpPlan{
    kIpOffset, kUdpOffset, bess::packet::IpVersion::kIpv4,
    bess::packet::NetworkChecksum::kIpv4Header,
    bess::packet::TransportChecksum::kUdp};

constexpr bess::packet::ChecksumPlan MakePlan(Protocol protocol,
                                               ChecksumIntent intent) {
  return {
      kIpOffset,
      kUdpOffset,
      bess::packet::IpVersion::kIpv4,
      intent == ChecksumIntent::kTransportOnly
          ? bess::packet::NetworkChecksum::kNone
          : bess::packet::NetworkChecksum::kIpv4Header,
      intent == ChecksumIntent::kNetworkOnly
          ? bess::packet::TransportChecksum::kNone
          : (protocol == Protocol::kUdp ? bess::packet::TransportChecksum::kUdp
                                        : bess::packet::TransportChecksum::kTcp)};
}


enum class Shape : int64_t {
  kContiguous = 0,
  kTwoSplitHeader = 1,
  kTwoSplitChecksum = 2,
  kFourSplitHeaderAndChecksum = 3,
  kWritableHeadSharedTail = 4,
};

std::vector<size_t> SegmentLengths(Shape shape, size_t bytes,
                                   Protocol protocol) {
  const size_t checksum_field_offset =
      kUdpOffset + (protocol == Protocol::kUdp ? 6 : 16);
  const size_t checksum_split = checksum_field_offset + 1;
  switch (shape) {
    case Shape::kContiguous:
      return {bytes};
    case Shape::kTwoSplitHeader:
      return {kIpOffset + 10, bytes - (kIpOffset + 10)};
    case Shape::kTwoSplitChecksum:
      return {checksum_split, bytes - checksum_split};
    case Shape::kFourSplitHeaderAndChecksum:
      return {kIpOffset + 10, checksum_split - (kIpOffset + 10), 1,
              bytes - checksum_split - 1};
    case Shape::kWritableHeadSharedTail:
      return {64, bytes - 64};
  }
  LOG(FATAL) << "Unknown checksum benchmark shape";
  return {};
}

bess::PacketHandle BuildPacket(bess::PlainPacketPool &pool, size_t bytes,
                               Shape shape,
                               Protocol protocol = Protocol::kUdp) {
  const size_t transport_header_length =
      protocol == Protocol::kUdp ? kUdpLength : kTcpLength;
  const size_t header_length = kUdpOffset + transport_header_length;
  CHECK_GE(bytes, header_length);
  std::vector<size_t> lengths = SegmentLengths(shape, bytes, protocol);
  bess::PacketHandle head = nullptr;
  bess::PacketHandle previous = nullptr;
  size_t offset = 0;
  for (size_t i = 0; i < lengths.size(); ++i) {
    const size_t length = lengths[i];
    bess::PacketHandle segment = pool.Alloc(length);
    CHECK(segment != nullptr);
    if (head == nullptr) {
      head = segment;
    } else {
      previous->next = segment;
    }
    auto *dst =
        reinterpret_cast<uint8_t *>(bess::PacketRef(segment).head_data());
    for (size_t j = 0; j < length; ++j) {
      dst[j] = static_cast<uint8_t>((offset + j) * 29 + 7);
    }
    offset += length;
    previous = segment;
  }
  CHECK(head != nullptr);
  head->pkt_len = static_cast<uint32_t>(bytes);
  head->nb_segs = static_cast<uint16_t>(lengths.size());

  std::array<uint8_t, kTcpPacketHeaderLength> header{};
  header[12] = 0x08;
  header[13] = 0x00;
  header[14] = 0x45;
  const uint16_t ip_length = static_cast<uint16_t>(bytes - kIpOffset);
  header[16] = static_cast<uint8_t>(ip_length >> 8);
  header[17] = static_cast<uint8_t>(ip_length);
  header[22] = 64;
  header[23] = static_cast<uint8_t>(protocol);
  header[26] = 192;
  header[27] = 0;
  header[28] = 2;
  header[29] = 1;
  header[30] = 198;
  header[31] = 51;
  header[32] = 100;
  header[33] = 2;
  header[34] = 0x13;
  header[35] = 0x88;
  header[36] = 0x17;
  header[37] = 0x70;
  if (protocol == Protocol::kUdp) {
    const uint16_t udp_length = static_cast<uint16_t>(bytes - kUdpOffset);
    header[38] = static_cast<uint8_t>(udp_length >> 8);
    header[39] = static_cast<uint8_t>(udp_length);
  } else {
    header[kUdpOffset + 12] = 0x50;
  }
  for (size_t i = 0; i < header_length; ++i) {
    size_t segment_offset = 0;
    for (bess::PacketHandle segment = head; segment; segment = segment->next) {
      const size_t segment_length = segment->data_len;
      if (i < segment_offset + segment_length) {
        auto *dst =
            reinterpret_cast<uint8_t *>(bess::PacketRef(segment).head_data());
        dst[i - segment_offset] = header[i];
        break;
      }
      segment_offset += segment_length;
    }
  }
  return head;
}

bess::PacketHandle BuildSharedTailPacket(bess::PlainPacketPool &pool,
                                         size_t bytes,
                                         bess::PacketHandle *tail_owner) {
  CHECK_GE(bytes, 64);
  bess::PacketHandle owner = pool.Alloc(bytes - 64);
  CHECK(owner != nullptr);
  auto *tail_data =
      reinterpret_cast<uint8_t *>(bess::PacketRef(owner).head_data());
  for (size_t i = 0; i < bytes - 64; ++i) {
    tail_data[i] = static_cast<uint8_t>((64 + i) * 29 + 7);
  }
  *tail_owner = owner;

  bess::PacketHandle head = pool.Alloc(64);
  CHECK(head != nullptr);
  auto *head_data =
      reinterpret_cast<uint8_t *>(bess::PacketRef(head).head_data());
  for (size_t i = 0; i < 64; ++i) {
    head_data[i] = static_cast<uint8_t>(i * 29 + 7);
  }
  bess::PacketHandle shared = bess::PacketClone(owner);
  CHECK(shared != nullptr);
  head->next = shared;
  head->pkt_len = static_cast<uint32_t>(bytes);
  head->nb_segs = 2;

  // Header and both checksum fields live in the writable first segment.
  std::array<uint8_t, kPacketHeaderLength> header{};
  header[12] = 0x08;
  header[13] = 0x00;
  header[14] = 0x45;
  const uint16_t ip_length = static_cast<uint16_t>(bytes - kIpOffset);
  header[16] = static_cast<uint8_t>(ip_length >> 8);
  header[17] = static_cast<uint8_t>(ip_length);
  header[22] = 64;
  header[23] = 17;
  header[26] = 192;
  header[28] = 2;
  header[29] = 1;
  header[30] = 198;
  header[32] = 100;
  header[33] = 2;
  header[34] = 0x13;
  header[35] = 0x88;
  header[36] = 0x17;
  header[37] = 0x70;
  const uint16_t udp_length = static_cast<uint16_t>(bytes - kUdpOffset);
  header[38] = static_cast<uint8_t>(udp_length >> 8);
  header[39] = static_cast<uint8_t>(udp_length);
  std::memcpy(head_data + kIpOffset, header.data() + kIpOffset,
              kPacketHeaderLength - kIpOffset);
  head_data[12] = 0x08;
  head_data[13] = 0x00;
  return head;
}

size_t SegmentCount(Shape shape) {
  switch (shape) {
    case Shape::kContiguous:
      return 1;
    case Shape::kTwoSplitHeader:
    case Shape::kTwoSplitChecksum:
    case Shape::kWritableHeadSharedTail:
      return 2;
    case Shape::kFourSplitHeaderAndChecksum:
      return 4;
  }
  return 0;
}

struct ChecksumPair {
  uint16_t network = 0;
  uint16_t transport = 0;
};

enum class ChecksumEngine {
  kBess,
  kDpdk,
  kDpdkMbuf,
};

struct ValidatedIpv4View {
  alignas(utils::Ipv4) std::array<uint8_t, 60> ip_bytes{};
  alignas(utils::Tcp) std::array<uint8_t, sizeof(utils::Tcp)> transport_bytes{};
  const uint8_t *transport_data = nullptr;
  size_t transport_length = 0;
};

bool ReadPacketBytes(bess::PacketHandle packet, size_t offset, size_t length,
                     uint8_t *scratch) {
  const void *data = rte_pktmbuf_read(packet, static_cast<uint32_t>(offset),
                                      static_cast<uint32_t>(length), scratch);
  if (data == nullptr) {
    return false;
  }
  if (data != scratch) {
    std::memcpy(scratch, data, length);
  }
  return true;
}

bool LoadIpv4Header(bess::PacketHandle packet, size_t offset,
                    std::array<uint8_t, 60> *storage) {
  if (packet == nullptr || offset > packet->pkt_len ||
      sizeof(utils::Ipv4) > packet->pkt_len - offset ||
      !ReadPacketBytes(packet, offset, sizeof(utils::Ipv4), storage->data())) {
    return false;
  }
  const auto *ip = reinterpret_cast<const utils::Ipv4 *>(storage->data());
  const size_t header_length = static_cast<size_t>(ip->header_length) * 4;
  return header_length >= sizeof(utils::Ipv4) &&
         header_length <= storage->size() &&
         header_length <= packet->pkt_len - offset &&
         ReadPacketBytes(packet, offset, header_length, storage->data());
}

bool ValidatePacketChain(bess::PacketHandle packet) {
  if (packet == nullptr || packet->nb_segs == 0) {
    return false;
  }
  uint64_t logical_length = 0;
  bess::PacketHandle segment = packet;
  for (uint16_t index = 0; index < packet->nb_segs; ++index) {
    if (segment == nullptr || segment->pool == nullptr ||
        segment->buf_addr == nullptr || segment->data_off > segment->buf_len ||
        segment->data_len > segment->buf_len - segment->data_off) {
      return false;
    }
    logical_length += segment->data_len;
    if (logical_length > UINT32_MAX) {
      return false;
    }
    segment = segment->next;
  }
  return segment == nullptr && logical_length == packet->pkt_len;
}

bool ValidateIpv4Packet(bess::PacketHandle packet,
                        const bess::packet::ChecksumPlan &plan,
                        Protocol protocol, ChecksumIntent intent,
                        ValidatedIpv4View *view) {
  if (!ValidatePacketChain(packet) ||
      plan.ip_version != bess::packet::IpVersion::kIpv4 ||
      (plan.network == bess::packet::NetworkChecksum::kNone &&
       plan.transport == bess::packet::TransportChecksum::kNone) ||
      plan.transport !=
          (intent == ChecksumIntent::kNetworkOnly
               ? bess::packet::TransportChecksum::kNone
               : (protocol == Protocol::kUdp
                      ? bess::packet::TransportChecksum::kUdp
                      : bess::packet::TransportChecksum::kTcp)) ||
      plan.network !=
          (intent == ChecksumIntent::kTransportOnly
               ? bess::packet::NetworkChecksum::kNone
               : bess::packet::NetworkChecksum::kIpv4Header) ||
      plan.network_offset > packet->pkt_len ||
      sizeof(utils::Ipv4) > packet->pkt_len - plan.network_offset ||
      !LoadIpv4Header(packet, plan.network_offset, &view->ip_bytes)) {
    return false;
  }

  const auto *ip =
      reinterpret_cast<const utils::Ipv4 *>(view->ip_bytes.data());
  if (ip->version != 4 || ip->header_length < 5) {
    return false;
  }
  const size_t ip_header_length = static_cast<size_t>(ip->header_length) * 4;
  const size_t ip_total_length = ip->length.value();
  if (ip_header_length > ip_total_length ||
      ip_total_length > packet->pkt_len - plan.network_offset) {
    return false;
  }
  if (intent == ChecksumIntent::kNetworkOnly) {
    return true;
  }

  const uint8_t expected_protocol =
      protocol == Protocol::kUdp ? utils::Ipv4::kUdp : utils::Ipv4::kTcp;
  if (ip->protocol != expected_protocol ||
      (ip->fragment_offset.value() &
       (utils::Ipv4::kMF | static_cast<uint16_t>(0x1fff))) != 0) {
    return false;
  }
  const size_t transport_offset = plan.network_offset + ip_header_length;
  if (plan.transport_offset != transport_offset) {
    return false;
  }
  view->transport_length = ip_total_length - ip_header_length;
  const size_t minimum_header =
      protocol == Protocol::kUdp ? sizeof(utils::Udp) : sizeof(utils::Tcp);
  if (view->transport_length < minimum_header) {
    return false;
  }
  if (!ReadPacketBytes(packet, transport_offset, minimum_header,
                       view->transport_bytes.data())) {
    return false;
  }
  if (protocol == Protocol::kUdp) {
    const auto *udp =
        reinterpret_cast<const utils::Udp *>(view->transport_bytes.data());
    if (udp->length.value() != view->transport_length) {
      return false;
    }
  } else {
    const auto *tcp =
        reinterpret_cast<const utils::Tcp *>(view->transport_bytes.data());
    const size_t tcp_header_length = static_cast<size_t>(tcp->offset) * 4;
    if (tcp_header_length < sizeof(utils::Tcp) ||
        tcp_header_length > view->transport_length) {
      return false;
    }
  }
  view->transport_data = static_cast<const uint8_t *>(
      rte_pktmbuf_read(packet, static_cast<uint32_t>(transport_offset),
                       static_cast<uint32_t>(minimum_header),
                       view->transport_bytes.data()));
  return view->transport_data != nullptr;
}

ChecksumPair ComputeBessChecksum(const utils::Ipv4 *ip,
                                 const uint8_t *transport_data,
                                 Protocol protocol, ChecksumIntent intent) {
  ChecksumPair result;
  if (intent != ChecksumIntent::kTransportOnly) {
    result.network = utils::CalculateIpv4Checksum(*ip);
  }
  if (intent != ChecksumIntent::kNetworkOnly) {
    if (protocol == Protocol::kUdp) {
      result.transport = utils::CalculateIpv4UdpChecksum(
          *ip, *reinterpret_cast<const utils::Udp *>(transport_data));
    } else {
      result.transport = utils::CalculateIpv4TcpChecksum(
          *ip, *reinterpret_cast<const utils::Tcp *>(transport_data));
    }
  }
  return result;
}

ChecksumPair ComputeDpdkChecksum(const rte_ipv4_hdr *ip,
                                const uint8_t *transport_data,
                                Protocol protocol, ChecksumIntent intent) {
  ChecksumPair result;
  if (intent != ChecksumIntent::kTransportOnly) {
    result.network = rte_ipv4_cksum(ip);
  }
  if (intent != ChecksumIntent::kNetworkOnly) {
    result.transport = rte_ipv4_udptcp_cksum(ip, transport_data);
    CHECK(protocol == Protocol::kUdp || protocol == Protocol::kTcp);
  }
  return result;
}

ChecksumPair ComputeDpdkMbufChecksum(
    bess::PacketHandle packet, const utils::Ipv4 *ip, Protocol protocol,
    ChecksumIntent intent, size_t transport_offset) {
  ChecksumPair result;
  const auto *dpdk_ip = reinterpret_cast<const rte_ipv4_hdr *>(ip);
  if (intent != ChecksumIntent::kTransportOnly) {
    result.network = rte_ipv4_cksum(dpdk_ip);
  }
  if (intent != ChecksumIntent::kNetworkOnly) {
    result.transport = rte_ipv4_udptcp_cksum_mbuf(
        packet, dpdk_ip, static_cast<uint16_t>(transport_offset));
    CHECK(protocol == Protocol::kUdp || protocol == Protocol::kTcp);
  }
  return result;
}

ChecksumPair ExpectedPair(
    const std::expected<bess::packet::ChecksumValues,
                        bess::packet::ChecksumError> &expected) {
  CHECK(expected.has_value());
  return {static_cast<uint16_t>(expected->network
                                    ? expected->network->raw_value()
                                    : 0),
          static_cast<uint16_t>(expected->transport
                                    ? expected->transport->raw_value()
                                    : 0)};
}


bool CheckEnginePairMatches(
    const ChecksumPair &actual,
    const std::expected<bess::packet::ChecksumValues,
                        bess::packet::ChecksumError> &expected,
    ChecksumEngine engine, ChecksumIntent intent, Shape shape) {
  const ChecksumPair wanted = ExpectedPair(expected);
  CHECK_EQ(actual.network, wanted.network);
  if (actual.transport == wanted.transport) {
    return true;
  }
  // DPDK 25.11.3's mbuf checksum helper disagrees with the contiguous
  // checksum when the transport checksum field itself is split across mbufs.
  if (engine == ChecksumEngine::kDpdkMbuf &&
      intent != ChecksumIntent::kNetworkOnly &&
      shape == Shape::kTwoSplitChecksum) {
    return false;
  }
  CHECK_EQ(actual.transport, wanted.transport);
  return true;
}

ChecksumPair RawChecksum(bess::PacketHandle packet,
                         const bess::packet::ChecksumPlan &plan,
                         Protocol protocol, ChecksumIntent intent,
                         ChecksumEngine engine) {
  if (engine == ChecksumEngine::kBess ||
      engine == ChecksumEngine::kDpdk) {
    const auto *base = reinterpret_cast<const uint8_t *>(
        bess::PacketRef(packet).head_data());
    const auto *ip =
        reinterpret_cast<const utils::Ipv4 *>(base + plan.network_offset);
    const auto *transport = base + plan.transport_offset;
    if (engine == ChecksumEngine::kBess) {
      return ComputeBessChecksum(ip, transport, protocol, intent);
    }
    return ComputeDpdkChecksum(
        reinterpret_cast<const rte_ipv4_hdr *>(ip), transport, protocol,
        intent);
  }

  std::array<uint8_t, 60> ip_bytes{};
  CHECK(LoadIpv4Header(packet, plan.network_offset, &ip_bytes));
  const auto *ip = reinterpret_cast<const utils::Ipv4 *>(ip_bytes.data());
  return ComputeDpdkMbufChecksum(packet, ip, protocol, intent,
                                 plan.transport_offset);
}

ChecksumPair ValidatedReferenceChecksum(
    bess::PacketHandle packet, const bess::packet::ChecksumPlan &plan,
    Protocol protocol, ChecksumIntent intent, ChecksumEngine engine,
    ValidatedIpv4View *view) {
  CHECK(ValidateIpv4Packet(packet, plan, protocol, intent, view));
  const auto *ip =
      reinterpret_cast<const utils::Ipv4 *>(view->ip_bytes.data());
  if (engine == ChecksumEngine::kBess) {
    return ComputeBessChecksum(ip, view->transport_data, protocol, intent);
  }
  if (engine == ChecksumEngine::kDpdk) {
    CHECK_EQ(packet->nb_segs, 1);
    return ComputeDpdkChecksum(reinterpret_cast<const rte_ipv4_hdr *>(ip),
                               view->transport_data, protocol, intent);
  }
  return ComputeDpdkMbufChecksum(packet, ip, protocol, intent,
                                 plan.transport_offset);
}

void RunEngineBenchmark(benchmark::State &state, ChecksumEngine engine,
                        bool validated, bool chained) {
  bess::PlainPacketPool &pool = GetChecksumPool();
  const size_t bytes = static_cast<size_t>(state.range(0));
  const auto shape =
      chained ? static_cast<Shape>(state.range(1)) : Shape::kContiguous;
  const auto protocol = static_cast<Protocol>(
      state.range(chained ? 2 : 1));
  const auto intent =
      static_cast<ChecksumIntent>(state.range(chained ? 3 : 2));
  const auto plan = MakePlan(protocol, intent);
  bess::PacketHandle packet = BuildPacket(pool, bytes, shape, protocol);
  const auto expected =
      bess::packet::ComputeChecksums(bess::PacketRef(packet), plan);
  ChecksumPair initial;
  if (validated) {
    ValidatedIpv4View view;
    initial = ValidatedReferenceChecksum(packet, plan, protocol, intent,
                                        engine, &view);
  } else {
    initial = RawChecksum(packet, plan, protocol, intent, engine);
  }
  const bool dpdk_checksum_mismatch =
      !CheckEnginePairMatches(initial, expected, engine, intent, shape);

  for (auto _ : state) {
    ChecksumPair result;
    if (validated) {
      ValidatedIpv4View view;
      result = ValidatedReferenceChecksum(packet, plan, protocol, intent,
                                          engine, &view);
    } else {
      result = RawChecksum(packet, plan, protocol, intent, engine);
    }
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["packet_bytes"] = static_cast<double>(bytes);
  state.counters["segments"] = static_cast<double>(SegmentCount(shape));
  state.counters["protocol"] = static_cast<int64_t>(protocol);
  state.counters["checksum_intent"] = static_cast<int64_t>(intent);
  state.counters["dpdk_checksum_mismatch"] = dpdk_checksum_mismatch ? 1 : 0;
  bess::PacketFree(packet);
}

void BM_RawBessContiguous(benchmark::State &state) {
  RunEngineBenchmark(state, ChecksumEngine::kBess, false, false);
}

void BM_RawDpdkContiguous(benchmark::State &state) {
  RunEngineBenchmark(state, ChecksumEngine::kDpdk, false, false);
}

void BM_RawDpdkMbuf(benchmark::State &state) {
  RunEngineBenchmark(state, ChecksumEngine::kDpdkMbuf, false, true);
}

void BM_ValidatedBessContiguous(benchmark::State &state) {
  RunEngineBenchmark(state, ChecksumEngine::kBess, true, false);
}

void BM_ValidatedDpdkContiguous(benchmark::State &state) {
  RunEngineBenchmark(state, ChecksumEngine::kDpdk, true, false);
}

void BM_ValidatedDpdkMbuf(benchmark::State &state) {
  RunEngineBenchmark(state, ChecksumEngine::kDpdkMbuf, true, true);
}

void BM_ComputeChecksums(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetChecksumPool();
  const size_t bytes = static_cast<size_t>(state.range(0));
  const auto shape = static_cast<Shape>(state.range(1));
  const auto protocol = static_cast<Protocol>(state.range(2));
  const auto intent = static_cast<ChecksumIntent>(state.range(3));
  const bool cold = state.range(4) != 0;
  const size_t count = cold ? kWorkingSet : 1;
  const auto plan = MakePlan(protocol, intent);
  std::vector<bess::PacketHandle> packets;
  packets.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    packets.push_back(BuildPacket(pool, bytes, shape, protocol));
  }
  size_t next = 0;
  for (auto _ : state) {
    bess::PacketRef packet(packets[next++ & (count - 1)]);
    auto result = bess::packet::ComputeChecksums(packet, plan);
    CHECK(result.has_value());
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["packet_bytes"] = static_cast<double>(bytes);
  state.counters["segments"] = static_cast<double>(SegmentCount(shape));
  state.counters["protocol"] = static_cast<int64_t>(protocol);
  state.counters["checksum_intent"] = static_cast<int64_t>(intent);
  state.counters["working_set"] = static_cast<double>(count);
  state.counters["cold_rotation"] = cold ? 1 : 0;
  bess::PacketFreeBulk(packets.data(), packets.size());
}

void BM_ApplyChecksumsPlan(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetChecksumPool();
  const size_t bytes = static_cast<size_t>(state.range(0));
  const auto shape = static_cast<Shape>(state.range(1));
  const auto protocol = static_cast<Protocol>(state.range(2));
  const auto intent = static_cast<ChecksumIntent>(state.range(3));
  const auto plan = MakePlan(protocol, intent);
  bess::PacketHandle packet = BuildPacket(pool, bytes, shape, protocol);
  for (auto _ : state) {
    auto result = bess::packet::ApplySoftwareChecksums(packet, plan);
    CHECK(result.has_value());
    benchmark::DoNotOptimize(packet);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["packet_bytes"] = static_cast<double>(bytes);
  state.counters["segments"] = static_cast<double>(SegmentCount(shape));
  state.counters["protocol"] = static_cast<int64_t>(protocol);
  state.counters["checksum_intent"] = static_cast<int64_t>(intent);
  bess::PacketFree(packet);
}

void BM_ApplyChecksumsWritable(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetChecksumPool();
  const size_t bytes = static_cast<size_t>(state.range(0));
  const auto shape = static_cast<Shape>(state.range(1));
  bess::PacketHandle packet = shape == Shape::kWritableHeadSharedTail
                                  ? nullptr
                                  : BuildPacket(pool, bytes, shape);
  bess::PacketHandle tail_owner = nullptr;
  if (shape == Shape::kWritableHeadSharedTail) {
    packet = BuildSharedTailPacket(pool, bytes, &tail_owner);
  }
  for (auto _ : state) {
    auto result = bess::packet::ApplySoftwareChecksums(packet, kUdpPlan);
    CHECK(result.has_value());
    benchmark::DoNotOptimize(packet);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["packet_bytes"] = static_cast<double>(bytes);
  state.counters["segments"] = static_cast<double>(SegmentCount(shape));
  state.counters["writable_head_shared_tail"] =
      shape == Shape::kWritableHeadSharedTail ? 1 : 0;
  bess::PacketFree(packet);
  if (tail_owner != nullptr) {
    bess::PacketFree(tail_owner);
  }
}

void BM_ApplyChecksumsSharedHeaderCow(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetChecksumPool();
  const size_t bytes = static_cast<size_t>(state.range(0));
  bess::PacketHandle owner = BuildPacket(pool, bytes, Shape::kContiguous);
  for (auto _ : state) {
    state.PauseTiming();
    bess::PacketHandle packet = bess::PacketClone(owner);
    CHECK(packet != nullptr);
    state.ResumeTiming();

    auto result = bess::packet::ApplySoftwareChecksums(packet, kUdpPlan);
    CHECK(result.has_value());
    benchmark::DoNotOptimize(packet);

    state.PauseTiming();
    bess::PacketFree(packet);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["packet_bytes"] = static_cast<double>(bytes);
  state.counters["segments"] = 1;
  state.counters["shared_checksum_header_cow"] = 1;
  bess::PacketFree(owner);
}

BENCHMARK(BM_RawBessContiguous)
    ->ArgsProduct({{64, 1500, 4096}, {6, 17}, {0, 1, 2}})
    ->ArgNames({"packet_bytes", "protocol", "checksum_intent"});
BENCHMARK(BM_RawDpdkContiguous)
    ->ArgsProduct({{64, 1500, 4096}, {6, 17}, {0, 1, 2}})
    ->ArgNames({"packet_bytes", "protocol", "checksum_intent"});
BENCHMARK(BM_RawDpdkMbuf)
    ->ArgsProduct({{64, 1500, 4096}, {0, 1, 2, 3}, {6, 17}, {0, 1, 2}})
    ->ArgNames({"packet_bytes", "shape", "protocol", "checksum_intent"});
BENCHMARK(BM_ValidatedBessContiguous)
    ->ArgsProduct({{64, 1500, 4096}, {6, 17}, {0, 1, 2}})
    ->ArgNames({"packet_bytes", "protocol", "checksum_intent"});
BENCHMARK(BM_ValidatedDpdkContiguous)
    ->ArgsProduct({{64, 1500, 4096}, {6, 17}, {0, 1, 2}})
    ->ArgNames({"packet_bytes", "protocol", "checksum_intent"});
BENCHMARK(BM_ValidatedDpdkMbuf)
    ->ArgsProduct({{64, 1500, 4096}, {0, 1, 2, 3}, {6, 17}, {0, 1, 2}})
    ->ArgNames({"packet_bytes", "shape", "protocol", "checksum_intent"});
BENCHMARK(BM_ComputeChecksums)
    ->ArgsProduct(
        {{64, 1500, 4096}, {0, 1, 2, 3}, {6, 17}, {0, 1, 2}, {0, 1}})
    ->ArgNames({"packet_bytes", "shape", "protocol", "checksum_intent",
                "cold_rotation"});
BENCHMARK(BM_ApplyChecksumsPlan)
    ->ArgsProduct({{64, 1500, 4096}, {0, 1, 2, 3}, {6, 17}, {0, 1, 2}})
    ->ArgNames({"packet_bytes", "shape", "protocol", "checksum_intent"});
BENCHMARK(BM_ApplyChecksumsWritable)
    ->ArgsProduct({{1500, 4096}, {4}})
    ->ArgNames({"packet_bytes", "shape"});
BENCHMARK(BM_ApplyChecksumsSharedHeaderCow)
    ->Arg(64)
    ->Arg(1500)
    ->Arg(4096)
    ->ArgNames({"packet_bytes"});




}  // namespace
