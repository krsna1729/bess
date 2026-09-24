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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "control/runtime_state.h"
#include "modules/queue_out.h"
#include "packet.h"
#include "packet_checksum.h"
#include "packet_pool.h"
#include "packet_tx_checksum.h"
#include "port.h"

namespace {

using bess::PacketBatch;
using bess::PacketFree;
using bess::PacketHandle;
using bess::PacketRef;
using bess::PlainPacketPool;
using bess::packet::ApplySoftwareChecksums;
using bess::packet::BindTxFinalizationProfile;
using bess::packet::BoundTxFinalizationProfile;
using bess::packet::ChecksumPlan;
using bess::packet::FinalizeTxPacketBatch;
using bess::packet::IpVersion;
using bess::packet::NetworkChecksum;
using bess::packet::TransportChecksum;
using bess::packet::TxChecksumCapabilities;
using bess::packet::TxEncapsulationKind;
using bess::packet::TxFinalizationProfile;

enum class Variant : int64_t {
  kNoProfilePath = 0,
  kEmptyProfileHelper = 1,
  kSoftwarePrimitive = 2,
  kSoftwareProfile = 3,
  kOneDomainHardware = 4,
  kTwoDomainHardware = 5,
  kMixedSoftwareHardware = 6,
};

enum class Protocol : int64_t {
  kUdp = 0,
  kTcp = 1,
};

enum class Shape : int64_t {
  kContiguous = 0,
  kChainedPayload = 1,
};

class TxChecksumBenchPort final : public Port {
 public:
  CommandResponse Init(const google::protobuf::Any &) {
    return CommandSuccess();
  }
  void DeInit() override {}
  int RecvPackets(queue_t, PacketHandle *, int) override { return 0; }
  int SendPackets(queue_t, PacketHandle *, int count) override {
    sent_packets = count;
    return count;
  }

  int sent_packets = 0;
};

ADD_DRIVER(TxChecksumBenchPort, "tx_checksum_bench",
           "TX checksum benchmark port");

TxChecksumBenchPort *GetTxChecksumBenchPort() {
  static auto *port = [] {
    const auto &drivers = PortBuilder::all_port_builders();
    const auto builder = drivers.find("TxChecksumBenchPort");
    CHECK(builder != drivers.end());
    std::unique_ptr<Port> instance(
        builder->second.CreatePort("tx-checksum-bench"));
    instance->num_queues[PACKET_DIR_OUT] = 1;
    auto *bench_port = static_cast<TxChecksumBenchPort *>(instance.get());
    CHECK(bess::control::runtime().ports().Add(std::move(instance)));
    return bench_port;
  }();
  return port;
}

PlainPacketPool &GetPool() {
  static PlainPacketPool pool(4096, -1, 8192);
  return pool;
}

void Store16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value >> 8);
  bytes[offset + 1] = static_cast<uint8_t>(value);
}

ChecksumPlan OrdinaryPlan(Protocol protocol, size_t network_offset = 0,
                          size_t transport_offset = 20,
                          bool network_checksum = true) {
  return {.network_offset = network_offset,
          .transport_offset = transport_offset,
          .ip_version = IpVersion::kIpv4,
          .network = network_checksum ? NetworkChecksum::kIpv4Header
                                      : NetworkChecksum::kNone,
          .transport = protocol == Protocol::kUdp ? TransportChecksum::kUdp
                                                  : TransportChecksum::kTcp};
}

ChecksumPlan OuterPlan(bool network_checksum = true) {
  return {.network_offset = 0,
          .transport_offset = 20,
          .ip_version = IpVersion::kIpv4,
          .network = network_checksum ? NetworkChecksum::kIpv4Header
                                      : NetworkChecksum::kNone,
          .transport = TransportChecksum::kUdp};
}

TxFinalizationProfile MakeProfile(Variant variant, Protocol protocol) {
  TxFinalizationProfile profile;
  if (variant == Variant::kTwoDomainHardware ||
      variant == Variant::kMixedSoftwareHardware) {
    profile.encapsulation = {.kind = TxEncapsulationKind::kUdp,
                             .outer_ip_version = IpVersion::kIpv4,
                             .outer_network_offset = 0};
    profile.outer = OuterPlan(variant == Variant::kTwoDomainHardware);
    profile.inner =
        OrdinaryPlan(protocol, 28, 48, variant == Variant::kTwoDomainHardware);
  } else {
    profile.outer = OrdinaryPlan(protocol);
  }
  return profile;
}

TxChecksumCapabilities CapabilitiesFor(Variant variant) {
  switch (variant) {
    case Variant::kOneDomainHardware:
      return {.ipv4_header = true,
              .udp = true,
              .tcp = true,
              .multi_segment_tx = true};
    case Variant::kTwoDomainHardware:
      return {.ipv4_header = true,
              .udp = true,
              .tcp = true,
              .outer_ipv4_header = true,
              .outer_udp = true,
              .ip_tunnel = true,
              .udp_tunnel = true,
              .multi_segment_tx = true};
    case Variant::kMixedSoftwareHardware:
      return {.udp = true,
              .tcp = true,
              .udp_tunnel = true,
              .multi_segment_tx = true};
    default:
      return {};
  }
}

size_t RequiredContiguousHeader(Protocol protocol, bool tunneled) {
  const size_t l4_header = protocol == Protocol::kUdp ? 8 : 20;
  return (tunneled ? 28 : 0) + 20 + l4_header;
}

std::vector<uint8_t> MakePacketBytes(size_t packet_size, Protocol protocol,
                                     bool tunneled) {
  const size_t inner_ip_offset = tunneled ? 28 : 0;
  const size_t inner_transport_offset = inner_ip_offset + 20;
  std::vector<uint8_t> bytes(packet_size);
  for (size_t i = 0; i < bytes.size(); i++) {
    bytes[i] = static_cast<uint8_t>(i * 29 + 7);
  }

  const size_t outer_protocol =
      tunneled ? 17 : (protocol == Protocol::kUdp ? 17 : 6);
  bytes[0] = 0x45;
  Store16(bytes, 2, static_cast<uint16_t>(packet_size));
  Store16(bytes, 4, 0);
  Store16(bytes, 6, 0);
  bytes[8] = 64;
  bytes[9] = static_cast<uint8_t>(outer_protocol);
  bytes[10] = 0;
  bytes[11] = 0;
  bytes[12] = 192;
  bytes[13] = 0;
  bytes[14] = 2;
  bytes[15] = 1;
  bytes[16] = 198;
  bytes[17] = 51;
  bytes[18] = 100;
  bytes[19] = 2;

  if (tunneled) {
    bytes[20] = 0x13;
    bytes[21] = 0x88;
    bytes[22] = 0x17;
    bytes[23] = 0x70;
    Store16(bytes, 24, static_cast<uint16_t>(packet_size - 20));
    Store16(bytes, 26, 0);
    bytes[inner_ip_offset] = 0x45;
    Store16(bytes, inner_ip_offset + 2,
            static_cast<uint16_t>(packet_size - inner_ip_offset));
    Store16(bytes, inner_ip_offset + 4, 0);
    Store16(bytes, inner_ip_offset + 6, 0);
    bytes[inner_ip_offset + 8] = 64;
    bytes[inner_ip_offset + 9] = protocol == Protocol::kUdp ? 17 : 6;
    bytes[inner_ip_offset + 10] = 0;
    bytes[inner_ip_offset + 11] = 0;
    bytes[inner_ip_offset + 12] = 192;
    bytes[inner_ip_offset + 14] = 2;
    bytes[inner_ip_offset + 15] = 3;
    bytes[inner_ip_offset + 16] = 198;
    bytes[inner_ip_offset + 17] = 51;
    bytes[inner_ip_offset + 18] = 100;
    bytes[inner_ip_offset + 19] = 4;
  }

  bytes[inner_transport_offset] = 0x13;
  bytes[inner_transport_offset + 1] = 0x88;
  bytes[inner_transport_offset + 2] = 0x17;
  bytes[inner_transport_offset + 3] = 0x70;
  if (protocol == Protocol::kUdp) {
    Store16(bytes, inner_transport_offset + 4,
            static_cast<uint16_t>(packet_size - inner_transport_offset));
    Store16(bytes, inner_transport_offset + 6, 0x1234);
  } else {
    bytes[inner_transport_offset + 12] = 0x50;
    bytes[inner_transport_offset + 13] = 0x02;
    bytes[inner_transport_offset + 16] = 0x12;
    bytes[inner_transport_offset + 17] = 0x34;
  }
  return bytes;
}

PacketHandle BuildPacket(PlainPacketPool &pool, size_t packet_size,
                         Protocol protocol, Shape shape, bool tunneled) {
  std::vector<uint8_t> bytes = MakePacketBytes(packet_size, protocol, tunneled);
  const size_t header_length = RequiredContiguousHeader(protocol, tunneled);
  if (shape == Shape::kChainedPayload && packet_size > header_length) {
    const size_t lengths[] = {header_length, packet_size - header_length};
    PacketHandle head = nullptr;
    PacketHandle previous = nullptr;
    size_t offset = 0;
    for (size_t length : lengths) {
      PacketHandle segment = pool.Alloc(length);
      CHECK(segment != nullptr);
      if (head == nullptr) {
        head = segment;
      } else {
        previous->next = segment;
      }
      std::memcpy(PacketRef(segment).head_data(), bytes.data() + offset,
                  length);
      offset += length;
      previous = segment;
    }
    head->pkt_len = static_cast<uint32_t>(packet_size);
    head->nb_segs = 2;
    return head;
  }

  PacketHandle packet = pool.Alloc(packet_size);
  CHECK(packet != nullptr);
  std::memcpy(PacketRef(packet).head_data(), bytes.data(), packet_size);
  return packet;
}

void BenchmarkTxFinalization(benchmark::State &state) {
  const auto variant = static_cast<Variant>(state.range(0));
  const auto protocol = static_cast<Protocol>(state.range(1));
  const size_t packet_size = static_cast<size_t>(state.range(2));
  const int batch_size = static_cast<int>(state.range(3));
  const auto shape = static_cast<Shape>(state.range(4));
  const bool tunneled = variant == Variant::kTwoDomainHardware ||
                        variant == Variant::kMixedSoftwareHardware;
  const bool use_chain = shape == Shape::kChainedPayload;
  const size_t header_length = RequiredContiguousHeader(protocol, tunneled);
  CHECK_GE(packet_size, header_length);
  CHECK(!use_chain || packet_size > header_length);

  TxFinalizationProfile requested_profile;
  BoundTxFinalizationProfile bound_profile;
  if (variant != Variant::kNoProfilePath &&
      variant != Variant::kEmptyProfileHelper &&
      variant != Variant::kSoftwarePrimitive) {
    requested_profile = MakeProfile(variant, protocol);
    const auto bound =
        BindTxFinalizationProfile(requested_profile, CapabilitiesFor(variant));
    CHECK(bound.has_value());
    bound_profile = *bound;
  } else if (variant == Variant::kSoftwarePrimitive) {
    requested_profile = MakeProfile(Variant::kSoftwareProfile, protocol);
  }

  PacketBatch batch;
  batch.clear();
  const bool packet_tunneled = tunneled;
  std::vector<PacketHandle> packets;
  packets.reserve(batch_size);
  for (int i = 0; i < batch_size; i++) {
    PacketHandle packet =
        BuildPacket(GetPool(), packet_size, protocol,
                    use_chain ? Shape::kChainedPayload : Shape::kContiguous,
                    packet_tunneled);
    packets.push_back(packet);
    batch.add(packet);
  }

  switch (variant) {
    case Variant::kNoProfilePath:
      for (auto _ : state) {
        benchmark::DoNotOptimize(batch.handles());
        benchmark::ClobberMemory();
      }
      break;
    case Variant::kEmptyProfileHelper:
      for (auto _ : state) {
        const auto result = FinalizeTxPacketBatch(batch, {});
        benchmark::DoNotOptimize(result.rejected);
        benchmark::ClobberMemory();
      }
      break;
    case Variant::kSoftwarePrimitive: {
      const auto plan = *requested_profile.outer;
      for (auto _ : state) {
        for (int i = 0; i < batch_size; i++) {
          const auto result = ApplySoftwareChecksums(packets[i], plan);
          CHECK(result.has_value()) << "software checksum baseline failed";
        }
        benchmark::ClobberMemory();
      }
      break;
    }
    case Variant::kSoftwareProfile:
    case Variant::kOneDomainHardware:
    case Variant::kTwoDomainHardware:
    case Variant::kMixedSoftwareHardware:
      for (auto _ : state) {
        const auto result = FinalizeTxPacketBatch(batch, bound_profile);
        CHECK_EQ(result.rejected, 0U)
            << "finalizer rejected a benchmark packet";
        benchmark::DoNotOptimize(result.rejected);
        benchmark::ClobberMemory();
      }
      break;
  }

  state.SetItemsProcessed(state.iterations() * batch_size);
  state.SetBytesProcessed(state.iterations() * batch_size * packet_size);
  for (int i = 0; i < batch.cnt(); i++) {
    PacketFree(batch.handles()[i]);
  }
}

void BenchmarkQueueOutEgress(benchmark::State &state) {
  const size_t packet_size = static_cast<size_t>(state.range(0));
  const auto protocol = static_cast<Protocol>(state.range(1));
  const int batch_size = static_cast<int>(state.range(2));
  const bool with_profile = state.range(3) != 0;

  bess::pb::QueueOutArg arg;
  arg.set_port("tx-checksum-bench");
  arg.set_qid(0);
  if (with_profile) {
    auto *domain = arg.mutable_tx_checksum_profile()->mutable_outer();
    domain->set_network_offset(0);
    domain->set_transport_offset(20);
    domain->set_ip_version(bess::pb::CHECKSUM_IP_VERSION_IPV4);
    domain->set_network(bess::pb::TX_CHECKSUM_NETWORK_IPV4_HEADER);
    domain->set_transport(protocol == Protocol::kUdp
                              ? bess::pb::TX_CHECKSUM_TRANSPORT_UDP
                              : bess::pb::TX_CHECKSUM_TRANSPORT_TCP);
  }

  TxChecksumBenchPort *port = GetTxChecksumBenchPort();
  QueueOut output;
  CHECK(!output.Init(arg).has_error());

  PacketBatch batch;
  batch.clear();
  for (int i = 0; i < batch_size; i++) {
    batch.add(BuildPacket(GetPool(), packet_size, protocol, Shape::kContiguous,
                          false));
    CHECK(batch.handles()[i] != nullptr);
  }

  for (auto _ : state) {
    output.ProcessBatch(nullptr, &batch);
    benchmark::DoNotOptimize(port->sent_packets);
    benchmark::ClobberMemory();
  }

  const bool sent_all = port->sent_packets == batch_size;
  output.DeInit();
  state.SetItemsProcessed(state.iterations() * batch_size);
  state.SetBytesProcessed(state.iterations() * batch_size * packet_size);
  for (int i = 0; i < batch.cnt(); i++) {
    PacketFree(batch.handles()[i]);
  }
  CHECK(sent_all);
}

BENCHMARK(BenchmarkQueueOutEgress)
    ->ArgsProduct({{64, 1500, 4096}, {0, 1}, {1, 8, 32}, {0, 1}})
    ->ArgNames({"packet_bytes", "protocol", "batch", "profile"});

BENCHMARK(BenchmarkTxFinalization)->Apply([](benchmark::Benchmark *benchmark) {
  constexpr int64_t kVariants[] = {0, 1, 2, 3, 4, 5, 6};
  constexpr int64_t kProtocols[] = {0, 1};
  constexpr int64_t kSizes[] = {64, 1500, 4096};
  constexpr int64_t kBatchSizes[] = {1, 8, 32};
  constexpr int64_t kShapes[] = {0, 1};
  for (int64_t variant_value : kVariants) {
    const auto variant = static_cast<Variant>(variant_value);
    const bool tunneled = variant == Variant::kTwoDomainHardware ||
                          variant == Variant::kMixedSoftwareHardware;
    for (int64_t protocol_value : kProtocols) {
      const auto protocol = static_cast<Protocol>(protocol_value);
      const size_t min_size = RequiredContiguousHeader(protocol, tunneled);
      for (int64_t packet_size : kSizes) {
        if (static_cast<size_t>(packet_size) < min_size) {
          continue;
        }
        for (int64_t shape_value : kShapes) {
          const auto shape = static_cast<Shape>(shape_value);
          if (shape == Shape::kChainedPayload &&
              static_cast<size_t>(packet_size) == min_size) {
            continue;
          }
          for (int64_t batch_size : kBatchSizes) {
            benchmark->Args({variant_value, protocol_value, packet_size,
                             batch_size, shape_value});
          }
        }
      }
    }
  }
});

}  // namespace
