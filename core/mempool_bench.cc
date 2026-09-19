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

// Mempool backend / cache size / worker topology benchmark -- the measurement
// substrate for the mempool experiment (MODERNIZATION.md benchmark-backlog
// item 3) and, right after it, for the WorkerId-vs-rte_lcore_id()
// decoupling. pmd_bench.cc covers the other half of that plan (the PMD
// boundary); this covers the allocator.
//
// Why a dedicated harness: BESS's packet pools are per-socket and shared by
// every worker, while DPDK's mempool cache is per *lcore* -- keyed on
// rte_lcore_id(), the id DPDK assigns when a worker pthread registers as a
// non-EAL lcore (`rte_thread_register()`, core/worker.cc; before
// MODERNIZATION.md entry 33 BESS wrote DPDK's private
// RTE_PER_LCORE(_lcore_id) for the same purpose). A packet can therefore be
// allocated from one worker's cache, handed over a Queue, and freed into
// another worker's cache. Neither the single-threaded pmd_bench EndToEnd
// variants nor packet_bench can answer whether the current `ring_mp_mc` +
// cache 512 configuration is sensible for that; this binary can.
//
// Two families, deliberately different shapes:
//   - BM_MempoolLocal: one thread, alloc-burst + free-burst of the same
//     packets. With a cache this is the cache-hit path; with cache 0 it is
//     the bare backend path. The "A alloc -> A free" case.
//   - BM_MempoolPipeline: a producer thread allocates bursts and enqueues
//     them on an rte_ring in Queue's exact mode (MP enqueue burst, SC
//     dequeue burst); the benchmark thread consumes and frees. The
//     "A alloc -> B free" case, and the minimal shape of the real
//     Source -> Queue -> Sink dataplane. The ring is included in the timed
//     region on purpose: it is the handoff a real pipeline pays for, and it
//     is identical across every measured configuration.
//
// Parameters are mempool ops backend x cache size x batch size, selected
// with --benchmark_filter. Recommended for a real (non-smoke) run, on a
// quiet machine:
//
//   ./mempool_bench --benchmark_repetitions=5
//       --benchmark_report_aggregates_only=true --pin=cores
//
// Flags of this binary (stripped from argv before Google Benchmark parses
// it):
//   --lcore_mode=bess|register|none      default bess
//       bess:     `RTE_PER_LCORE(_lcore_id) = wid` -- BESS's mechanism up
//                 to MODERNIZATION.md entry 33, kept as the "before" side
//                 of this bench's own A/B; production no longer does this.
//       register: rte_thread_register() -- what core/worker.cc does now
//                 (entry 33), and what this axis existed to de-risk before
//                 that commit. Note the registered *consumer* thread here
//                 is Google Benchmark's main/EAL thread, which
//                 rte_thread_register() merely re-labels from the EAL main
//                 lcore to a fresh non-EAL lcore id; in bessd the
//                 allocator's threads are spawned workers, so this
//                 difference does not exist there.
//       none:     no lcore id at all: rte_lcore_id() is forced to
//                 LCORE_ID_ANY, so DPDK hands out no default cache whatever
//                 cache_size says -- the control that shows what the cache
//                 axis is worth (and that a worker without an lcore id
//                 silently gets none). Asserted, see CheckCacheInPlay().
//   --pin=none|smt|cores|numa            default none
//       none:  no affinity changes (shared/noisy hosts: the scheduler
//              usually places threads better than any static assignment).
//       smt:   the two threads on sibling hyperthreads of one physical core.
//       cores: two distinct physical cores of the same package.
//       numa:  two different NUMA nodes. Fails loudly on a single-node host
//              instead of quietly reporting same-node numbers as
//              cross-node ones.
//     Placement comes from sysfs (/sys/devices/system/cpu/...), restricted
//     to the process's own affinity mask. The chosen CPUs are logged at
//     startup and reported per case (prod_cpu/cons_cpu counters), so a run
//     can never be silently mis-described by the machine it landed on.
//
// Metrics per case (Google Benchmark's own JSON output,
// --benchmark_format=json, has the unrounded values):
//   - items/s as Google Benchmark counts them, plus `pkt_per_s`: the
//     pipeline's wall-clock rate for one epoch, measured here. Both are
//     reported because Google Benchmark's rate is derived from *thread CPU
//     time* (benchmark_runner.cc: `i.seconds = i.results.cpu_time_used`
//     unless UseRealTime()), which for a two-thread pipeline is the
//     consumer's busy time, not the epoch's duration.
//   - prod_busy / cons_busy: each thread's CPU time over the epoch wall
//     time. A producer pinned against a saturated consumer shows up here
//     before it shows up in throughput.
//   - prod_cache / cons_cache: mean default-cache occupancy (objects) of
//     each worker at epoch boundaries, read from the public
//     `struct rte_mempool`'s local_cache[]. This is the cache-imbalance
//     signal: a producer that keeps draining its cache and a consumer that
//     keeps filling one is exactly the cross-worker failure mode a large
//     per-lcore cache can create. Note rte_mempool_avail_count() *includes*
//     cache contents (DPDK 25.11 adds every local_cache[].len to the backend
//     count), so availability alone can never show this split.
//   - cache_len (local family): same reading for the single-thread case --
//     how much the alloc/free loop parks in its own cache.
//   - alloc_failures (asserted zero), cpu_ns_pkt (local family).
// Every epoch boundary is also checked: avail_count() must be back to the
// pool capacity, i.e. nothing may still be allocated. A stranded packet
// would silently shift every later epoch's workload, and this is the one
// moment the check is exact.
//
// What this file deliberately does not measure:
//   - Cache flush/refill *frequency*: DPDK only counts cache operations when
//     built with -Dc_args=-DRTE_LIBRTE_MEMPOOL_STATS, and turning that on
//     would instrument the very path being timed. Occupancy (above) plus
//     throughput bound the behaviour without perturbing it.
//   - NUMA locality: this sandbox is single-socket. --pin=numa is the hook
//     for real hardware; there is no point faking a cross-socket run here.
//   - Module dispatch overhead (no C++ harness for Module::ProcessBatch()
//     outside a live daemon exists yet).
//
// Numbers are relative-comparable only (same machine, same flags), as with
// every bench in this tree: pool memory is mmap-backed
// (MEMPOOL_F_NO_IOVA_CONTIG, PlainPacketPool's shape) and EAL runs --no-huge,
// so this builds and runs in CI and in the sandbox alike.

#include <benchmark/benchmark.h>
#include <glog/logging.h>

#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "dpdk.h"
#include "packet.h"
#include "snbuf_layout.h"
#include "utils/rte_ring_alloc.h"

namespace {

using bess::Packet;

// Pool and handoff sizing. The worst-case in-flight set must stay strictly
// below the pool capacity, or the producer would spin on a drained pool --
// a shifted workload, not a measurement:
//   ring (2048) + two caches (2 x 512) + one held burst per side (2 x 32)
//   = 3136 << 8191.
const size_t kPoolCapacity = 8191;
const size_t kRingSlots = 2048;        // power of two: rte_ring_init() insists
const uint64_t kEpochItems = 1 << 16;  // packets moved per timed iteration
const size_t kPktLen = 60;
const size_t kMaxBatch = bess::PacketBatch::kMaxBurst;

// A generous cap on allocation failures before giving up: transient failure
// means the pool sizing above is wrong, and the right answer is a loud
// failure, not a benchmark that silently measures a smaller workload. At
// ~10ns per retry this is ~10ms of grace.
const uint64_t kMaxAllocFailures = 1 << 20;

enum BackendId {
  kRingMpMc = 0,
  kRingMtRts,
  kRingMtHts,
  kStack,
  kLfStack,
  kNumBackends,
};

// Names are DPDK's registered mempool ops names (drivers/mempool/ring and
// .../stack). `librte_mempool_stack` is part of the default DPDK build;
// Create() still reports an unavailable backend instead of crashing, so a
// DPDK configured without it turns into skipped cases, not a broken run.
const char *const kBackendNames[kNumBackends] = {
    "ring_mp_mc", "ring_mt_rts", "ring_mt_hts", "stack", "lf_stack"};

enum class LcoreMode { kBess, kRegister, kNone };
enum class PinMode { kNone, kSmt, kCores, kNuma };

LcoreMode g_lcore_mode = LcoreMode::kBess;
PinMode g_pin_mode = PinMode::kNone;
int g_producer_cpu = -1;  // -1: leave the scheduler alone
int g_consumer_cpu = -1;

// Worker ids for BESS's manual lcore assignment. Distinct by construction:
// two workers sharing one lcore id would share one default cache and quietly
// delete the cross-worker question this benchmark exists to answer.
const unsigned kProducerWid = 0;
const unsigned kConsumerWid = 1;

const char *LcoreModeName(LcoreMode mode) {
  switch (mode) {
    case LcoreMode::kBess:
      return "bess";
    case LcoreMode::kRegister:
      return "register";
    case LcoreMode::kNone:
      return "none";
  }
  return "?";
}

const char *PinModeName(PinMode mode) {
  switch (mode) {
    case PinMode::kNone:
      return "none";
    case PinMode::kSmt:
      return "smt";
    case PinMode::kCores:
      return "cores";
    case PinMode::kNuma:
      return "numa";
  }
  return "?";
}

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t ThreadCpuNs() {
  struct timespec ts;
  CHECK_EQ(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), 0);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

std::string ReadSysfsLine(const std::string &path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}

// "0-3,8,12-13" -> {0,1,2,3,8,12,13}
std::vector<int> ParseCpuList(const std::string &list) {
  std::vector<int> cpus;
  size_t pos = 0;
  while (pos < list.size()) {
    const size_t comma = list.find(',', pos);
    const std::string item = list.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const size_t dash = item.find('-');
    if (dash == std::string::npos) {
      cpus.push_back(std::atoi(item.c_str()));
    } else {
      const int lo = std::atoi(item.substr(0, dash).c_str());
      const int hi = std::atoi(item.substr(dash + 1).c_str());
      for (int cpu = lo; cpu <= hi; cpu++) {
        cpus.push_back(cpu);
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return cpus;
}

// Topology facts placement needs, all read from sysfs (BESS is Linux-only,
// so there is no portability shim). Only consulted when a --pin mode is
// requested; a machine whose sysfs does not answer fails loudly rather than
// pinning somewhere unintended.
struct Topology {
  std::vector<int> allowed;                 // CPUs this process may use
  std::map<int, int> package_of;            // cpu -> physical_package_id
  std::map<int, int> core_of;               // cpu -> core_id
  std::vector<std::vector<int>> node_cpus;  // NUMA node -> allowed CPUs
};

Topology ReadTopology(const cpu_set_t &allowed_mask) {
  Topology topo;
  for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
    if (!CPU_ISSET(cpu, &allowed_mask)) {
      continue;
    }
    topo.allowed.push_back(cpu);
    const std::string base =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
    const std::string package = ReadSysfsLine(base + "physical_package_id");
    const std::string core = ReadSysfsLine(base + "core_id");
    CHECK(!package.empty() && !core.empty())
        << "cannot read CPU topology for cpu" << cpu;
    topo.package_of[cpu] = std::atoi(package.c_str());
    topo.core_of[cpu] = std::atoi(core.c_str());
  }

  const std::vector<int> nodes =
      ParseCpuList(ReadSysfsLine("/sys/devices/system/node/online"));
  for (int node : nodes) {
    const std::string list = ReadSysfsLine("/sys/devices/system/node/node" +
                                           std::to_string(node) + "/cpulist");
    if (list.empty()) {
      continue;  // no NUMA descriptions in sysfs: leave the node list short
    }
    std::vector<int> cpus;
    for (int cpu : ParseCpuList(list)) {
      if (topo.core_of.count(cpu) != 0) {  // allowed CPUs only
        cpus.push_back(cpu);
      }
    }
    if (!cpus.empty()) {
      topo.node_cpus.push_back(cpus);
    }
  }
  return topo;
}

void ChoosePlacement() {
  if (g_pin_mode == PinMode::kNone) {
    return;
  }

  // Snapshot derived from the process mask taken in main(), never from a
  // thread's own (possibly already narrowed) affinity.
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CHECK_EQ(sched_getaffinity(0, sizeof(mask), &mask), 0);
  const Topology topo = ReadTopology(mask);
  CHECK(!topo.allowed.empty()) << "no CPUs available to this process";

  switch (g_pin_mode) {
    case PinMode::kSmt: {
      // Group by physical core and take the first with two allowed siblings.
      std::map<std::pair<int, int>, std::vector<int>> cores;
      for (int cpu : topo.allowed) {
        cores[{topo.package_of.at(cpu), topo.core_of.at(cpu)}].push_back(cpu);
      }
      for (const auto &entry : cores) {
        if (entry.second.size() >= 2) {
          g_producer_cpu = entry.second[0];
          g_consumer_cpu = entry.second[1];
          return;
        }
      }
      LOG(FATAL) << "--pin=smt: no physical core has two allowed sibling CPUs "
                    "in this process's affinity mask";
      return;
    }
    case PinMode::kCores: {
      std::map<std::pair<int, int>, std::vector<int>> cores;
      for (int cpu : topo.allowed) {
        cores[{topo.package_of.at(cpu), topo.core_of.at(cpu)}].push_back(cpu);
      }
      // First core of a package, then the first core of that same package
      // with a different core id.
      const auto first = cores.begin();
      if (first == cores.end()) {
        LOG(FATAL) << "--pin=cores: no CPUs available";
        return;
      }
      for (auto entry = std::next(first); entry != cores.end(); ++entry) {
        if (entry->first.first == first->first.first) {
          g_producer_cpu = first->second[0];
          g_consumer_cpu = entry->second[0];
          return;
        }
      }
      LOG(FATAL) << "--pin=cores: all allowed CPUs are the same physical core";
      return;
    }
    case PinMode::kNuma: {
      if (topo.node_cpus.size() < 2) {
        LOG(FATAL) << "--pin=numa: only " << topo.node_cpus.size()
                   << " NUMA node(s) are usable by this process; "
                      "cross-node placement is impossible on this host";
        return;
      }
      g_producer_cpu = topo.node_cpus[0][0];
      g_consumer_cpu = topo.node_cpus[1][0];
      return;
    }
    case PinMode::kNone:
      return;
  }
}

void PinCurrentThread(int cpu) {
  if (cpu < 0) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  CHECK_EQ(sched_setaffinity(0, sizeof(set), &set), 0)
      << "cannot pin to cpu" << cpu;
}

// Gives the calling thread whatever lcore identity --lcore_mode asks for,
// and takes it back on scope exit. The lcore id is the key DPDK's default
// mempool cache is stored under, so this is the axis the lcore migration
// (MODERNIZATION.md entry 33) was measured along; both ends of that change
// (BESS's manual assignment and rte_thread_register()) stay first-class
// here -- as a kept A/B, not as pending work.
class LcoreScope {
 public:
  explicit LcoreScope(unsigned wid) {
    switch (g_lcore_mode) {
      case LcoreMode::kBess:
        // What core/worker.cc did before entry 33: a DPDK implementation
        // detail, written from outside DPDK. Kept as this bench's "before".
        RTE_PER_LCORE(_lcore_id) = wid;
        break;
      case LcoreMode::kRegister:
        CHECK_EQ(rte_thread_register(), 0)
            << "rte_thread_register() failed: " << rte_strerror(rte_errno);
        break;
      case LcoreMode::kNone:
        // The benchmark thread starts out as the EAL main lcore (127, from
        // BESS's --main-lcore), and DPDK gives a default cache to *any*
        // rte_lcore_id() that is not LCORE_ID_ANY -- registered or not. So
        // "no lcore id" has to be written explicitly here, the same manual
        // write `bess` mode does, pointing the other way; spawned (non-EAL)
        // threads like the producer already start at LCORE_ID_ANY.
        RTE_PER_LCORE(_lcore_id) = LCORE_ID_ANY;
        break;
    }
  }

  ~LcoreScope() {
    if (g_lcore_mode == LcoreMode::kRegister) {
      rte_thread_unregister();
    }
  }

  LcoreScope(const LcoreScope &) = delete;
  LcoreScope &operator=(const LcoreScope &) = delete;

  unsigned id() const { return rte_lcore_id(); }
};

// The cache axis of the sweep only exists if DPDK's per-lcore cache is
// actually in play, and rte_mempool_default_cache() returns NULL both for
// cache_size == 0 *and* for any thread whose rte_lcore_id() is
// LCORE_ID_ANY. Assert the expectation instead of assuming it: a silently
// absent cache would make every cache size in the sweep measure the same
// thing (the "measured the wrong workload" failure class recorded for the
// llring experiment in MODERNIZATION.md entry 28).
void CheckCacheInPlay(rte_mempool *mp, unsigned cache_size) {
  const bool expected = cache_size > 0 && g_lcore_mode != LcoreMode::kNone;
  const bool present = rte_mempool_default_cache(mp, rte_lcore_id()) != nullptr;
  CHECK_EQ(present, expected)
      << "mempool cache " << (present ? "present" : "absent") << " but "
      << (expected ? "expected" : "not expected")
      << " (cache_size=" << cache_size << ", lcore_id=" << rte_lcore_id()
      << ", lcore_mode=" << LcoreModeName(g_lcore_mode) << ")";
}

// DPDK's per-lcore default cache occupancy, read out of the public
// `struct rte_mempool` (local_cache[] -> len). It is reported separately
// from rte_mempool_avail_count() on purpose: avail_count *folds the default
// caches into its count* (verified in DPDK 25.11, rte_mempool.c: it adds
// every mp->local_cache[lcore].len to the backend count), so it says
// nothing about the cache-versus-ring split -- which is the interesting
// quantity for a cross-worker workload. 0 whenever no cache is in play.
uint64_t CacheLen(rte_mempool *mp, unsigned lcore_id) {
  if (mp->cache_size == 0 || lcore_id == static_cast<unsigned>(LCORE_ID_ANY)) {
    return 0;
  }
  return mp->local_cache[lcore_id].len;
}

rte_ring *CreateRing() {
  const ssize_t bytes = rte_ring_get_memsize(kRingSlots);
  CHECK_GT(bytes, 0);
  void *mem = bess::utils::AllocRingMem(static_cast<size_t>(bytes));
  CHECK(mem != nullptr);
  rte_ring *ring = static_cast<rte_ring *>(mem);
  const std::string name = bess::utils::NewRingName("mempoolbench");
  CHECK_EQ(rte_ring_init(ring, name.c_str(), kRingSlots, RING_F_SC_DEQ), 0);
  return ring;
}

void DestroyRing(rte_ring *ring) {
  std::free(ring);
}

// rte_pktmbuf_init()/rte_pktmbuf_reset() read the pool's private data
// through rte_mempool_get_priv(), which must start with
// rte_pktmbuf_pool_private (same shape as PacketPool::PoolPrivate, minus the
// owner backpointer this bench has no use for).
struct BenchPoolPrivate {
  rte_pktmbuf_pool_private dpdk_priv;
};

void InitPacket(rte_mempool *mp, void *, void *mbuf, unsigned index) {
  rte_pktmbuf_init(mp, nullptr, mbuf, index);

  auto *pkt = static_cast<Packet *>(mbuf);
  pkt->set_vaddr(pkt);
  pkt->set_paddr(rte_mempool_virt2iova(pkt));
}

// One rte_mempool built the way PlainPacketPool builds one (mmap-backed,
// MEMPOOL_F_NO_IOVA_CONTIG, mbufs initialized by rte_pktmbuf_init + BESS's
// vaddr/paddr bookkeeping), except that the two knobs PacketPool hardcodes --
// ops backend and cache size -- are explicit parameters. Kept local to this
// file rather than threaded through PacketPool: the experiment must not
// change production allocation behavior to be able to measure it.
class BenchPool {
 public:
  // nullptr when `ops` is not registered in this DPDK build, or the pool
  // could not be fully populated; the caller reports the case as skipped.
  static std::unique_ptr<BenchPool> Create(const char *ops,
                                           unsigned cache_size) {
    std::unique_ptr<BenchPool> pool(new BenchPool());
    if (!pool->Init(ops, cache_size)) {
      return nullptr;
    }
    return pool;
  }

  ~BenchPool() {
    if (mp_ != nullptr) {
      rte_mempool_free(mp_);
    }
    if (mem_ != nullptr) {
      munmap(mem_, mem_size_);
    }
  }

  rte_mempool *mp() const { return mp_; }

  // The mempool half of PacketPool::AllocBulk(). The 12 fields
  // rte_pktmbuf_reset() would write are already in the required state when a
  // packet comes back out of the pool: rte_pktmbuf_init() set them at pool
  // creation, and neither free path (BESS's fast free or DPDK's
  // rte_pktmbuf_free) leaves them in any other state, so only the two
  // lengths, which really do vary per allocation, are rewritten. That
  // omission is ~2 stores of constant work, identical in every measured
  // configuration, and it keeps this file out of Packet's private fields
  // (friend class PacketPool) entirely.
  bool AllocBulk(Packet **pkts, size_t count, size_t len) {
    if (rte_mempool_get_bulk(mp_, reinterpret_cast<void **>(pkts), count) < 0) {
      return false;
    }
    for (size_t i = 0; i < count; i++) {
      pkts[i]->set_total_len(len);
      pkts[i]->set_data_len(len);
    }
    return true;
  }

 private:
  BenchPool() = default;

  bool Init(const char *ops, unsigned cache_size) {
    static std::atomic<unsigned> next_id{0};
    char name[64];
    snprintf(name, sizeof(name), "MempoolBench%u", next_id.fetch_add(1));

    mp_ = rte_mempool_create_empty(name, kPoolCapacity, sizeof(Packet),
                                   cache_size, sizeof(BenchPoolPrivate),
                                   SOCKET_ID_ANY, 0);
    if (mp_ == nullptr) {
      LOG(ERROR) << "rte_mempool_create_empty() failed: "
                 << rte_strerror(rte_errno);
      return false;
    }
    if (rte_mempool_set_ops_byname(mp_, ops, nullptr) < 0) {
      LOG(WARNING) << "mempool ops backend not available: " << ops;
      return false;
    }

    mp_->flags |= MEMPOOL_F_NO_IOVA_CONTIG;

    const size_t page_shift = static_cast<size_t>(__builtin_ffs(getpagesize()));
    size_t min_chunk_size = 0;
    size_t align = 0;
    const size_t bytes = rte_mempool_op_calc_mem_size_default(
        mp_, mp_->size, page_shift, &min_chunk_size, &align);
    mem_size_ = bytes;
    mem_ = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem_ == MAP_FAILED) {
      mem_ = nullptr;
      PLOG(ERROR) << "mmap()";
      return false;
    }
    // Best-effort, exactly like PlainPacketPool: no guarantee of pinning.
    (void)mlock(mem_, bytes);

    const ssize_t populated = rte_mempool_populate_iova(
        mp_, static_cast<char *>(mem_), RTE_BAD_IOVA, bytes, nullptr, nullptr);
    if (populated < 0) {
      LOG(WARNING) << "rte_mempool_populate_iova() returned " << populated
                   << " (rte_errno=" << rte_errno << ", "
                   << rte_strerror(rte_errno) << ")";
    }
    if (mp_->populated_size != mp_->size) {
      LOG(WARNING) << name << ": requested " << mp_->size << " objects, got "
                   << mp_->populated_size;
      return false;
    }

    BenchPoolPrivate priv = {};
    priv.dpdk_priv.mbuf_data_room_size = SNBUF_HEADROOM + SNBUF_DATA;
    priv.dpdk_priv.mbuf_priv_size = SNBUF_RESERVE;
    priv.dpdk_priv.flags = 0;
    rte_pktmbuf_pool_init(mp_, &priv.dpdk_priv);
    rte_mempool_obj_iter(mp_, InitPacket, nullptr);
    return true;
  }

  rte_mempool *mp_ = nullptr;
  void *mem_ = nullptr;
  size_t mem_size_ = 0;
};

// One timed pipeline epoch's raw measurements, filled by the producer thread
// and by the consumer (Google Benchmark's own thread).
struct ProducerArgs {
  BenchPool *pool;
  rte_ring *ring;
  size_t batch;
  uint64_t quota;
  unsigned cache_size;
  unsigned consumer_lcore;
  const std::atomic<bool> *start;
  uint64_t cpu_ns = 0;
  uint64_t failures = 0;
  int cpu_id = -1;
  uint64_t cache_len = 0;  // producer's default-cache occupancy at exit
};

void ProducerLoop(ProducerArgs *args) {
  const LcoreScope lcore(kProducerWid);
  PinCurrentThread(g_producer_cpu);
  CheckCacheInPlay(args->pool->mp(), args->cache_size);
  if (g_lcore_mode != LcoreMode::kNone) {
    CHECK_NE(lcore.id(), args->consumer_lcore)
        << "producer and consumer ended up on the same lcore id; their "
           "default mempool caches would be the same object and the "
           "cross-worker case would not be measured at all";
  }

  Packet *pkts[kMaxBatch];
  while (!args->start->load(std::memory_order_acquire)) {
  }
  args->cpu_id = sched_getcpu();

  const uint64_t cpu0 = ThreadCpuNs();
  uint64_t moved = 0;
  while (moved < args->quota) {
    if (!args->pool->AllocBulk(pkts, args->batch, kPktLen)) {
      args->failures++;
      CHECK_LT(args->failures, kMaxAllocFailures)
          << "pool drained: the producer's allocation bursts no longer fit "
             "in the pool alongside the ring and both caches";
      continue;
    }
    size_t sent = 0;
    while (sent < args->batch) {
      // Queue's exact enqueue mode (MP burst; the ring is created with
      // RING_F_SC_DEQ for the consumer side).
      sent += rte_ring_mp_enqueue_burst(
          args->ring, reinterpret_cast<void *const *>(pkts + sent),
          static_cast<unsigned>(args->batch - sent), nullptr);
    }
    moved += args->batch;
  }
  args->cpu_ns = ThreadCpuNs() - cpu0;
  args->cache_len = CacheLen(args->pool->mp(), lcore.id());
  CHECK_EQ(moved, args->quota);
}

// "A alloc -> A free": one thread, one burst in and out of the pool. With a
// cache this is the cache-hit path; with cache 0 it is the backend path.
template <int kBackend>
void BM_MempoolLocal(benchmark::State &state) {
  const unsigned cache_size = static_cast<unsigned>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  CHECK_LE(batch, kMaxBatch);

  std::unique_ptr<BenchPool> pool =
      BenchPool::Create(kBackendNames[kBackend], cache_size);
  if (pool == nullptr) {
    state.SkipWithError((std::string("mempool ops backend unavailable: ") +
                         kBackendNames[kBackend])
                            .c_str());
    return;
  }

  const LcoreScope lcore(kConsumerWid);
  PinCurrentThread(g_consumer_cpu);
  CheckCacheInPlay(pool->mp(), cache_size);

  Packet *pkts[kMaxBatch];
  uint64_t items = 0;
  uint64_t failures = 0;

  // Thread CPU time is taken once around the whole timed loop, not per
  // iteration: two clock_gettime() calls inside a batch-1 iteration would
  // cost more than the allocation being measured.
  const uint64_t cpu0 = ThreadCpuNs();
  for (auto _ : state) {
    if (!pool->AllocBulk(pkts, batch, kPktLen)) {
      failures++;
      CHECK_LT(failures, kMaxAllocFailures) << "pool drained in the local case";
      continue;
    }
    Packet::Free(pkts, batch);
    items += batch;
  }
  const uint64_t cpu_ns = ThreadCpuNs() - cpu0;

  CHECK_GT(items, 0);
  // Every iteration allocates and frees the same packets, so at loop exit
  // nothing is left allocated. rte_mempool_avail_count() counts free objects
  // across backend + default caches, so a shortfall here means an object was
  // stranded in use -- a state that would silently shift every later
  // measurement.
  CHECK_EQ(rte_mempool_avail_count(pool->mp()), kPoolCapacity)
      << "objects left allocated after a balanced alloc/free loop";
  state.SetItemsProcessed(items);
  state.counters["cpu_ns_pkt"] = benchmark::Counter(
      static_cast<double>(cpu_ns) / static_cast<double>(items));
  state.counters["cache_len"] =
      benchmark::Counter(static_cast<double>(CacheLen(pool->mp(), lcore.id())));
  state.counters["alloc_failures"] =
      benchmark::Counter(static_cast<double>(failures));
  state.counters["cpu"] =
      benchmark::Counter(static_cast<double>(sched_getcpu()));
  state.counters["lcore"] = benchmark::Counter(static_cast<double>(lcore.id()));
}

// "A alloc -> B free" over the real handoff: the producer allocates bursts
// and enqueues them, the consumer dequeues and frees. The benchmark thread
// is the consumer, so Google Benchmark's per-thread accounting stays on real
// work; the epoch's wall clock is measured here regardless, because Google
// Benchmark's own rate is CPU-time based (see the file header).
template <int kBackend>
void BM_MempoolPipeline(benchmark::State &state) {
  const unsigned cache_size = static_cast<unsigned>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  CHECK_LE(batch, kMaxBatch);

  std::unique_ptr<BenchPool> pool =
      BenchPool::Create(kBackendNames[kBackend], cache_size);
  if (pool == nullptr) {
    state.SkipWithError((std::string("mempool ops backend unavailable: ") +
                         kBackendNames[kBackend])
                            .c_str());
    return;
  }

  rte_ring *ring = CreateRing();
  const LcoreScope lcore(kConsumerWid);
  PinCurrentThread(g_consumer_cpu);
  CheckCacheInPlay(pool->mp(), cache_size);

  Packet *pkts[kMaxBatch];
  uint64_t items_total = 0;
  uint64_t prod_cpu_total = 0;
  uint64_t cons_cpu_total = 0;
  uint64_t wall_total = 0;
  uint64_t prod_cache_total = 0;
  uint64_t cons_cache_total = 0;
  uint64_t epochs = 0;
  int prod_cpu_id = -1;

  for (auto _ : state) {
    state.PauseTiming();
    std::atomic<bool> start{false};
    ProducerArgs args = {pool.get(), ring,   batch, kEpochItems, cache_size,
                         lcore.id(), &start, 0,     0,           -1};
    std::thread producer(ProducerLoop, &args);
    state.ResumeTiming();

    start.store(true, std::memory_order_release);
    const uint64_t wall0 = NowNs();
    const uint64_t cpu0 = ThreadCpuNs();
    uint64_t consumed = 0;
    while (consumed < kEpochItems) {
      const unsigned got =
          rte_ring_sc_dequeue_burst(ring, reinterpret_cast<void **>(pkts),
                                    static_cast<unsigned>(batch), nullptr);
      if (got == 0) {
        continue;  // the consumer waits for the producer: real pipeline state
      }
      Packet::Free(pkts, got);
      consumed += got;
    }
    const uint64_t cons_cpu = ThreadCpuNs() - cpu0;
    const uint64_t wall = NowNs() - wall0;

    state.PauseTiming();
    producer.join();
    // Epoch boundary, outside the timed region: both workers are between
    // bursts, so this is the one moment the cache/ring split is stable and
    // readable. All objects must be free again (in backend or in a cache):
    // anything still in use would mean a packet was stranded, and every
    // later epoch would measure a shifted workload.
    CHECK_EQ(rte_mempool_avail_count(pool->mp()), kPoolCapacity)
        << "objects left allocated at a pipeline epoch boundary";
    prod_cache_total += args.cache_len;
    cons_cache_total += CacheLen(pool->mp(), lcore.id());
    epochs++;
    state.ResumeTiming();

    items_total += consumed;
    prod_cpu_total += args.cpu_ns;
    cons_cpu_total += cons_cpu;
    wall_total += wall;
    prod_cpu_id = args.cpu_id;
  }

  CHECK_GT(items_total, 0);
  CHECK_GT(wall_total, 0);
  state.SetItemsProcessed(items_total);
  state.counters["pkt_per_s"] = benchmark::Counter(
      static_cast<double>(items_total) * 1e9 / static_cast<double>(wall_total));
  state.counters["prod_busy"] = benchmark::Counter(
      static_cast<double>(prod_cpu_total) / static_cast<double>(wall_total));
  state.counters["cons_busy"] = benchmark::Counter(
      static_cast<double>(cons_cpu_total) / static_cast<double>(wall_total));
  state.counters["prod_cache"] = benchmark::Counter(
      static_cast<double>(prod_cache_total) / static_cast<double>(epochs));
  state.counters["cons_cache"] = benchmark::Counter(
      static_cast<double>(cons_cache_total) / static_cast<double>(epochs));
  state.counters["prod_cpu"] =
      benchmark::Counter(static_cast<double>(prod_cpu_id));
  state.counters["cons_cpu"] =
      benchmark::Counter(static_cast<double>(sched_getcpu()));
  state.counters["cons_lcore"] =
      benchmark::Counter(static_cast<double>(lcore.id()));

  DestroyRing(ring);
}

// One registration per backend, so the ops name is visible in the case name
// instead of an integer index, and each backend sweeps the same cache x
// batch grid.
#define REGISTER_BACKEND(backend, label)             \
  BENCHMARK_TEMPLATE(BM_MempoolLocal, backend)       \
      ->Name("BM_MempoolLocal/" label)               \
      ->ArgsProduct({{0, 32, 128, 512}, {1, 8, 32}}) \
      ->ArgNames({"cache_size", "batch"});           \
  BENCHMARK_TEMPLATE(BM_MempoolPipeline, backend)    \
      ->Name("BM_MempoolPipeline/" label)            \
      ->ArgsProduct({{0, 32, 128, 512}, {1, 8, 32}}) \
      ->ArgNames({"cache_size", "batch"});

REGISTER_BACKEND(kRingMpMc, "ring_mp_mc");
REGISTER_BACKEND(kRingMtRts, "ring_mt_rts");
REGISTER_BACKEND(kRingMtHts, "ring_mt_hts");
REGISTER_BACKEND(kStack, "stack");
REGISTER_BACKEND(kLfStack, "lf_stack");

}  // namespace

int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);

  // Strip this binary's own flags before Google Benchmark parses argv (it
  // errors on unrecognized arguments).
  int w = 1;
  for (int r = 1; r < argc; r++) {
    const std::string arg(argv[r]);
    if (arg.rfind("--pin=", 0) == 0) {
      const std::string value = arg.substr(6);
      if (value == "none") {
        g_pin_mode = PinMode::kNone;
      } else if (value == "smt") {
        g_pin_mode = PinMode::kSmt;
      } else if (value == "cores") {
        g_pin_mode = PinMode::kCores;
      } else if (value == "numa") {
        g_pin_mode = PinMode::kNuma;
      } else {
        LOG(FATAL) << "--pin=" << value << ": expected none|smt|cores|numa";
      }
    } else if (arg.rfind("--lcore_mode=", 0) == 0) {
      const std::string value = arg.substr(13);
      if (value == "bess") {
        g_lcore_mode = LcoreMode::kBess;
      } else if (value == "register") {
        g_lcore_mode = LcoreMode::kRegister;
      } else if (value == "none") {
        g_lcore_mode = LcoreMode::kNone;
      } else {
        LOG(FATAL) << "--lcore_mode=" << value
                   << ": expected bess|register|none";
      }
    } else {
      argv[w++] = argv[r];
    }
  }
  argc = w;

  // Same sandbox-safe EAL as pmd_bench/bessd -m 0: --no-huge, malloc-backed.
  // EAL is initialized before Google Benchmark threads exist, and before any
  // pool is created (the per-lcore caches are sized for the lcores known at
  // pool creation).
  bess::InitDpdk(0);

  ChoosePlacement();
  LOG(INFO) << "mempool_bench: lcore_mode=" << LcoreModeName(g_lcore_mode)
            << " pin=" << PinModeName(g_pin_mode) << " producer_cpu="
            << (g_producer_cpu < 0 ? std::string("scheduler")
                                   : std::to_string(g_producer_cpu))
            << " consumer_cpu="
            << (g_consumer_cpu < 0 ? std::string("scheduler")
                                   : std::to_string(g_consumer_cpu));

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
