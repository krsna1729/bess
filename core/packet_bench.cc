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

// Baseline microbenchmarks for Packet/PacketPool/PacketBatch -- the
// primitives Phase B (packet/mbuf architecture refactor, see
// MODERNIZATION.md) would touch. Captured *before* any such refactor so a
// candidate redesign can be checked against real numbers instead of
// intuition.
//
// Uses PlainPacketPool (see packet_pool.h): the only pool backend that
// doesn't require real hugepages, which this sandbox doesn't have. Not
// representative of DpdkPacketPool/BessPacketPool's absolute numbers, but
// the operations benchmarked here (Alloc/Free bookkeeping, accessor
// arithmetic, batch pointer-array copies) don't depend on which pool
// backend supplied the underlying memory.

#include <benchmark/benchmark.h>
#include <glog/logging.h>

#include "packet.h"
#include "packet_pool.h"
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
    bess::Packet *pkt = pool.Alloc();
    benchmark::DoNotOptimize(pkt);
    bess::Packet::Free(pkt);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PacketAllocFree);

void BM_PacketAllocFreeBulk(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t kBatch = bess::PacketBatch::kMaxBurst;
  bess::Packet *pkts[kBatch];

  for (auto _ : state) {
    bool ok = pool.AllocBulk(pkts, kBatch);
    benchmark::DoNotOptimize(pkts);
    if (ok) {
      bess::Packet::Free(pkts, kBatch);
    }
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_PacketAllocFreeBulk);

void BM_PacketHeadData(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  bess::Packet *pkt = pool.Alloc(64);

  for (auto _ : state) {
    char *data = pkt->head_data<char *>();
    benchmark::DoNotOptimize(data);
  }
  state.SetItemsProcessed(state.iterations());

  bess::Packet::Free(pkt);
}
BENCHMARK(BM_PacketHeadData);

void BM_PacketAppendTrim(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  bess::Packet *pkt = pool.Alloc();
  const uint16_t kLen = 64;

  for (auto _ : state) {
    void *data = pkt->append(kLen);
    benchmark::DoNotOptimize(data);
    pkt->trim(kLen);
  }
  state.SetItemsProcessed(state.iterations());

  bess::Packet::Free(pkt);
}
BENCHMARK(BM_PacketAppendTrim);

// Every module attribute read/write (Module::get_attr/set_attr/ptr_attr,
// module.h) funnels through Packet::metadata<T>(), which as of Phase B
// Stage 1 (see MODERNIZATION.md) resolves via Packet::priv() ->
// rte_mbuf_to_priv() instead of a Packet-side union member. This exercises
// that path directly to catch a regression in cost, not just correctness.
void BM_PacketMetadataAccess(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  bess::Packet *pkt = pool.Alloc();

  for (auto _ : state) {
    uintptr_t addr = pkt->metadata<uintptr_t>();
    benchmark::DoNotOptimize(addr);
  }
  state.SetItemsProcessed(state.iterations());

  bess::Packet::Free(pkt);
}
BENCHMARK(BM_PacketMetadataAccess);

// "Forwarding" a batch from one module to the next is, at its core, a
// pointer-array copy of up to kMaxBurst Packet* -- this is that copy in
// isolation, without the surrounding Module/Gate/Task dispatch machinery
// (which has no standalone-benchmark harness yet; see MODERNIZATION.md).
void BM_BatchForward(benchmark::State &state) {
  bess::PlainPacketPool &pool = GetPool();
  const size_t kBatch = bess::PacketBatch::kMaxBurst;
  bess::Packet *pkts[kBatch];
  CHECK(pool.AllocBulk(pkts, kBatch));

  bess::PacketBatch src;
  src.clear();
  for (size_t i = 0; i < kBatch; i++) {
    src.add(pkts[i]);
  }

  bess::PacketBatch dst;
  for (auto _ : state) {
    dst.clear();
    dst.add(&src);
    benchmark::DoNotOptimize(dst.pkts());
  }
  state.SetItemsProcessed(state.iterations() * kBatch);

  bess::Packet::Free(pkts, kBatch);
}
BENCHMARK(BM_BatchForward);

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
