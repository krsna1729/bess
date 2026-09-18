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

// PMD forwarding benchmarks -- the measurement substrate for every
// dataplane experiment that touches the NIC boundary (see MODERNIZATION.md:
// Phase B Stage 2, the llring-vs-rte_ring and mempool-backend experiments,
// MBUF_FAST_FREE, burst-size policy, rte_fib). Captured *before* any of
// those land, so each can be checked against real numbers.
//
// Uses DPDK virtual PMDs, so no NIC or hugepages are needed:
//   - net_null: TX discards (and frees) everything, RX always returns 0.
//     Measures the TX-burst + alloc/free path in isolation.
//   - net_ring: a port's TX enqueues onto its own ring and RX dequeues
//     from it, i.e. a self-loopback. Measures a full TX->RX round trip
//     through real rte_eth_rx/tx_burst calls.
//
// EAL is initialized with InitDpdk(0) (--no-huge, malloc-backed), the same
// no-hugepage fallback `bessd -m 0` uses, so this runs in the sandbox and
// in CI. Default pools are PlainPacketPool for the same reason; absolute
// Mpps here is not comparable to hugepage-backed production numbers, but
// *relative* comparisons between runs of this binary (the actual use case:
// before/after a refactor, same machine) are valid.
//
// Two families, with deliberately different timing boundaries -- read
// before comparing numbers across them:
//   - BM_PmdNullTx / BM_PmdRingRoundTrip: the PMD/interface boundary only.
//     Allocation is set up outside the timed region, so these isolate
//     rte_eth_rx/tx_burst (+ PMD-side free) cost. Use them for Stage 2,
//     burst-size, and FIB questions.
//   - BM_PmdNullTxEndToEnd / BM_PmdRingRoundTripEndToEnd: the full local
//     lifecycle -- alloc + TX + RX + free, all timed. Use them for
//     allocator-adjacent questions (mempool backend/cache), where omitting
//     half the lifecycle would be measuring the wrong thing.
// Neither replaces the cross-worker PortInc -> Queue -> PortOut harness
// the mempool experiment needs (allocate on worker A, free on worker B);
// that is its own work, gated on this file plus the DumpMempool() fix.
//
// Deliberately PMD-level, not module-graph-level: PortInc/PortOut modules
// need a live daemon (workers, scheduler loop), which has no standalone
// harness -- this exercises the exact rte_eth_rx/tx_burst boundary Phase B
// Stage 2 must redesign, without it.

#include <benchmark/benchmark.h>
#include <glog/logging.h>

#include "drivers/pmd.h"
#include "opts.h"
#include "packet_pool.h"
#include "pktbatch.h"
#include "port.h"

namespace {

const size_t kPoolCapacity = 32767;
const int kPktLen = 60;  // minimum Ethernet-sized payload; content is irrelevant

// Created once, shared by every benchmark below: re-probing the same vdev
// name twice fails, so ports live for the whole process.
struct PmdFixture {
  PMDPort null_port;
  PMDPort ring_port;

  PmdFixture() {
    // Sandbox-safe EAL: --no-huge, malloc-backed (bessd -m 0 equivalent).
    // FLAGS_m = 0 makes CreateDefaultPools() pick PlainPacketPool.
    FLAGS_m = 0;
    bess::PacketPool::CreateDefaultPools(kPoolCapacity);

    InitPort(&null_port, "net_null0");
    InitPort(&ring_port, "net_ring0");
  }

  void InitPort(PMDPort *port, const std::string &vdev) {
    port->num_queues[PACKET_DIR_INC] = 1;
    port->num_queues[PACKET_DIR_OUT] = 1;
    port->queue_size[PACKET_DIR_INC] = 1024;
    port->queue_size[PACKET_DIR_OUT] = 1024;

    bess::pb::PMDPortArg arg;
    arg.set_vdev(vdev);
    CommandResponse resp = port->Init(arg);
    CHECK(resp.error().code() == 0)
        << "PMDPort::Init(" << vdev << ") failed: " << resp.error().errmsg();
  }
};

PmdFixture &GetFixture() {
  static PmdFixture fixture;
  return fixture;
}

bess::PacketPool *DefaultPool() {
  bess::PacketPool *pool = bess::PacketPool::GetDefaultPool(0);
  CHECK(pool != nullptr);
  return pool;
}

// TX-only through the null PMD: the TX-burst path in isolation (RX is
// meaningless here -- always 0). Allocation is deliberately outside the
// timed region; see the EndToEnd twin below for the full lifecycle.
void BM_PmdNullTx(benchmark::State &state) {
  PmdFixture &fx = GetFixture();
  bess::PacketPool *pool = DefaultPool();
  const int batch = static_cast<int>(state.range(0));
  bess::Packet *pkts[bess::PacketBatch::kMaxBurst];
  uint64_t sent_total = 0;
  uint64_t dropped = 0;

  for (auto _ : state) {
    state.PauseTiming();
    CHECK(pool->AllocBulk(pkts, batch, kPktLen));
    state.ResumeTiming();

    int sent = fx.null_port.SendPackets(0, pkts, batch);
    sent_total += static_cast<uint64_t>(sent);
    dropped += static_cast<uint64_t>(batch - sent);
    // The null PMD frees everything it accepts, so nothing to free here.
    // Anything it didn't accept is a real drop: free it to keep the pool
    // balanced across iterations.
    for (int i = sent; i < batch; i++) {
      bess::Packet::Free(pkts[i]);
    }
  }
  // Actual packets transmitted, not requested: if a future change starts
  // dropping, throughput must visibly fall, not look excellent while doing
  // less work. Drops are reported separately below.
  state.SetItemsProcessed(sent_total);
  state.counters["tx_drops"] = benchmark::Counter(dropped);
}
BENCHMARK(BM_PmdNullTx)->RangeMultiplier(2)->Range(1, bess::PacketBatch::kMaxBurst);

// Null-TX twin with the full local lifecycle timed: alloc + TX +
// PMD-side free. The allocator-facing variant; see the file header.
void BM_PmdNullTxEndToEnd(benchmark::State &state) {
  PmdFixture &fx = GetFixture();
  bess::PacketPool *pool = DefaultPool();
  const int batch = static_cast<int>(state.range(0));
  bess::Packet *pkts[bess::PacketBatch::kMaxBurst];
  uint64_t sent_total = 0;
  uint64_t dropped = 0;

  for (auto _ : state) {
    CHECK(pool->AllocBulk(pkts, batch, kPktLen));
    int sent = fx.null_port.SendPackets(0, pkts, batch);
    sent_total += static_cast<uint64_t>(sent);
    dropped += static_cast<uint64_t>(batch - sent);
    for (int i = sent; i < batch; i++) {
      bess::Packet::Free(pkts[i]);
    }
  }
  state.SetItemsProcessed(sent_total);
  state.counters["tx_drops"] = benchmark::Counter(dropped);
}
BENCHMARK(BM_PmdNullTxEndToEnd)->RangeMultiplier(2)->Range(1, bess::PacketBatch::kMaxBurst);

// Full self-loopback through the ring PMD: TX a burst, RX it back, free.
// This is the closest standalone analogue of PortInc -> PortOut forwarding.
// Allocation is outside the timed region (interface-boundary variant).
void BM_PmdRingRoundTrip(benchmark::State &state) {
  PmdFixture &fx = GetFixture();
  bess::PacketPool *pool = DefaultPool();
  const int batch = static_cast<int>(state.range(0));
  bess::Packet *tx[bess::PacketBatch::kMaxBurst];
  bess::Packet *rx[bess::PacketBatch::kMaxBurst];
  uint64_t tx_drops = 0;
  uint64_t rx_total = 0;

  for (auto _ : state) {
    state.PauseTiming();
    CHECK(pool->AllocBulk(tx, batch, kPktLen));
    state.ResumeTiming();

    int sent = fx.ring_port.SendPackets(0, tx, batch);
    tx_drops += static_cast<uint64_t>(batch - sent);
    for (int i = sent; i < batch; i++) {
      bess::Packet::Free(tx[i]);
    }

    int recvd = fx.ring_port.RecvPackets(0, rx, batch);
    rx_total += static_cast<uint64_t>(recvd);
    // Steady state: the ring is empty at iteration start (batch <= 32 is
    // far below the ring size and the previous iteration drained it), so
    // recvd == sent every iteration. Enforced, not just documented: a
    // future PMD/Stage-2 change that strands a packet would otherwise leak
    // it into the next iteration and silently measure a shifted workload.
    CHECK_EQ(recvd, sent);
    bess::Packet::Free(rx, recvd);
  }
  state.SetItemsProcessed(rx_total);
  state.counters["tx_drops"] = benchmark::Counter(tx_drops);
}
BENCHMARK(BM_PmdRingRoundTrip)->RangeMultiplier(2)->Range(1, bess::PacketBatch::kMaxBurst);

// Ring twin with the full local lifecycle timed: alloc + TX + RX + free.
// The allocator-facing variant; see the file header.
void BM_PmdRingRoundTripEndToEnd(benchmark::State &state) {
  PmdFixture &fx = GetFixture();
  bess::PacketPool *pool = DefaultPool();
  const int batch = static_cast<int>(state.range(0));
  bess::Packet *tx[bess::PacketBatch::kMaxBurst];
  bess::Packet *rx[bess::PacketBatch::kMaxBurst];
  uint64_t tx_drops = 0;
  uint64_t rx_total = 0;

  for (auto _ : state) {
    CHECK(pool->AllocBulk(tx, batch, kPktLen));
    int sent = fx.ring_port.SendPackets(0, tx, batch);
    tx_drops += static_cast<uint64_t>(batch - sent);
    for (int i = sent; i < batch; i++) {
      bess::Packet::Free(tx[i]);
    }

    int recvd = fx.ring_port.RecvPackets(0, rx, batch);
    rx_total += static_cast<uint64_t>(recvd);
    CHECK_EQ(recvd, sent);  // self-loopback invariant, see above
    bess::Packet::Free(rx, recvd);
  }
  state.SetItemsProcessed(rx_total);
  state.counters["tx_drops"] = benchmark::Counter(tx_drops);
}
BENCHMARK(BM_PmdRingRoundTripEndToEnd)->RangeMultiplier(2)->Range(1, bess::PacketBatch::kMaxBurst);

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
