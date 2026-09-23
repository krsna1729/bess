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
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "packet.h"
#include "packet_checksum.h"
#include "packet_pool.h"

namespace {

bess::PlainPacketPool &GetChecksumPool() {
  static bess::PlainPacketPool pool(4096, -1, 8192);
  return pool;
}

constexpr size_t kIpOffset = 14;
constexpr size_t kIpHeaderLength = 20;
constexpr size_t kUdpOffset = kIpOffset + kIpHeaderLength;
constexpr size_t kUdpLength = 8;
constexpr size_t kPacketHeaderLength = kUdpOffset + kUdpLength;
constexpr size_t kWorkingSet = 128;

const bess::packet::ChecksumPlan kUdpPlan{
    kIpOffset, kUdpOffset, bess::packet::IpVersion::kIpv4,
    bess::packet::NetworkChecksum::kIpv4Header,
    bess::packet::TransportChecksum::kUdp};

enum class Shape : int64_t {
  kContiguous = 0,
  kTwoSplitHeader = 1,
  kTwoSplitChecksum = 2,
  kFourSplitHeaderAndChecksum = 3,
  kWritableHeadSharedTail = 4,
};

std::vector<size_t> SegmentLengths(Shape shape, size_t bytes) {
  switch (shape) {
    case Shape::kContiguous:
      return {bytes};
    case Shape::kTwoSplitHeader:
      return {kIpOffset + 10, bytes - (kIpOffset + 10)};
    case Shape::kTwoSplitChecksum:
      // UDP checksum occupies bytes 40-41; this boundary splits its field.
      return {kUdpOffset + 7, bytes - (kUdpOffset + 7)};
    case Shape::kFourSplitHeaderAndChecksum:
      return {kIpOffset + 10, kUdpOffset + 7 - (kIpOffset + 10), 1,
              bytes - (kUdpOffset + 8)};
    case Shape::kWritableHeadSharedTail:
      return {64, bytes - 64};
  }
  LOG(FATAL) << "Unknown checksum benchmark shape";
  return {};
}

bess::PacketHandle BuildPacket(bess::PlainPacketPool &pool, size_t bytes,
                               Shape shape) {
  CHECK_GE(bytes, kPacketHeaderLength);
  std::vector<size_t> lengths = SegmentLengths(shape, bytes);
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
  const uint16_t udp_length = static_cast<uint16_t>(bytes - kUdpOffset);
  header[38] = static_cast<uint8_t>(udp_length >> 8);
  header[39] = static_cast<uint8_t>(udp_length);
  for (size_t i = 0; i < header.size(); ++i) {
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

void BM_ComputeChecksums(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetChecksumPool();
  const size_t bytes = static_cast<size_t>(state.range(0));
  const auto shape = static_cast<Shape>(state.range(1));
  const bool cold = state.range(2) != 0;
  const size_t count = cold ? kWorkingSet : 1;
  std::vector<bess::PacketHandle> packets;
  packets.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    packets.push_back(BuildPacket(pool, bytes, shape));
  }
  size_t next = 0;
  for (auto _ : state) {
    bess::PacketRef packet(packets[next++ & (count - 1)]);
    auto result = bess::packet::ComputeChecksums(packet, kUdpPlan);
    CHECK(result.has_value());
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["packet_bytes"] = static_cast<double>(bytes);
  state.counters["segments"] = static_cast<double>(SegmentCount(shape));
  state.counters["working_set"] = static_cast<double>(count);
  state.counters["cold_rotation"] = cold ? 1 : 0;
  bess::PacketFreeBulk(packets.data(), packets.size());
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

BENCHMARK(BM_ComputeChecksums)
    ->ArgsProduct({{64, 1500, 4096}, {0, 1, 2, 3}, {0, 1}})
    ->ArgNames({"packet_bytes", "shape", "cold_rotation"});
BENCHMARK(BM_ApplyChecksumsWritable)
    ->ArgsProduct({{64, 1500, 4096}, {0, 1, 2, 3}})
    ->ArgNames({"packet_bytes", "shape"});
BENCHMARK(BM_ApplyChecksumsWritable)
    ->ArgsProduct({{1500, 4096}, {4}})
    ->ArgNames({"packet_bytes", "shape"});
BENCHMARK(BM_ApplyChecksumsSharedHeaderCow)
    ->Arg(64)
    ->Arg(1500)
    ->Arg(4096)
    ->ArgNames({"packet_bytes"});

}  // namespace
