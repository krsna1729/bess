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

// Baseline microbenchmarks for PacketHandle/PacketRef/PacketPool/PacketBatch
// -- the primitives Phase B (packet/mbuf architecture refactor, see
// MODERNIZATION.md) touches. Captured at the Stage 2A boundary so the native
// handle redesign can be checked against real numbers instead of intuition.
//
// Uses PlainPacketPool (see packet_pool.h): the only pool backend that
// doesn't require real hugepages, which this sandbox doesn't have. Not
// representative of DpdkPacketPool/BessPacketPool's absolute numbers, but
// the operations benchmarked here (Alloc/Free bookkeeping, accessor
// arithmetic, batch pointer-array copies) don't depend on which pool
// backend supplied the underlying memory.

#include <benchmark/benchmark.h>
#include <glog/logging.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "packet.h"
#include "packet_cursor.h"
#include "packet_mutation.h"
#include "packet_pool.h"
#include "packet_reshape.h"
#include "pktbatch.h"

namespace {

bess::PlainPacketPool &GetPool() {
  // Small on purpose: every benchmark below holds at most kMaxBurst
  // packets live at a time, and a small pool keeps this binary's startup
  // (and any future CI run of it) fast.
  static bess::PlainPacketPool pool(4096);
  return pool;
}

void BM_PacketAllocFree(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  for (auto _ : state) {
    bess::PacketHandle pkt = pool.Alloc();
    benchmark::DoNotOptimize(pkt);
    bess::PacketFree(pkt);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PacketAllocFree);

void BM_PacketAllocFreeBulk(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t kBatch = bess::PacketBatch::kMaxBurst;
  bess::PacketHandle pkts[kBatch];

  for (auto _ : state) {
    bool ok = pool.AllocBulk(pkts, kBatch);
    benchmark::DoNotOptimize(pkts);
    if (ok) {
      bess::PacketFreeBulk(pkts, kBatch);
    }
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_PacketAllocFreeBulk);

void BM_PacketHeadData(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  constexpr size_t kPackets = 32;
  bess::PacketHandle pkts[kPackets];
  CHECK(pool.AllocBulk(pkts, kPackets, 64));
  size_t next = 0;

  for (auto _ : state) {
    bess::PacketRef pkt(pkts[next++ & (kPackets - 1)]);
    char *data = pkt.head_data<char *>();
    benchmark::DoNotOptimize(data);
  }
  state.SetItemsProcessed(state.iterations());

  bess::PacketFreeBulk(pkts, kPackets);
}
BENCHMARK(BM_PacketHeadData);
void BM_PacketAppendTrim(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  bess::PacketHandle pkt_handle = pool.Alloc();
  bess::PacketRef pkt(pkt_handle);
  const uint16_t kLen = 64;

  for (auto _ : state) {
    void *data = pkt.append(kLen);
    benchmark::DoNotOptimize(data);
    pkt.trim(kLen);
  }
  state.SetItemsProcessed(state.iterations());

  bess::PacketFree(pkt_handle);
}
BENCHMARK(BM_PacketAppendTrim);

// Every module attribute read/write (Module::get_attr/set_attr/ptr_attr,
// module.h) funnels through PacketRef::metadata<T>(), which resolves via
// rte_mbuf_to_priv(). This exercises that path directly to catch a regression
// in cost, not just correctness.
void BM_PacketMetadataAccess(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  constexpr size_t kPackets = 32;
  bess::PacketHandle pkts[kPackets];
  CHECK(pool.AllocBulk(pkts, kPackets));
  size_t next = 0;

  for (auto _ : state) {
    bess::PacketRef pkt(pkts[next++ & (kPackets - 1)]);
    uintptr_t addr = pkt.metadata<uintptr_t>();
    benchmark::DoNotOptimize(addr);
  }
  state.SetItemsProcessed(state.iterations());

  bess::PacketFreeBulk(pkts, kPackets);
}
BENCHMARK(BM_PacketMetadataAccess);

// "Forwarding" a batch from one module to the next is, at its core, a
void BM_BatchForward(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t kBatch = bess::PacketBatch::kMaxBurst;
  bess::PacketHandle pkts[kBatch];
  CHECK(pool.AllocBulk(pkts, kBatch));

  bess::PacketBatch src;
  src.clear();
  for (size_t i = 0; i < kBatch; i++) {
    src.handles()[i] = pkts[i];
  }
  src.set_cnt(kBatch);

  bess::PacketBatch dst;
  for (auto _ : state) {
    dst.clear();
    dst.add(&src);
    benchmark::DoNotOptimize(dst.handles());
  }
  state.SetItemsProcessed(state.iterations() * kBatch);

  bess::PacketFreeBulk(pkts, kBatch);
}
BENCHMARK(BM_BatchForward);

// Build the chain shapes used by the cursor benchmark. Shape 0 is contiguous;
// shapes 1 and 2 are a two-segment packet with a read before or at the
// boundary; shape 3 crosses two boundaries.
bess::PacketHandle BuildCursorBenchmarkChain(bess::PlainPacketPool &pool,
                                             size_t shape) {
  const std::array<std::array<size_t, 4>, 4> lengths = {{
      {128, 0, 0, 0},
      {64, 64, 0, 0},
      {64, 64, 0, 0},
      {16, 16, 16, 80},
  }};
  const std::array<size_t, 4> counts = {1, 2, 2, 4};
  CHECK_LT(shape, lengths.size());

  bess::PacketHandle head = nullptr;
  bess::PacketHandle previous = nullptr;
  size_t total_len = 0;
  for (size_t i = 0; i < counts[shape]; i++) {
    const size_t length = lengths[shape][i];
    bess::PacketHandle segment = pool.Alloc(length);
    CHECK(segment != nullptr);
    if (head == nullptr) {
      head = segment;
    } else {
      previous->next = segment;
    }
    previous = segment;
    std::memset(bess::PacketRef(segment).head_data(), 0, length);
    total_len += length;
  }
  head->pkt_len = static_cast<uint32_t>(total_len);
  head->nb_segs = static_cast<uint16_t>(counts[shape]);
  return head;
}

template <size_t Width>
void ReadCursorWidth(bess::packet::PacketCursor &cursor) {
  if constexpr (Width == 1) {
    benchmark::DoNotOptimize(cursor.Read<uint8_t>());
  } else if constexpr (Width == 2) {
    benchmark::DoNotOptimize(cursor.Read<uint16_t>());
  } else if constexpr (Width == 4) {
    benchmark::DoNotOptimize(cursor.Read<uint32_t>());
  } else if constexpr (Width == 8) {
    benchmark::DoNotOptimize(cursor.Read<uint64_t>());
  } else {
    using Bytes = std::array<std::byte, Width>;
    benchmark::DoNotOptimize(cursor.Read<Bytes>());
  }
}

void ReadCursorWidth(bess::packet::PacketCursor &cursor, size_t width) {
  switch (width) {
    case 1:
      ReadCursorWidth<1>(cursor);
      break;
    case 2:
      ReadCursorWidth<2>(cursor);
      break;
    case 4:
      ReadCursorWidth<4>(cursor);
      break;
    case 8:
      ReadCursorWidth<8>(cursor);
      break;
    case 16:
      ReadCursorWidth<16>(cursor);
      break;
    case 32:
      ReadCursorWidth<32>(cursor);
      break;
    default:
      CHECK(false) << "unsupported cursor benchmark width: " << width;
  }
}

void BM_PacketCursorConstruct(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t batch = static_cast<size_t>(state.range(0));
  constexpr size_t kMaxBatch = 32;
  std::array<bess::PacketHandle, kMaxBatch> pkts{};
  CHECK(pool.AllocBulk(pkts.data(), batch, 128));

  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      bess::packet::PacketCursor cursor{bess::PacketRef(pkts[i])};
      benchmark::DoNotOptimize(cursor);
    }
  }
  state.SetItemsProcessed(state.iterations() * batch);
  state.counters["cursor_constructs/read"] = 1;
  state.counters["bytes_copied/read"] = 0;

  bess::PacketFreeBulk(pkts.data(), batch);
}

void BM_PacketCursorPositionedRead(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t width = static_cast<size_t>(state.range(0));
  const size_t offset = static_cast<size_t>(state.range(1));
  const size_t batch = static_cast<size_t>(state.range(2));
  constexpr size_t kMaxBatch = 32;
  std::array<bess::PacketHandle, kMaxBatch> pkts{};
  std::array<std::optional<bess::packet::PacketCursor>, kMaxBatch> cursors{};
  CHECK(pool.AllocBulk(pkts.data(), batch, 128));
  for (size_t i = 0; i < batch; i++) {
    cursors[i].emplace(bess::PacketRef(pkts[i]));
    CHECK(cursors[i]->Skip(offset));
  }

  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      bess::packet::PacketCursor cursor = *cursors[i];
      ReadCursorWidth(cursor, width);
      benchmark::DoNotOptimize(cursor);
    }
  }
  state.SetItemsProcessed(state.iterations() * batch);
  state.SetBytesProcessed(state.iterations() * batch * width);
  state.counters["cursor_copies/read"] = 1;
  state.counters["bytes_copied/read"] = static_cast<double>(width);
  state.counters["segment_transitions/read"] = 0;

  bess::PacketFreeBulk(pkts.data(), batch);
}

void ReadSequentialFields(bess::packet::PacketCursor &cursor) {
  benchmark::DoNotOptimize(cursor.Read<uint8_t>());
  benchmark::DoNotOptimize(cursor.Read<uint16_t>());
  benchmark::DoNotOptimize(cursor.Read<uint32_t>());
  benchmark::DoNotOptimize(cursor.Read<uint64_t>());
}

void BM_PacketCursorSequentialRead(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t batch = static_cast<size_t>(state.range(0));
  constexpr size_t kMaxBatch = 32;
  constexpr size_t kBytesPerRead =
      sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint64_t);
  std::array<bess::PacketHandle, kMaxBatch> pkts{};
  CHECK(pool.AllocBulk(pkts.data(), batch, 32));

  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      bess::packet::PacketCursor cursor{bess::PacketRef(pkts[i])};
      ReadSequentialFields(cursor);
      benchmark::DoNotOptimize(cursor);
    }
  }
  state.SetItemsProcessed(state.iterations() * batch * 4);
  state.SetBytesProcessed(state.iterations() * batch * kBytesPerRead);
  state.counters["fields/read"] = 4;
  state.counters["bytes_copied/read"] = static_cast<double>(kBytesPerRead);
  state.counters["segment_transitions/read"] = 0;

  bess::PacketFreeBulk(pkts.data(), batch);
}

BENCHMARK(BM_PacketCursorConstruct)
    ->ArgsProduct({{1, 8, 32}})
    ->ArgNames({"batch"});
BENCHMARK(BM_PacketCursorPositionedRead)
    ->ArgsProduct({{1, 2, 4, 8, 16, 32}, {0, 14, 34, 64}, {1, 8, 32}})
    ->ArgNames({"width", "offset", "batch"});
BENCHMARK(BM_PacketCursorSequentialRead)
    ->ArgsProduct({{1, 8, 32}})
    ->ArgNames({"batch"});

enum class MutationBenchmarkOp : uint8_t {
  kPrepend,
  kAppend,
  kRemovePrefix,
  kTrimSuffix,
};

bess::PacketHandle BuildMutationBenchmarkPacket(bess::PlainPacketPool &pool,
                                                size_t shape) {
  const std::array<std::array<size_t, 2>, 2> lengths = {{
      {128, 0},
      {64, 64},
  }};
  const std::array<size_t, 2> counts = {1, 2};
  CHECK_LT(shape, lengths.size());

  bess::PacketHandle head = nullptr;
  bess::PacketHandle previous = nullptr;
  size_t total_len = 0;
  for (size_t i = 0; i < counts[shape]; i++) {
    const size_t length = lengths[shape][i];
    bess::PacketHandle segment = pool.Alloc(length);
    CHECK(segment != nullptr);
    if (head == nullptr) {
      head = segment;
    } else {
      previous->next = segment;
    }
    previous = segment;
    std::memset(bess::PacketRef(segment).head_data(), 0, length);
    total_len += length;
  }
  head->pkt_len = static_cast<uint32_t>(total_len);
  head->nb_segs = static_cast<uint16_t>(counts[shape]);
  return head;
}

template <bool Checked>
void RunMutationBenchmark(benchmark::State &state,
                          MutationBenchmarkOp operation) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t bytes = static_cast<size_t>(state.range(0));
  const size_t shape = static_cast<size_t>(state.range(1));
  const size_t batch = static_cast<size_t>(state.range(2));
  constexpr size_t kMaxBatch = 32;
  std::array<bess::PacketHandle, kMaxBatch> pkts{};
  for (size_t i = 0; i < batch; i++) {
    pkts[i] = BuildMutationBenchmarkPacket(pool, shape);
  }

  auto apply = [&](bess::PacketHandle packet) {
    bess::PacketRef ref(packet);
    if constexpr (Checked) {
      switch (operation) {
        case MutationBenchmarkOp::kPrepend:
          benchmark::DoNotOptimize(bess::packet::PrependInPlace(ref, bytes));
          break;
        case MutationBenchmarkOp::kAppend:
          benchmark::DoNotOptimize(bess::packet::AppendInPlace(ref, bytes));
          break;
        case MutationBenchmarkOp::kRemovePrefix:
          benchmark::DoNotOptimize(
              bess::packet::RemovePrefixInPlace(ref, bytes));
          break;
        case MutationBenchmarkOp::kTrimSuffix:
          benchmark::DoNotOptimize(bess::packet::TrimSuffixInPlace(ref, bytes));
          break;
      }
    } else {
      switch (operation) {
        case MutationBenchmarkOp::kPrepend:
          benchmark::DoNotOptimize(
              rte_pktmbuf_prepend(packet, static_cast<uint16_t>(bytes)));
          break;
        case MutationBenchmarkOp::kAppend:
          benchmark::DoNotOptimize(
              rte_pktmbuf_append(packet, static_cast<uint16_t>(bytes)));
          break;
        case MutationBenchmarkOp::kRemovePrefix:
          benchmark::DoNotOptimize(
              rte_pktmbuf_adj(packet, static_cast<uint16_t>(bytes)));
          break;
        case MutationBenchmarkOp::kTrimSuffix:
          benchmark::DoNotOptimize(
              rte_pktmbuf_trim(packet, static_cast<uint16_t>(bytes)));
          break;
      }
    }
  };

  auto reset = [&](bess::PacketHandle packet) {
    switch (operation) {
      case MutationBenchmarkOp::kPrepend:
        CHECK(rte_pktmbuf_adj(packet, static_cast<uint16_t>(bytes)) != nullptr);
        break;
      case MutationBenchmarkOp::kAppend:
        CHECK(rte_pktmbuf_trim(packet, static_cast<uint16_t>(bytes)) == 0);
        break;
      case MutationBenchmarkOp::kRemovePrefix:
        CHECK(rte_pktmbuf_prepend(packet, static_cast<uint16_t>(bytes)) !=
              nullptr);
        break;
      case MutationBenchmarkOp::kTrimSuffix:
        CHECK(rte_pktmbuf_append(packet, static_cast<uint16_t>(bytes)) !=
              nullptr);
        break;
    }
  };

  apply(pkts[0]);
  reset(pkts[0]);
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      apply(pkts[i]);
    }
    state.PauseTiming();
    for (size_t i = 0; i < batch; i++) {
      reset(pkts[i]);
    }
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * batch);
  state.counters["bytes/mutation"] = static_cast<double>(bytes);
  state.counters["segments/mutation"] = shape == 0 ? 1 : 2;

  bess::PacketFreeBulk(pkts.data(), batch);
}

void BM_PacketRawPrepend(benchmark::State &state) {
  RunMutationBenchmark<false>(state, MutationBenchmarkOp::kPrepend);
}

void BM_PacketCheckedPrepend(benchmark::State &state) {
  RunMutationBenchmark<true>(state, MutationBenchmarkOp::kPrepend);
}

void BM_PacketRawAppend(benchmark::State &state) {
  RunMutationBenchmark<false>(state, MutationBenchmarkOp::kAppend);
}

void BM_PacketCheckedAppend(benchmark::State &state) {
  RunMutationBenchmark<true>(state, MutationBenchmarkOp::kAppend);
}

void BM_PacketRawRemovePrefix(benchmark::State &state) {
  RunMutationBenchmark<false>(state, MutationBenchmarkOp::kRemovePrefix);
}

void BM_PacketCheckedRemovePrefix(benchmark::State &state) {
  RunMutationBenchmark<true>(state, MutationBenchmarkOp::kRemovePrefix);
}

void BM_PacketRawTrimSuffix(benchmark::State &state) {
  RunMutationBenchmark<false>(state, MutationBenchmarkOp::kTrimSuffix);
}

void BM_PacketCheckedTrimSuffix(benchmark::State &state) {
  RunMutationBenchmark<true>(state, MutationBenchmarkOp::kTrimSuffix);
}

struct MutationBenchExternalOwner {
  int *free_count;
};

void FreeMutationBenchExternal(void *address, void *opaque) {
  auto *owner = static_cast<MutationBenchExternalOwner *>(opaque);
  ++*owner->free_count;
  delete[] static_cast<unsigned char *>(address);
  delete owner;
}

bess::PacketHandle BuildMutationBenchExternal(
    bess::PlainPacketPool &pool, int *free_count,
    rte_mbuf_ext_shared_info **shinfo_out) {
  auto *buffer = new unsigned char[4096];
  auto *owner = new MutationBenchExternalOwner{free_count};
  uint16_t buffer_len = 4096;
  auto *shinfo = rte_pktmbuf_ext_shinfo_init_helper(
      buffer, &buffer_len, FreeMutationBenchExternal, owner);
  CHECK(shinfo != nullptr);
  bess::PacketHandle packet =
      pool.AllocExternal(buffer, RTE_BAD_IOVA, buffer_len, shinfo, 64);
  CHECK(packet != nullptr);
  *shinfo_out = shinfo;
  return packet;
}

void BM_PayloadWriteability(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t kind = static_cast<size_t>(state.range(0));
  CHECK_LT(kind, 4u);
  int free_count = 0;
  rte_mbuf_ext_shared_info *shinfo = nullptr;
  bess::PacketHandle packet = nullptr;
  bess::PacketHandle sibling = nullptr;

  switch (kind) {
    case 0:
      packet = pool.Alloc(64);
      break;
    case 1: {
      sibling = pool.Alloc(64);
      CHECK(sibling != nullptr);
      packet = bess::PacketClone(sibling);
      CHECK(packet != nullptr);
      break;
    }
    case 2:
      packet = BuildMutationBenchExternal(pool, &free_count, &shinfo);
      break;
    case 3: {
      sibling = BuildMutationBenchExternal(pool, &free_count, &shinfo);
      packet = bess::PacketClone(sibling);
      CHECK(packet != nullptr);
      break;
    }
  }
  CHECK(packet != nullptr);

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        bess::packet::PayloadWriteabilityOf(bess::PacketRef(packet)));
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["writable"] =
      bess::packet::PayloadWriteabilityOf(bess::PacketRef(packet)) ==
              bess::packet::PayloadWriteability::kWritable
          ? 1
          : 0;

  if (sibling != nullptr) {
    bess::PacketFree(packet);
    bess::PacketFree(sibling);
  } else {
    bess::PacketFree(packet);
  }
  CHECK_EQ(free_count, kind >= 2 ? 1 : 0);
}

BENCHMARK(BM_PacketRawPrepend)
    ->ArgsProduct({{14, 20, 36, 64}, {0, 1}, {1, 8, 32}})
    ->ArgNames({"bytes", "shape", "batch"});
BENCHMARK(BM_PacketCheckedPrepend)
    ->ArgsProduct({{14, 20, 36, 64}, {0, 1}, {1, 8, 32}})
    ->ArgNames({"bytes", "shape", "batch"});
BENCHMARK(BM_PacketRawAppend)
    ->ArgsProduct({{8, 32, 64}, {0, 1}, {1, 8, 32}})
    ->ArgNames({"bytes", "shape", "batch"});
BENCHMARK(BM_PacketCheckedAppend)
    ->ArgsProduct({{8, 32, 64}, {0, 1}, {1, 8, 32}})
    ->ArgNames({"bytes", "shape", "batch"});
BENCHMARK(BM_PacketRawRemovePrefix)
    ->ArgsProduct({{14, 20, 36}, {0, 1}, {1, 8, 32}})
    ->ArgNames({"bytes", "shape", "batch"});
BENCHMARK(BM_PacketCheckedRemovePrefix)
    ->ArgsProduct({{14, 20, 36}, {0, 1}, {1, 8, 32}})
    ->ArgNames({"bytes", "shape", "batch"});
BENCHMARK(BM_PacketRawTrimSuffix)
    ->ArgsProduct({{14, 20, 36}, {0, 1}, {1, 8, 32}})
    ->ArgNames({"bytes", "shape", "batch"});
BENCHMARK(BM_PacketCheckedTrimSuffix)
    ->ArgsProduct({{14, 20, 36}, {0, 1}, {1, 8, 32}})
    ->ArgNames({"bytes", "shape", "batch"});
BENCHMARK(BM_PayloadWriteability)
    ->ArgsProduct({{0, 1, 2, 3}})
    ->ArgNames({"storage"});

enum class EnsureWritableBenchmarkPath : uint8_t {
  kUniqueDirect,
  kUniqueMultisegment,
  kSharedCow,
};

bess::PacketHandle BuildReshapeBenchmarkPacket(bess::PlainPacketPool &pool,
                                               size_t bytes,
                                               bool multisegment) {
  if (!multisegment) {
    bess::PacketHandle packet = pool.Alloc(bytes);
    CHECK(packet != nullptr);
    std::memset(bess::PacketRef(packet).head_data(), 0, bytes);
    return packet;
  }

  constexpr size_t kSegmentCapacity = 1024;
  bess::PacketHandle head = nullptr;
  bess::PacketHandle previous = nullptr;
  size_t remaining = bytes;
  uint16_t segment_count = 0;
  while (remaining != 0) {
    const size_t length = std::min(remaining, kSegmentCapacity);
    bess::PacketHandle segment = pool.Alloc(length);
    CHECK(segment != nullptr);
    if (head == nullptr) {
      head = segment;
    } else {
      previous->next = segment;
    }
    previous = segment;
    std::memset(bess::PacketRef(segment).head_data(), 0, length);
    remaining -= length;
    segment_count++;
  }
  CHECK(head != nullptr);
  head->pkt_len = static_cast<uint32_t>(bytes);
  head->nb_segs = segment_count;
  return head;
}

void RunEnsureWritableBenchmark(benchmark::State &state,
                                EnsureWritableBenchmarkPath path) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t bytes = static_cast<size_t>(state.range(0));
  const bool shared = path == EnsureWritableBenchmarkPath::kSharedCow;
  const bool multisegment =
      path == EnsureWritableBenchmarkPath::kUniqueMultisegment;

  bess::PacketHandle packet = nullptr;
  bess::PacketHandle sibling = nullptr;
  if (shared) {
    std::vector<std::byte> payload(bytes);
    sibling = pool.AllocCopy(payload.data(), payload.size());
    CHECK(sibling != nullptr);
    packet = bess::PacketClone(sibling);
    CHECK(packet != nullptr);
  } else {
    packet = BuildReshapeBenchmarkPacket(pool, bytes, multisegment);
  }
  const uint16_t baseline_segments = packet->nb_segs;

  for (auto _ : state) {
    auto result = bess::packet::EnsureWritable(packet);
    CHECK(result.has_value());
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(packet);
    if (shared) {
      state.PauseTiming();
      bess::PacketFree(packet);
      packet = bess::PacketClone(sibling);
      CHECK(packet != nullptr);
      state.ResumeTiming();
    }
  }

  state.SetItemsProcessed(state.iterations());
  state.counters["bytes_copied/op"] = shared ? static_cast<double>(bytes) : 0;
  state.counters["segments/op"] = baseline_segments;
  state.counters["head_replaced/op"] = shared ? 1 : 0;
  state.counters["allocation/op"] = shared ? 1 : 0;

  if (sibling != nullptr) {
    bess::PacketFree(packet);
    bess::PacketFree(sibling);
  } else {
    bess::PacketFree(packet);
  }
}

void BM_EnsureWritableUniqueDirect(benchmark::State &state) {
  RunEnsureWritableBenchmark(state, EnsureWritableBenchmarkPath::kUniqueDirect);
}

void BM_EnsureWritableUniqueMultisegment(benchmark::State &state) {
  RunEnsureWritableBenchmark(state,
                             EnsureWritableBenchmarkPath::kUniqueMultisegment);
}

void BM_EnsureWritableSharedCow(benchmark::State &state) {
  RunEnsureWritableBenchmark(state, EnsureWritableBenchmarkPath::kSharedCow);
}

BENCHMARK(BM_EnsureWritableUniqueDirect)
    ->ArgsProduct({{64, 256, 1500}})
    ->ArgNames({"bytes"});
BENCHMARK(BM_EnsureWritableUniqueMultisegment)
    ->ArgsProduct({{256, 1500, 4096}})
    ->ArgNames({"bytes"});
BENCHMARK(BM_EnsureWritableSharedCow)
    ->ArgsProduct({{64, 256, 1500, 4096}})
    ->ArgNames({"bytes"});

enum class EnsureLinearBenchmarkPath : uint8_t {
  kAlreadyLinear,
  kNativeTwoSegment,
  kNativeFourSegment,
  kReplacementTwoSegment,
  kReplacementFourSegment,
};

bess::PacketHandle BuildFixedReshapeChain(bess::PlainPacketPool &pool,
                                          size_t segments) {
  constexpr size_t kSegmentLength = 64;
  bess::PacketHandle head = nullptr;
  bess::PacketHandle previous = nullptr;
  for (size_t i = 0; i < segments; i++) {
    bess::PacketHandle segment = pool.Alloc(kSegmentLength);
    CHECK(segment != nullptr);
    std::memset(bess::PacketRef(segment).head_data(), 0, kSegmentLength);
    if (head == nullptr) {
      head = segment;
    } else {
      previous->next = segment;
    }
    previous = segment;
  }
  CHECK(head != nullptr);
  head->pkt_len = static_cast<uint32_t>(segments * kSegmentLength);
  head->nb_segs = static_cast<uint16_t>(segments);
  return head;
}

bess::PacketHandle BuildWritableHeadSharedTailReshapePacket(
    bess::PlainPacketPool &pool, bess::PacketHandle *tail_owner_out) {
  constexpr size_t kSegmentLength = 64;
  bess::PacketHandle head = pool.Alloc(kSegmentLength);
  CHECK(head != nullptr);
  std::memset(bess::PacketRef(head).head_data(), 0, kSegmentLength);
  *tail_owner_out = pool.Alloc(kSegmentLength);
  CHECK(*tail_owner_out != nullptr);
  std::memset(bess::PacketRef(*tail_owner_out).head_data(), 0, kSegmentLength);
  bess::PacketHandle shared_tail = bess::PacketClone(*tail_owner_out);
  CHECK(shared_tail != nullptr);
  head->next = shared_tail;
  head->pkt_len = 2 * kSegmentLength;
  head->nb_segs = 2;
  return head;
}

void RunEnsureLinearBenchmark(benchmark::State &state,
                              EnsureLinearBenchmarkPath path) {
  bess::PlainPacketPool &pool = GetPool();
  const bool already_linear = path == EnsureLinearBenchmarkPath::kAlreadyLinear;
  const bool native = path == EnsureLinearBenchmarkPath::kNativeTwoSegment ||
                      path == EnsureLinearBenchmarkPath::kNativeFourSegment;
  const size_t segments =
      path == EnsureLinearBenchmarkPath::kNativeFourSegment ||
              path == EnsureLinearBenchmarkPath::kReplacementFourSegment
          ? 4
          : 2;
  const size_t bytes = already_linear ? 64 : segments * 64;

  bess::PacketHandle packet = nullptr;
  bess::PacketHandle sibling = nullptr;
  if (already_linear) {
    packet = pool.Alloc(bytes);
    CHECK(packet != nullptr);
    std::memset(bess::PacketRef(packet).head_data(), 0, bytes);
  } else if (native) {
    packet = BuildFixedReshapeChain(pool, segments);
  } else {
    sibling = BuildFixedReshapeChain(pool, segments);
    packet = bess::PacketClone(sibling);
    CHECK(packet != nullptr);
  }

  for (auto _ : state) {
    auto result = bess::packet::EnsureLinear(packet);
    CHECK(result.has_value());
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(packet);

    if (native) {
      state.PauseTiming();
      bess::PacketFree(packet);
      packet = BuildFixedReshapeChain(pool, segments);
      state.ResumeTiming();
    } else if (!already_linear) {
      state.PauseTiming();
      bess::PacketFree(packet);
      packet = bess::PacketClone(sibling);
      CHECK(packet != nullptr);
      state.ResumeTiming();
    }
  }

  state.SetItemsProcessed(state.iterations());
  state.counters["allocations"] = native || already_linear ? 0 : 1;
  state.counters["bytes_copied"] =
      already_linear
          ? 0
          : static_cast<double>(native ? (segments - 1) * 64 : segments * 64);
  state.counters["segments_freed"] =
      already_linear ? 0
                     : static_cast<double>(native ? segments - 1 : segments);
  state.counters["head_replacements"] = native || already_linear ? 0 : 1;

  bess::PacketFree(packet);
  if (sibling != nullptr) {
    bess::PacketFree(sibling);
  }
}

void BM_EnsureLinearAlreadyLinear(benchmark::State &state) {
  RunEnsureLinearBenchmark(state, EnsureLinearBenchmarkPath::kAlreadyLinear);
}

void BM_EnsureLinearNativeTwoSegment(benchmark::State &state) {
  RunEnsureLinearBenchmark(state, EnsureLinearBenchmarkPath::kNativeTwoSegment);
}

void BM_EnsureLinearNativeFourSegment(benchmark::State &state) {
  RunEnsureLinearBenchmark(state,
                           EnsureLinearBenchmarkPath::kNativeFourSegment);
}

void BM_EnsureLinearReplacementTwoSegment(benchmark::State &state) {
  RunEnsureLinearBenchmark(state,
                           EnsureLinearBenchmarkPath::kReplacementTwoSegment);
}

void BM_EnsureLinearReplacementFourSegment(benchmark::State &state) {
  RunEnsureLinearBenchmark(state,
                           EnsureLinearBenchmarkPath::kReplacementFourSegment);
}

BENCHMARK(BM_EnsureLinearAlreadyLinear);
BENCHMARK(BM_EnsureLinearNativeTwoSegment);
BENCHMARK(BM_EnsureLinearNativeFourSegment);
BENCHMARK(BM_EnsureLinearReplacementTwoSegment);
BENCHMARK(BM_EnsureLinearReplacementFourSegment);

enum class EnsureContiguousBenchmarkPath : uint8_t {
  kSameSegmentWritable,
  kSameSegmentShared,
  kCrossesTwoSegments,
  kCrossesTwoSegmentsSharedTail,
  kCrossesFourSegments,
};

void RunEnsureContiguousBenchmark(benchmark::State &state,
                                  EnsureContiguousBenchmarkPath path) {
  bess::PlainPacketPool &pool = GetPool();
  const bool shared = path == EnsureContiguousBenchmarkPath::kSameSegmentShared;
  const bool shared_tail =
      path == EnsureContiguousBenchmarkPath::kCrossesTwoSegmentsSharedTail;
  const bool same_segment =
      path == EnsureContiguousBenchmarkPath::kSameSegmentWritable || shared;
  const size_t segments =
      path == EnsureContiguousBenchmarkPath::kCrossesFourSegments ? 4 : 2;
  const size_t total_bytes = same_segment ? 128 : segments * 64;
  const size_t offset = 16;
  const size_t bytes = same_segment ? 32 : total_bytes - 32;

  bess::PacketHandle packet = nullptr;
  bess::PacketHandle sibling = nullptr;
  if (shared) {
    sibling = pool.Alloc(total_bytes);
    CHECK(sibling != nullptr);
    std::memset(bess::PacketRef(sibling).head_data(), 0, total_bytes);
    packet = bess::PacketClone(sibling);
    CHECK(packet != nullptr);
  } else if (shared_tail) {
    packet = BuildWritableHeadSharedTailReshapePacket(pool, &sibling);
  } else if (same_segment) {
    packet = pool.Alloc(total_bytes);
    CHECK(packet != nullptr);
    std::memset(bess::PacketRef(packet).head_data(), 0, total_bytes);
  } else {
    packet = BuildFixedReshapeChain(pool, segments);
  }

  for (auto _ : state) {
    auto result = bess::packet::EnsureContiguous(packet, offset, bytes);
    CHECK(result.has_value());
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(packet);

    if (shared || shared_tail) {
      state.PauseTiming();
      bess::PacketFree(packet);
      if (shared) {
        packet = bess::PacketClone(sibling);
        CHECK(packet != nullptr);
      } else {
        bess::PacketFree(sibling);
        packet = BuildWritableHeadSharedTailReshapePacket(pool, &sibling);
      }
      state.ResumeTiming();
    } else if (!same_segment) {
      state.PauseTiming();
      bess::PacketFree(packet);
      packet = BuildFixedReshapeChain(pool, segments);
      state.ResumeTiming();
    }
  }

  state.SetItemsProcessed(state.iterations());
  state.counters["allocations"] = shared ? 1 : 0;
  state.counters["bytes_copied"] = shared_tail ? 64
                                   : shared ? static_cast<double>(total_bytes)
                                   : same_segment
                                       ? 0
                                       : static_cast<double>(total_bytes - 64);
  state.counters["segments_freed"] = shared || shared_tail ? 1
                                     : same_segment
                                         ? 0
                                         : static_cast<double>(segments - 1);
  state.counters["head_replacements"] = shared ? 1 : 0;

  bess::PacketFree(packet);
  if (sibling != nullptr) {
    bess::PacketFree(sibling);
  }
}

void BM_EnsureContiguousSameSegmentWritable(benchmark::State &state) {
  RunEnsureContiguousBenchmark(
      state, EnsureContiguousBenchmarkPath::kSameSegmentWritable);
}

void BM_EnsureContiguousSameSegmentShared(benchmark::State &state) {
  RunEnsureContiguousBenchmark(
      state, EnsureContiguousBenchmarkPath::kSameSegmentShared);
}

void BM_EnsureContiguousCrossesTwoSegments(benchmark::State &state) {
  RunEnsureContiguousBenchmark(
      state, EnsureContiguousBenchmarkPath::kCrossesTwoSegments);
}

void BM_EnsureContiguousCrossesFourSegments(benchmark::State &state) {
  RunEnsureContiguousBenchmark(
      state, EnsureContiguousBenchmarkPath::kCrossesFourSegments);
}

void BM_EnsureContiguousCrossesTwoSegmentsSharedTail(benchmark::State &state) {
  RunEnsureContiguousBenchmark(
      state, EnsureContiguousBenchmarkPath::kCrossesTwoSegmentsSharedTail);
}

BENCHMARK(BM_EnsureContiguousSameSegmentWritable);
BENCHMARK(BM_EnsureContiguousSameSegmentShared);
BENCHMARK(BM_EnsureContiguousCrossesTwoSegments);
BENCHMARK(BM_EnsureContiguousCrossesFourSegments);
BENCHMARK(BM_EnsureContiguousCrossesTwoSegmentsSharedTail);

enum class TopologyRemovalBenchmarkPath : uint8_t {
  kPrefixWithinHead,
  kPrefixAcrossTwoSegments,
  kPrefixAcrossFourSegments,
  kSuffixWithinTail,
  kSuffixAcrossTwoSegments,
  kSuffixAcrossFourSegments,
};

void RunTopologyRemovalBenchmark(benchmark::State &state,
                                 TopologyRemovalBenchmarkPath path) {
  bess::PlainPacketPool &pool = GetPool();
  bool remove_prefix = false;
  size_t segments = 0;
  size_t bytes_removed = 0;
  size_t segments_freed = 0;
  size_t head_replacements = 0;
  switch (path) {
    case TopologyRemovalBenchmarkPath::kPrefixWithinHead:
      remove_prefix = true;
      segments = 2;
      bytes_removed = 16;
      break;
    case TopologyRemovalBenchmarkPath::kPrefixAcrossTwoSegments:
      remove_prefix = true;
      segments = 2;
      bytes_removed = 80;
      segments_freed = 1;
      head_replacements = 1;
      break;
    case TopologyRemovalBenchmarkPath::kPrefixAcrossFourSegments:
      remove_prefix = true;
      segments = 4;
      bytes_removed = 200;
      segments_freed = 3;
      head_replacements = 1;
      break;
    case TopologyRemovalBenchmarkPath::kSuffixWithinTail:
      segments = 2;
      bytes_removed = 16;
      break;
    case TopologyRemovalBenchmarkPath::kSuffixAcrossTwoSegments:
      segments = 2;
      bytes_removed = 80;
      segments_freed = 1;
      break;
    case TopologyRemovalBenchmarkPath::kSuffixAcrossFourSegments:
      segments = 4;
      bytes_removed = 200;
      segments_freed = 3;
      break;
  }

  bess::PacketHandle packet = BuildFixedReshapeChain(pool, segments);
  for (auto _ : state) {
    const auto result = remove_prefix
                            ? bess::packet::RemovePrefix(packet, bytes_removed)
                            : bess::packet::TrimSuffix(packet, bytes_removed);
    CHECK(result.has_value());
    CHECK_EQ(packet->pkt_len, segments * 64 - bytes_removed);
    CHECK_EQ(packet->nb_segs, segments - segments_freed);
    benchmark::DoNotOptimize(packet);

    state.PauseTiming();
    bess::PacketFree(packet);
    packet = BuildFixedReshapeChain(pool, segments);
    state.ResumeTiming();
  }

  state.SetItemsProcessed(state.iterations());
  state.counters["allocations"] = 0;
  state.counters["bytes_copied"] = 0;
  state.counters["segments_freed"] = segments_freed;
  state.counters["head_replacements"] = head_replacements;
  state.counters["bytes_removed"] = bytes_removed;
  bess::PacketFree(packet);
}

void BM_RemovePrefixWithinHead(benchmark::State &state) {
  RunTopologyRemovalBenchmark(state,
                              TopologyRemovalBenchmarkPath::kPrefixWithinHead);
}

void BM_RemovePrefixAcrossTwoSegments(benchmark::State &state) {
  RunTopologyRemovalBenchmark(
      state, TopologyRemovalBenchmarkPath::kPrefixAcrossTwoSegments);
}

void BM_RemovePrefixAcrossFourSegments(benchmark::State &state) {
  RunTopologyRemovalBenchmark(
      state, TopologyRemovalBenchmarkPath::kPrefixAcrossFourSegments);
}

void BM_TrimSuffixWithinTail(benchmark::State &state) {
  RunTopologyRemovalBenchmark(state,
                              TopologyRemovalBenchmarkPath::kSuffixWithinTail);
}

void BM_TrimSuffixAcrossTwoSegments(benchmark::State &state) {
  RunTopologyRemovalBenchmark(
      state, TopologyRemovalBenchmarkPath::kSuffixAcrossTwoSegments);
}

void BM_TrimSuffixAcrossFourSegments(benchmark::State &state) {
  RunTopologyRemovalBenchmark(
      state, TopologyRemovalBenchmarkPath::kSuffixAcrossFourSegments);
}

BENCHMARK(BM_RemovePrefixWithinHead);
BENCHMARK(BM_RemovePrefixAcrossTwoSegments);
BENCHMARK(BM_RemovePrefixAcrossFourSegments);
BENCHMARK(BM_TrimSuffixWithinTail);
BENCHMARK(BM_TrimSuffixAcrossTwoSegments);
BENCHMARK(BM_TrimSuffixAcrossFourSegments);

void BM_PacketCursorChainRead(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t width = static_cast<size_t>(state.range(0));
  const size_t shape = static_cast<size_t>(state.range(1));
  const size_t batch = static_cast<size_t>(state.range(2));
  const size_t offset = shape == 2 ? 64 : 14;
  constexpr size_t kMaxBatch = 32;
  std::array<bess::PacketHandle, kMaxBatch> pkts{};

  for (size_t i = 0; i < batch; i++) {
    pkts[i] = BuildCursorBenchmarkChain(pool, shape);
  }

  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      bess::packet::PacketCursor cursor{bess::PacketRef(pkts[i])};
      benchmark::DoNotOptimize(cursor.Skip(offset));
      ReadCursorWidth(cursor, width);
    }
  }
  state.SetItemsProcessed(state.iterations() * batch);
  state.SetBytesProcessed(state.iterations() * batch * width);
  size_t transitions = 0;
  if (shape == 2) {
    transitions = 1;
  } else if (shape == 3) {
    transitions = (offset + width - 1) / 16 - offset / 16;
  }
  state.counters["bytes_copied/read"] = static_cast<double>(width);
  state.counters["segment_transitions/read"] = static_cast<double>(transitions);

  bess::PacketFreeBulk(pkts.data(), batch);
}

BENCHMARK(BM_PacketCursorChainRead)
    ->ArgsProduct({{1, 4, 8, 16, 32}, {0, 1, 2, 3}, {1, 8, 32}})
    ->ArgNames({"width", "shape", "batch"});

template <bool UseCursor>
void BM_PacketRead(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t width = static_cast<size_t>(state.range(0));
  const size_t offset = static_cast<size_t>(state.range(1));
  const size_t batch = static_cast<size_t>(state.range(2));
  constexpr size_t kMaxBatch = 32;
  std::array<bess::PacketHandle, kMaxBatch> pkts{};
  CHECK(pool.AllocBulk(pkts.data(), batch, 128));
  std::array<std::byte, 32> out{};

  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      bess::PacketRef packet(pkts[i]);
      if constexpr (UseCursor) {
        bess::packet::PacketCursor cursor{packet};
        benchmark::DoNotOptimize(cursor.Skip(offset));
        ReadCursorWidth(cursor, width);
      } else {
        std::memcpy(out.data(), packet.head_data<const std::byte *>() + offset,
                    width);
      }
      benchmark::DoNotOptimize(out);
    }
  }
  state.SetItemsProcessed(state.iterations() * batch);
  state.SetBytesProcessed(state.iterations() * batch * width);
  state.counters["bytes_copied/read"] = static_cast<double>(width);
  state.counters["segment_transitions/read"] = 0;

  bess::PacketFreeBulk(pkts.data(), batch);
}

void BM_PacketDirectRead(benchmark::State &state) {
  BM_PacketRead<false>(state);
}

void BM_PacketCursorRead(benchmark::State &state) {
  BM_PacketRead<true>(state);
}

BENCHMARK(BM_PacketDirectRead)
    ->ArgsProduct({{1, 2, 4, 8, 16, 32}, {0, 14, 34, 64}, {1, 8, 32}})
    ->ArgNames({"width", "offset", "batch"});
BENCHMARK(BM_PacketCursorRead)
    ->ArgsProduct({{1, 2, 4, 8, 16, 32}, {0, 14, 34, 64}, {1, 8, 32}})
    ->ArgNames({"width", "offset", "batch"});

// K4.5 compares module-like batch bodies, excluding scheduler/virtual dispatch.
// Runtime readers consume offsets; specialized readers bake offsets and use a
// contiguous-head fast path with PacketCursor fallback. ILP4 interleaves reads
// from four packets.
struct BatchFieldPlan {
  size_t first_offset;
  size_t second_offset;
};

uint32_t ReadBatchFieldRuntime(bess::PacketRef packet, size_t offset) {
  bess::packet::PacketCursor cursor{packet};
  CHECK(cursor.Skip(offset));
  const auto value = cursor.Read<uint32_t>();
  CHECK(value.has_value());
  return *value;
}

inline uint32_t ReadBatchFieldRuntimeInline(bess::PacketRef packet,
                                            size_t offset) {
  bess::packet::PacketCursor cursor{packet};
  CHECK(cursor.Skip(offset));
  const auto value = cursor.Read<uint32_t>();
  CHECK(value.has_value());
  return *value;
}

template <size_t Offset>
uint32_t ReadBatchFieldSpecialized(bess::PacketRef packet) {
  if (packet.data_len() >= Offset + sizeof(uint32_t)) {
    uint32_t value;
    std::memcpy(
        &value,
        packet.head_data<const std::byte *>(static_cast<uint16_t>(Offset)),
        sizeof(value));
    return value;
  }
  return ReadBatchFieldRuntime(packet, Offset);
}

uint64_t CombineBatchFields(uint32_t first, uint32_t second) {
  return (static_cast<uint64_t>(first) << 32) | second;
}

void ProcessRuntimeGenericScalar(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan plan,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  for (size_t i = 0; i < count; i++) {
    const bess::PacketRef packet = batch.packet(i);
    results[i] =
        CombineBatchFields(ReadBatchFieldRuntime(packet, plan.first_offset),
                           ReadBatchFieldRuntime(packet, plan.second_offset));
  }
}

void ProcessRuntimeGenericInlineScalar(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan plan,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  for (size_t i = 0; i < count; i++) {
    const bess::PacketRef packet = batch.packet(i);
    results[i] = CombineBatchFields(
        ReadBatchFieldRuntimeInline(packet, plan.first_offset),
        ReadBatchFieldRuntimeInline(packet, plan.second_offset));
  }
}

void ProcessRuntimeGenericUnrolled4(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan plan,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    const bess::PacketRef p0 = batch.packet(i);
    const bess::PacketRef p1 = batch.packet(i + 1);
    const bess::PacketRef p2 = batch.packet(i + 2);
    const bess::PacketRef p3 = batch.packet(i + 3);
    results[i] = CombineBatchFields(
        ReadBatchFieldRuntimeInline(p0, plan.first_offset),
        ReadBatchFieldRuntimeInline(p0, plan.second_offset));
    results[i + 1] = CombineBatchFields(
        ReadBatchFieldRuntimeInline(p1, plan.first_offset),
        ReadBatchFieldRuntimeInline(p1, plan.second_offset));
    results[i + 2] = CombineBatchFields(
        ReadBatchFieldRuntimeInline(p2, plan.first_offset),
        ReadBatchFieldRuntimeInline(p2, plan.second_offset));
    results[i + 3] = CombineBatchFields(
        ReadBatchFieldRuntimeInline(p3, plan.first_offset),
        ReadBatchFieldRuntimeInline(p3, plan.second_offset));
  }
  for (; i < count; i++) {
    const bess::PacketRef packet = batch.packet(i);
    results[i] = CombineBatchFields(
        ReadBatchFieldRuntimeInline(packet, plan.first_offset),
        ReadBatchFieldRuntimeInline(packet, plan.second_offset));
  }
}

void ProcessRuntimeGenericIlp4(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan plan,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    std::array<uint32_t, 4> first;
    std::array<uint32_t, 4> second;
    for (size_t lane = 0; lane < 4; lane++) {
      first[lane] =
          ReadBatchFieldRuntime(batch.packet(i + lane), plan.first_offset);
    }
    for (size_t lane = 0; lane < 4; lane++) {
      second[lane] =
          ReadBatchFieldRuntime(batch.packet(i + lane), plan.second_offset);
    }
    for (size_t lane = 0; lane < 4; lane++) {
      results[i + lane] = CombineBatchFields(first[lane], second[lane]);
    }
  }
  for (; i < count; i++) {
    const bess::PacketRef packet = batch.packet(i);
    results[i] =
        CombineBatchFields(ReadBatchFieldRuntime(packet, plan.first_offset),
                           ReadBatchFieldRuntime(packet, plan.second_offset));
  }
}

template <size_t FirstOffset, size_t SecondOffset>
void ProcessCompileTimeSpecializedScalar(
    const bess::PacketBatch &batch, size_t count,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  for (size_t i = 0; i < count; i++) {
    const bess::PacketRef packet = batch.packet(i);
    results[i] =
        CombineBatchFields(ReadBatchFieldSpecialized<FirstOffset>(packet),
                           ReadBatchFieldSpecialized<SecondOffset>(packet));
  }
}

template <size_t FirstOffset, size_t SecondOffset>
void ProcessCompileTimeSpecializedIlp4(
    const bess::PacketBatch &batch, size_t count,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    std::array<uint32_t, 4> first;
    std::array<uint32_t, 4> second;
    for (size_t lane = 0; lane < 4; lane++) {
      first[lane] =
          ReadBatchFieldSpecialized<FirstOffset>(batch.packet(i + lane));
    }
    for (size_t lane = 0; lane < 4; lane++) {
      second[lane] =
          ReadBatchFieldSpecialized<SecondOffset>(batch.packet(i + lane));
    }
    for (size_t lane = 0; lane < 4; lane++) {
      results[i + lane] = CombineBatchFields(first[lane], second[lane]);
    }
  }
  for (; i < count; i++) {
    const bess::PacketRef packet = batch.packet(i);
    results[i] =
        CombineBatchFields(ReadBatchFieldSpecialized<FirstOffset>(packet),
                           ReadBatchFieldSpecialized<SecondOffset>(packet));
  }
}

struct PipelineTable {
  static constexpr size_t kMaxEntries = 1u << 16;
  std::vector<uint64_t> keys;
  std::vector<uint64_t> actions;
  size_t mask;

  explicit PipelineTable(size_t entries)
      : keys(entries, UINT64_MAX), actions(entries, 0), mask(entries - 1) {
    CHECK(entries != 0 && (entries & mask) == 0);
  }
};

uint64_t HashPipelineKey(uint64_t key) {
  key ^= key >> 30;
  key *= 0xbf58476d1ce4e5b9ULL;
  key ^= key >> 27;
  key *= 0x94d049bb133111ebULL;
  return key ^ (key >> 31);
}

uint64_t MakePipelineKey(uint32_t first, uint32_t second) {
  return HashPipelineKey(CombineBatchFields(first, second));
}

struct PipelineKeyInput {
  uint32_t first;
  uint32_t second;
  uint64_t key;
};

PipelineKeyInput MakePipelineInput(uint64_t seed, bool miss) {
  const uint32_t first_base = miss ? 0xd0000000u : 0x10000000u;
  const uint32_t second_base = miss ? 0x50000000u : 0x20000000u;
  const uint32_t first_mix = static_cast<uint32_t>(seed * 0x9e3779b9u);
  const uint32_t second_mix = static_cast<uint32_t>(
      (seed * 0xbf58476d1ce4e5b9ULL) >> 17);
  PipelineKeyInput input{first_base | (first_mix & 0x0fffffffu),
                          second_base | (second_mix & 0x0fffffffu), 0};
  input.key = MakePipelineKey(input.first, input.second);
  if (input.key == UINT64_MAX) {
    input.second ^= 1;
    input.key = MakePipelineKey(input.first, input.second);
  }
  return input;
}

void WritePipelineBytes(bess::PacketHandle packet, size_t offset,
                        const void *source, size_t length) {
  const auto *src = static_cast<const std::byte *>(source);
  for (bess::PacketHandle segment = packet; segment != nullptr;
       segment = segment->next) {
    const size_t segment_length = bess::PacketRef(segment).data_len();
    if (offset >= segment_length) {
      offset -= segment_length;
      continue;
    }
    const size_t copied = std::min(length, segment_length - offset);
    std::memcpy(bess::PacketRef(segment).head_data<std::byte *>() + offset,
                src, copied);
    src += copied;
    length -= copied;
    offset = 0;
    if (length == 0) {
      return;
    }
  }
  CHECK_EQ(length, 0u);
}

void FillCursorBenchmarkChain(bess::PacketHandle packet, size_t packet_seed);

void FillPipelinePacket(bess::PacketHandle packet, size_t packet_seed,
                        BatchFieldPlan plan, const PipelineKeyInput &input) {
  FillCursorBenchmarkChain(packet, packet_seed);
  WritePipelineBytes(packet, plan.first_offset, &input.first,
                     sizeof(input.first));
  WritePipelineBytes(packet, plan.second_offset, &input.second,
                     sizeof(input.second));
}

void InsertPipelineEntry(PipelineTable &table, uint64_t key, uint64_t action) {
  size_t index = HashPipelineKey(key) & table.mask;
  for (size_t probes = 0; probes <= table.mask; probes++) {
    if (table.keys[index] == UINT64_MAX || table.keys[index] == key) {
      table.keys[index] = key;
      table.actions[index] = action;
      return;
    }
    index = (index + 1) & table.mask;
  }
  CHECK(false) << "pipeline table is full";
}

uint64_t LookupPipelineEntry(const PipelineTable &table, uint64_t key) {
  size_t index = HashPipelineKey(key) & table.mask;
  for (size_t probes = 0; probes <= table.mask; probes++) {
    const uint64_t stored = table.keys[index];
    if (stored == key) {
      return table.actions[index];
    }
    if (stored == UINT64_MAX) {
      break;
    }
    index = (index + 1) & table.mask;
  }
  return 0;
}

std::vector<PipelineKeyInput> PopulatePipelineTable(PipelineTable &table) {
  constexpr uint64_t kActionSalt = 0x9e3779b97f4a7c15ULL;
  const size_t population = table.keys.size() / 2;
  std::vector<PipelineKeyInput> hits;
  hits.reserve(population);
  for (size_t i = 0; i < population; i++) {
    PipelineKeyInput input = MakePipelineInput(i, false);
    uint64_t action = input.key ^ kActionSalt;
    if (action == 0) {
      action = 1;
    }
    InsertPipelineEntry(table, input.key, action);
    hits.push_back(input);
  }
  return hits;
}

PipelineKeyInput SelectPipelineInput(
    size_t lookup_mode, size_t corpus_index, size_t packet_index,
    size_t batch_size, const std::vector<PipelineKeyInput> &hits) {
  CHECK_LT(lookup_mode, 4u);
  CHECK(!hits.empty());
  const size_t sequence = corpus_index * batch_size + packet_index;
  const size_t hot_count = std::min<size_t>(32, hits.size());
  const bool use_hit = lookup_mode == 0 || lookup_mode == 1 ||
                       (lookup_mode == 3 && (sequence & 1) == 0);
  if (use_hit) {
    const size_t index = lookup_mode == 0
                             ? sequence % hot_count
                             : (sequence * 2654435761ULL) % hits.size();
    return hits[index];
  }
  return MakePipelineInput(0x100000000ULL + sequence, true);
}

uint64_t ReadAndMakePipelineKey(bess::PacketRef packet, BatchFieldPlan plan) {
  return MakePipelineKey(ReadBatchFieldRuntimeInline(packet, plan.first_offset),
                         ReadBatchFieldRuntimeInline(packet, plan.second_offset));
}

void ProcessPipelineRuntimeScalar(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan plan,
    const PipelineTable &table,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  for (size_t i = 0; i < count; i++) {
    results[i] = LookupPipelineEntry(
        table, ReadAndMakePipelineKey(batch.packet(i), plan));
  }
}

void ProcessPipelineRuntimeUnrolled4(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan plan,
    const PipelineTable &table,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    const bess::PacketRef p0 = batch.packet(i);
    const bess::PacketRef p1 = batch.packet(i + 1);
    const bess::PacketRef p2 = batch.packet(i + 2);
    const bess::PacketRef p3 = batch.packet(i + 3);
    results[i] = LookupPipelineEntry(table, ReadAndMakePipelineKey(p0, plan));
    results[i + 1] =
        LookupPipelineEntry(table, ReadAndMakePipelineKey(p1, plan));
    results[i + 2] =
        LookupPipelineEntry(table, ReadAndMakePipelineKey(p2, plan));
    results[i + 3] =
        LookupPipelineEntry(table, ReadAndMakePipelineKey(p3, plan));
  }
  for (; i < count; i++) {
    results[i] = LookupPipelineEntry(
        table, ReadAndMakePipelineKey(batch.packet(i), plan));
  }
}

void ProcessPipelineRuntimePrefetch4(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan plan,
    const PipelineTable &table,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    if (i + 4 < count) {
      __builtin_prefetch(batch.packet(i + 4).head_data<const void *>(), 0, 1);
      __builtin_prefetch(batch.packet(i + 5).head_data<const void *>(), 0, 1);
      __builtin_prefetch(batch.packet(i + 6).head_data<const void *>(), 0, 1);
      __builtin_prefetch(batch.packet(i + 7).head_data<const void *>(), 0, 1);
    }
    const bess::PacketRef p0 = batch.packet(i);
    const bess::PacketRef p1 = batch.packet(i + 1);
    const bess::PacketRef p2 = batch.packet(i + 2);
    const bess::PacketRef p3 = batch.packet(i + 3);
    results[i] = LookupPipelineEntry(table, ReadAndMakePipelineKey(p0, plan));
    results[i + 1] =
        LookupPipelineEntry(table, ReadAndMakePipelineKey(p1, plan));
    results[i + 2] =
        LookupPipelineEntry(table, ReadAndMakePipelineKey(p2, plan));
    results[i + 3] =
        LookupPipelineEntry(table, ReadAndMakePipelineKey(p3, plan));
  }
  for (; i < count; i++) {
    results[i] = LookupPipelineEntry(
        table, ReadAndMakePipelineKey(batch.packet(i), plan));
  }
}

template <size_t FirstOffset, size_t SecondOffset>
void ProcessPipelineTypedScalar(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan,
    const PipelineTable &table,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  for (size_t i = 0; i < count; i++) {
    const uint64_t key = MakePipelineKey(
        ReadBatchFieldSpecialized<FirstOffset>(batch.packet(i)),
        ReadBatchFieldSpecialized<SecondOffset>(batch.packet(i)));
    results[i] = LookupPipelineEntry(table, key);
  }
}

template <size_t FirstOffset, size_t SecondOffset>
void ProcessPipelineTypedUnrolled4(
    const bess::PacketBatch &batch, size_t count, BatchFieldPlan,
    const PipelineTable &table,
    std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    for (size_t lane = 0; lane < 4; lane++) {
      const uint64_t key = MakePipelineKey(
          ReadBatchFieldSpecialized<FirstOffset>(batch.packet(i + lane)),
          ReadBatchFieldSpecialized<SecondOffset>(batch.packet(i + lane)));
      results[i + lane] = LookupPipelineEntry(table, key);
    }
  }
  for (; i < count; i++) {
    const uint64_t key = MakePipelineKey(
        ReadBatchFieldSpecialized<FirstOffset>(batch.packet(i)),
        ReadBatchFieldSpecialized<SecondOffset>(batch.packet(i)));
    results[i] = LookupPipelineEntry(table, key);
  }
}

void FillCursorBenchmarkChain(bess::PacketHandle packet, size_t packet_seed = 0) {
  size_t packet_offset = 0;
  for (bess::PacketHandle segment = packet; segment != nullptr;
       segment = segment->next) {
    bess::PacketRef segment_ref(segment);
    auto *data = segment_ref.head_data<std::byte *>();
    for (size_t i = 0; i < segment_ref.data_len(); i++) {
      data[i] = static_cast<std::byte>((packet_offset + i + packet_seed * 13) *
                                       37 + 19);
    }
    packet_offset += segment_ref.data_len();
  }
}

template <typename Process>
void RunPacketBatchExecutionBenchmark(benchmark::State &state,
                                      BatchFieldPlan plan, Process process,
                                      size_t ilp_lanes) {
  constexpr size_t kMaxBatch = bess::PacketBatch::kMaxBurst;
  const size_t count = static_cast<size_t>(state.range(0));
  const size_t shape = static_cast<size_t>(state.range(1));
  CHECK_LE(count, kMaxBatch);

  bess::PlainPacketPool &pool = GetPool();
  std::array<bess::PacketHandle, kMaxBatch> packets{};
  bess::PacketBatch batch;
  batch.clear();
  for (size_t i = 0; i < count; i++) {
    packets[i] = BuildCursorBenchmarkChain(pool, shape);
    FillCursorBenchmarkChain(packets[i], i);
    batch.add(packets[i]);
  }

  std::array<uint64_t, kMaxBatch> expected{};
  std::array<uint64_t, kMaxBatch> results{};
  ProcessRuntimeGenericScalar(batch, count, plan, expected);
  process(batch, count, plan, results);
  CHECK(
      std::equal(expected.begin(), expected.begin() + count, results.begin()));

  for (auto _ : state) {
    process(batch, count, plan, results);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * count);
  state.counters["fields/packet"] = 2;
  state.counters["packet_bytes/read"] = 8;
  state.counters["ilp_lanes"] = static_cast<double>(ilp_lanes);
  for (size_t i = 0; i < count; i++) {
    bess::PacketFree(packets[i]);
  }
}

template <bool Ilp4>
void RunRuntimeGenericBatchBenchmark(benchmark::State &state) {
  constexpr std::array<BatchFieldPlan, 2> kPlans = {BatchFieldPlan{14, 62},
                                                    BatchFieldPlan{30, 94}};
  const size_t plan_index = static_cast<size_t>(state.range(2));
  CHECK_LT(plan_index, kPlans.size());
  RunPacketBatchExecutionBenchmark(
      state, kPlans[plan_index],
      [](const bess::PacketBatch &batch, size_t count, BatchFieldPlan plan,
         std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
        if constexpr (Ilp4) {
          ProcessRuntimeGenericIlp4(batch, count, plan, results);
        } else {
          ProcessRuntimeGenericScalar(batch, count, plan, results);
        }
      },
      Ilp4 ? 4 : 1);
}

template <size_t FirstOffset, size_t SecondOffset, bool Ilp4>
void RunCompileTimeSpecializedBatchBenchmark(benchmark::State &state) {
  RunPacketBatchExecutionBenchmark(
      state, BatchFieldPlan{FirstOffset, SecondOffset},
      [](const bess::PacketBatch &batch, size_t count, BatchFieldPlan,
         std::array<uint64_t, bess::PacketBatch::kMaxBurst> &results) {
        if constexpr (Ilp4) {
          ProcessCompileTimeSpecializedIlp4<FirstOffset, SecondOffset>(
              batch, count, results);
        } else {
          ProcessCompileTimeSpecializedScalar<FirstOffset, SecondOffset>(
              batch, count, results);
        }
      },
      Ilp4 ? 4 : 1);
}

template <typename Process>
void RunPacketPipelineBenchmark(benchmark::State &state,
                                BatchFieldPlan plan, Process process,
                                size_t ilp_lanes) {
  constexpr size_t kMaxBatch = bess::PacketBatch::kMaxBurst;
  const size_t count = static_cast<size_t>(state.range(0));
  const size_t shape = static_cast<size_t>(state.range(1));
  const size_t lookup_mode = static_cast<size_t>(state.range(3));
  const size_t table_entries = size_t{1} << state.range(4);
  CHECK_LE(count, kMaxBatch);
  CHECK_GE(table_entries, 256u);
  CHECK_LE(table_entries, PipelineTable::kMaxEntries);

  bess::PlainPacketPool &pool = GetPool();
  std::array<bess::PacketHandle, kMaxBatch> packets{};
  bess::PacketBatch batch;
  batch.clear();
  PipelineTable table(table_entries);
  const std::vector<PipelineKeyInput> hits = PopulatePipelineTable(table);
  std::array<uint64_t, kMaxBatch> expected{};
  std::array<uint64_t, kMaxBatch> results{};
  size_t expected_hits = 0;
  for (size_t i = 0; i < count; i++) {
    const PipelineKeyInput input =
        SelectPipelineInput(lookup_mode, 0, i, count, hits);
    packets[i] = BuildCursorBenchmarkChain(pool, shape);
    FillPipelinePacket(packets[i], i, plan, input);
    batch.add(packets[i]);
    expected[i] = LookupPipelineEntry(table, input.key);
    expected_hits += expected[i] != 0;
  }
  process(batch, count, plan, table, results);
  CHECK(std::equal(expected.begin(), expected.begin() + count, results.begin()));

  for (auto _ : state) {
    process(batch, count, plan, table, results);
    benchmark::DoNotOptimize(results);
  }
  state.SetItemsProcessed(state.iterations() * count);
  state.counters["fields/packet"] = 2;
  state.counters["lookup_hits/packet"] =
      static_cast<double>(expected_hits) / count;
  state.counters["lookup_mode"] = static_cast<double>(lookup_mode);
  state.counters["table_entries"] = static_cast<double>(table_entries);
  state.counters["table_population"] = static_cast<double>(hits.size());
  state.counters["table_load_factor"] =
      static_cast<double>(hits.size()) / table_entries;
  state.counters["ilp_lanes"] = static_cast<double>(ilp_lanes);
  for (size_t i = 0; i < count; i++) {
    bess::PacketFree(packets[i]);
  }
}

template <typename Process>
void RunPacketPipelineWorkingSetBenchmark(benchmark::State &state,
                                           BatchFieldPlan plan, Process process,
                                           size_t ilp_lanes) {
  constexpr size_t kMaxBatch = bess::PacketBatch::kMaxBurst;
  constexpr size_t kCorpusBatches = 32;
  const size_t count = static_cast<size_t>(state.range(0));
  const size_t shape = static_cast<size_t>(state.range(1));
  const size_t lookup_mode = static_cast<size_t>(state.range(3));
  const size_t table_entries = size_t{1} << state.range(4);
  CHECK_LE(count, kMaxBatch);
  CHECK_GE(table_entries, 256u);
  CHECK_LE(table_entries, PipelineTable::kMaxEntries);

  bess::PlainPacketPool &pool = GetPool();
  PipelineTable table(table_entries);
  const std::vector<PipelineKeyInput> hits = PopulatePipelineTable(table);
  std::vector<std::array<bess::PacketHandle, kMaxBatch>> packets(
      kCorpusBatches);
  std::vector<bess::PacketBatch> batches(kCorpusBatches);
  std::vector<std::array<uint64_t, kMaxBatch>> expected(kCorpusBatches);
  std::array<uint64_t, kMaxBatch> results{};
  size_t total_hits = 0;

  for (size_t corpus = 0; corpus < kCorpusBatches; corpus++) {
    batches[corpus].clear();
    for (size_t i = 0; i < count; i++) {
      const PipelineKeyInput input =
          SelectPipelineInput(lookup_mode, corpus, i, count, hits);
      packets[corpus][i] = BuildCursorBenchmarkChain(pool, shape);
      FillPipelinePacket(packets[corpus][i], corpus * count + i, plan, input);
      batches[corpus].add(packets[corpus][i]);
      expected[corpus][i] = LookupPipelineEntry(table, input.key);
      total_hits += expected[corpus][i] != 0;
    }
    process(batches[corpus], count, plan, table, results);
    CHECK(std::equal(expected[corpus].begin(), expected[corpus].begin() + count,
                     results.begin()));
  }

  size_t corpus = 0;
  for (auto _ : state) {
    process(batches[corpus], count, plan, table, results);
    benchmark::DoNotOptimize(results);
    corpus = (corpus + 1) & (kCorpusBatches - 1);
  }
  state.SetItemsProcessed(state.iterations() * count);
  state.counters["fields/packet"] = 2;
  state.counters["lookup_hits/packet"] =
      static_cast<double>(total_hits) / (kCorpusBatches * count);
  state.counters["lookup_mode"] = static_cast<double>(lookup_mode);
  state.counters["table_entries"] = static_cast<double>(table_entries);
  state.counters["table_population"] = static_cast<double>(hits.size());
  state.counters["table_load_factor"] =
      static_cast<double>(hits.size()) / table_entries;
  state.counters["working_set_packets"] =
      static_cast<double>(kCorpusBatches * count);
  state.counters["ilp_lanes"] = static_cast<double>(ilp_lanes);
  for (size_t corpus_index = 0; corpus_index < kCorpusBatches; corpus_index++) {
    for (size_t i = 0; i < count; i++) {
      bess::PacketFree(packets[corpus_index][i]);
    }
  }
}

void BM_PacketBatchRuntimePipelineScalar(benchmark::State &state) {
  constexpr std::array<BatchFieldPlan, 2> kPlans = {BatchFieldPlan{14, 62},
                                                    BatchFieldPlan{30, 94}};
  const size_t plan_index = static_cast<size_t>(state.range(2));
  CHECK_LT(plan_index, kPlans.size());
  RunPacketPipelineBenchmark(state, kPlans[plan_index],
                             ProcessPipelineRuntimeScalar, 1);
}

void BM_PacketBatchRuntimePipelineUnrolled4(benchmark::State &state) {
  constexpr std::array<BatchFieldPlan, 2> kPlans = {BatchFieldPlan{14, 62},
                                                    BatchFieldPlan{30, 94}};
  const size_t plan_index = static_cast<size_t>(state.range(2));
  CHECK_LT(plan_index, kPlans.size());
  RunPacketPipelineBenchmark(state, kPlans[plan_index],
                             ProcessPipelineRuntimeUnrolled4, 4);
}

void BM_PacketBatchRuntimePipelinePrefetch4(benchmark::State &state) {
  constexpr std::array<BatchFieldPlan, 2> kPlans = {BatchFieldPlan{14, 62},
                                                    BatchFieldPlan{30, 94}};
  const size_t plan_index = static_cast<size_t>(state.range(2));
  CHECK_LT(plan_index, kPlans.size());
  RunPacketPipelineBenchmark(state, kPlans[plan_index],
                             ProcessPipelineRuntimePrefetch4, 4);
}

void BM_PacketBatchTypedPipelineScalar(benchmark::State &state) {
  const BatchFieldPlan plan = state.range(2) == 0
                                  ? BatchFieldPlan{14, 62}
                                  : BatchFieldPlan{30, 94};
  if (state.range(2) == 0) {
    RunPacketPipelineBenchmark(
        state, plan,
        ProcessPipelineTypedScalar<14, 62>, 1);
  } else {
    RunPacketPipelineBenchmark(
        state, plan,
        ProcessPipelineTypedScalar<30, 94>, 1);
  }
}

void BM_PacketBatchTypedPipelineUnrolled4(benchmark::State &state) {
  const BatchFieldPlan plan = state.range(2) == 0
                                  ? BatchFieldPlan{14, 62}
                                  : BatchFieldPlan{30, 94};
  if (state.range(2) == 0) {
    RunPacketPipelineBenchmark(
        state, plan,
        ProcessPipelineTypedUnrolled4<14, 62>, 4);
  } else {
    RunPacketPipelineBenchmark(
        state, plan,
        ProcessPipelineTypedUnrolled4<30, 94>, 4);
  }
}

void BM_PacketBatchRuntimePipelineWorkingSetScalar(benchmark::State &state) {
  constexpr std::array<BatchFieldPlan, 2> kPlans = {BatchFieldPlan{14, 62},
                                                    BatchFieldPlan{30, 94}};
  const size_t plan_index = static_cast<size_t>(state.range(2));
  CHECK_LT(plan_index, kPlans.size());
  RunPacketPipelineWorkingSetBenchmark(
      state, kPlans[plan_index], ProcessPipelineRuntimeScalar, 1);
}

void BM_PacketBatchTypedPipelineWorkingSetScalar(benchmark::State &state) {
  if (state.range(2) == 0) {
    RunPacketPipelineWorkingSetBenchmark(
        state, BatchFieldPlan{14, 62},
        ProcessPipelineTypedScalar<14, 62>, 1);
  } else {
    RunPacketPipelineWorkingSetBenchmark(
        state, BatchFieldPlan{30, 94},
        ProcessPipelineTypedScalar<30, 94>, 1);
  }
}

void BM_PacketBatchRuntimeGenericInlineScalar(benchmark::State &state) {
  constexpr std::array<BatchFieldPlan, 2> kPlans = {BatchFieldPlan{14, 62},
                                                    BatchFieldPlan{30, 94}};
  const size_t plan_index = static_cast<size_t>(state.range(2));
  CHECK_LT(plan_index, kPlans.size());
  RunPacketBatchExecutionBenchmark(
      state, kPlans[plan_index], ProcessRuntimeGenericInlineScalar, 1);
}

void BM_PacketBatchRuntimeGenericUnrolled4(benchmark::State &state) {
  constexpr std::array<BatchFieldPlan, 2> kPlans = {BatchFieldPlan{14, 62},
                                                    BatchFieldPlan{30, 94}};
  const size_t plan_index = static_cast<size_t>(state.range(2));
  CHECK_LT(plan_index, kPlans.size());
  RunPacketBatchExecutionBenchmark(
      state, kPlans[plan_index], ProcessRuntimeGenericUnrolled4, 4);
}

void BM_PacketBatchRuntimeGenericScalar(benchmark::State &state) {
  RunRuntimeGenericBatchBenchmark<false>(state);
}

void BM_PacketBatchRuntimeGenericIlp4(benchmark::State &state) {
  RunRuntimeGenericBatchBenchmark<true>(state);
}

void BM_PacketBatchCompileTimeSpecializedScalar(benchmark::State &state) {
  if (state.range(2) == 0) {
    RunCompileTimeSpecializedBatchBenchmark<14, 62, false>(state);
  } else {
    RunCompileTimeSpecializedBatchBenchmark<30, 94, false>(state);
  }
}

void BM_PacketBatchCompileTimeSpecializedIlp4(benchmark::State &state) {
  if (state.range(2) == 0) {
    RunCompileTimeSpecializedBatchBenchmark<14, 62, true>(state);
  } else {
    RunCompileTimeSpecializedBatchBenchmark<30, 94, true>(state);
  }
}

BENCHMARK(BM_PacketBatchRuntimeGenericInlineScalar)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}})
    ->ArgNames({"batch", "shape", "field_plan"});
BENCHMARK(BM_PacketBatchRuntimeGenericUnrolled4)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}})
    ->ArgNames({"batch", "shape", "field_plan"});
BENCHMARK(BM_PacketBatchRuntimeGenericScalar)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}})
    ->ArgNames({"batch", "shape", "field_plan"});
BENCHMARK(BM_PacketBatchRuntimeGenericIlp4)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}})
    ->ArgNames({"batch", "shape", "field_plan"});
BENCHMARK(BM_PacketBatchCompileTimeSpecializedScalar)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}})
    ->ArgNames({"batch", "shape", "field_plan"});
BENCHMARK(BM_PacketBatchCompileTimeSpecializedIlp4)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}})
    ->ArgNames({"batch", "shape", "field_plan"});

BENCHMARK(BM_PacketBatchRuntimePipelineScalar)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}, {0, 1, 2, 3},
                   {8, 12, 16}})
    ->ArgNames({"batch", "shape", "field_plan", "lookup_mode", "table_log2"});
BENCHMARK(BM_PacketBatchRuntimePipelineUnrolled4)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}, {0, 1, 2, 3},
                   {8, 12, 16}})
    ->ArgNames({"batch", "shape", "field_plan", "lookup_mode", "table_log2"});
BENCHMARK(BM_PacketBatchRuntimePipelinePrefetch4)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}, {0, 1, 2, 3},
                   {8, 12, 16}})
    ->ArgNames({"batch", "shape", "field_plan", "lookup_mode", "table_log2"});
BENCHMARK(BM_PacketBatchTypedPipelineScalar)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}, {0, 1, 2, 3},
                   {8, 12, 16}})
    ->ArgNames({"batch", "shape", "field_plan", "lookup_mode", "table_log2"});
BENCHMARK(BM_PacketBatchTypedPipelineUnrolled4)
    ->ArgsProduct({{1, 8, 16, 32}, {0, 1, 2, 3}, {0, 1}, {0, 1, 2, 3},
                   {8, 12, 16}})
    ->ArgNames({"batch", "shape", "field_plan", "lookup_mode", "table_log2"});

BENCHMARK(BM_PacketBatchRuntimePipelineWorkingSetScalar)
    ->ArgsProduct({{8, 32}, {0, 1, 2, 3}, {0, 1}, {0, 1, 2, 3},
                   {8, 12, 16}})
    ->ArgNames({"batch", "shape", "field_plan", "lookup_mode", "table_log2"});
BENCHMARK(BM_PacketBatchTypedPipelineWorkingSetScalar)
    ->ArgsProduct({{8, 32}, {0, 1, 2, 3}, {0, 1}, {0, 1, 2, 3},
                   {8, 12, 16}})
    ->ArgNames({"batch", "shape", "field_plan", "lookup_mode", "table_log2"});

}  // namespace

int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
