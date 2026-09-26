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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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


// How long is a QSBR grace period under real workers? Input for
// docs/decisions.md D-010 (spare slots >= update rate x grace period) and for
// anything that waits on one (RcuPtr retirement, rte_hash/rte_lpm reclaim).
//
// Real BESS workers run real pipelines (Source -> Bypass -> Sink, one per
// worker); the control thread repeatedly starts a grace period and spins
// until every online worker has passed a quiescent point, then waits a short
// random gap. Reports the distribution in microseconds.
//
// Usage: grace_period_bench <control_cpu> <worker_cpu>[,<worker_cpu>...]
//   Scenarios run in order; those needing more worker CPUs than given are
//   skipped. With no arguments (as the Meson benchmark suite runs it): a
//   short smoke run, one worker on an allowed CPU, default cadence only. Pin with an isolation wrapper, e.g.
//   omarchy-benchmark --cpu 2,4,6,8,10 --isolate -- grace_period_bench 2 4,6,8,10

#include <rte_cycles.h>
#include <rte_pause.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "control/runtime_state.h"
#include "dpdk.h"
#include "module.h"
#include "module_graph.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "port.h"
#include "rcu/rcu_domain.h"
#include "worker.h"

// Counts the packets it receives and frees them: the throughput probe.
std::atomic<uint64_t> g_counted{0};
class CountSink final : public Module {
 public:
  static const gate_idx_t kNumOGates = 0;
  static const Commands cmds;
  CommandResponse Init(const bess::pb::EmptyArg &) { return CommandSuccess(); }
  void ProcessBatch(Context *, bess::PacketBatch *batch) override {
    g_counted.fetch_add(static_cast<uint64_t>(batch->cnt()),
                        std::memory_order_relaxed);
    bess::PacketFreeBatch(batch);
  }
};
const Commands CountSink::cmds = {};
ADD_MODULE(CountSink, "count_sink", "counts and frees packets (bench)")

namespace {

// Smoke mode samples briefly; a full run samples ~2 s per case.
uint64_t g_sample_seconds_x10 = 20;

struct Scenario {
  const char *name;
  int workers;              // pipelines, one per worker
  bool pipelines;           // false: workers with no tasks at all
  uint32_t cycles_per_batch;  // Bypass busy-work per batch
  bool share_one_cpu;       // all workers on the first worker CPU
  // Throughput mode: measure packets/s through a counting sink while the
  // control thread starts `churn_per_sec` grace periods per second (what a
  // table writer deleting that many keys does), instead of grace periods.
  bool throughput = false;
  uint64_t churn_per_sec = 0;
};

Module *Create(const char *mclass, const std::string &name,
               const google::protobuf::Message &arg) {
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) std::abort();
  pb_error_t err;
  Module *m = ModuleGraph::CreateModule(
      ModuleBuilder::all_module_builders().at(mclass), name, packed, &err);
  if (m == nullptr || err.code() != 0) {
    std::fprintf(stderr, "create %s: %s\n", name.c_str(),
                 err.errmsg().c_str());
    std::abort();
  }
  return m;
}

void Measure(const Scenario &s, const std::vector<int> &cpus,
             const char *cadence) {
  if (s.workers > static_cast<int>(cpus.size())) {
    std::printf("| %s | %s | skipped: needs %d worker CPUs |\n", s.name,
                cadence, s.workers);
    return;
  }
  for (int w = 0; w < s.workers; w++) {
    launch_worker(w, s.share_one_cpu ? cpus[0] : cpus[w]);
  }
  if (s.pipelines) {
    for (int w = 0; w < s.workers; w++) {
      bess::pb::SourceArg src_arg;
      src_arg.set_pkt_size(64);
      bess::pb::BypassArg bypass_arg;
      bypass_arg.set_cycles_per_batch(s.cycles_per_batch);
      bess::pb::SinkArg sink_arg;
      bess::pb::EmptyArg empty;
      const std::string n = std::to_string(w);
      Module *src = Create("Source", "src" + n, src_arg);
      Module *bp = Create("Bypass", "bp" + n, bypass_arg);
      Module *sink = s.throughput ? Create("CountSink", "sink" + n, empty)
                                  : Create("Sink", "sink" + n, sink_arg);
      ModuleGraph::ConnectModules(src, 0, bp, 0);
      ModuleGraph::ConnectModules(bp, 0, sink, 0);
    }
    attach_orphans();
  }
  resume_all_workers();

  bess::rcu::RcuDomain &rcu = bess::control::runtime().rcu();
  if (s.throughput) {
    const uint64_t hz = rte_get_tsc_hz();
    const uint64_t warm = rte_rdtsc() + hz / 5;
    while (rte_rdtsc() < warm) rte_pause();
    const uint64_t start_count = g_counted.load();
    const uint64_t t0 = rte_rdtsc(), end = t0 + hz;
    const uint64_t step = s.churn_per_sec ? hz / s.churn_per_sec : 0;
    uint64_t next = t0;
    while (rte_rdtsc() < end) {
      if (step && rte_rdtsc() >= next) {
        rcu.StartGracePeriod();
        next += step;
      }
      rte_pause();
    }
    const double mpps = (g_counted.load() - start_count) /
                        ((rte_rdtsc() - t0) / static_cast<double>(hz)) / 1e6;
    pause_all_workers();
    {
      WorkerPauser pauser;
      ModuleGraph::DestroyAllModules();
    }
    destroy_all_workers();
    rcu.Drain();
    std::printf("| %s | %s | %.2f Mpps |\n", s.name, cadence, mpps);
    return;
  }
  const double us_per_cycle = 1e6 / static_cast<double>(rte_get_tsc_hz());
  std::mt19937 rng(42);
  std::vector<double> samples;
  // Warm up, then sample for ~2 s or 20000 grace periods -- but at least 30
  // samples, however long grace periods are.
  const uint64_t end =
      rte_rdtsc() + g_sample_seconds_x10 * rte_get_tsc_hz() / 10;
  for (int i = 0; samples.size() < 20000 &&
                  (rte_rdtsc() < end || samples.size() < 30);
       i++) {
    const uint64_t t0 = rte_rdtsc();
    const bess::rcu::GracePeriod token = rcu.StartGracePeriod();
    while (!rcu.IsComplete(token)) {
      rte_pause();
    }
    const double us = (rte_rdtsc() - t0) * us_per_cycle;
    if (i >= 3) samples.push_back(us);
    // A random gap so sampling does not lock onto the scheduler's rhythm.
    const uint64_t gap = rte_rdtsc() + (rng() % 50 + 10) *
                                           rte_get_tsc_hz() / 1000000;
    while (rte_rdtsc() < gap) rte_pause();
  }

  pause_all_workers();
  {
    WorkerPauser pauser;
    ModuleGraph::DestroyAllModules();
  }
  destroy_all_workers();
  rcu.Drain();

  std::sort(samples.begin(), samples.end());
  const auto q = [&](double p) {
    return samples[std::min(samples.size() - 1,
                            static_cast<size_t>(p * samples.size()))];
  };
  std::printf("| %s | %s | %zu | %.2f | %.2f | %.2f | %.2f | %.2f |\n",
              s.name, cadence, samples.size(), q(0.50), q(0.95), q(0.99),
              q(0.999), samples.back());
}

}  // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const bool smoke = argc < 3;
  std::vector<int> cpus;
  std::string control = argc > 1 ? argv[1] : "";
  std::string workers = argc > 2 ? argv[2] : "";
  if (smoke) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    sched_getaffinity(0, sizeof(allowed), &allowed);
    for (int c = 0; c < CPU_SETSIZE && cpus.empty(); c++) {
      if (CPU_ISSET(c, &allowed)) cpus.push_back(c);
    }
    control = workers = std::to_string(cpus[0]);
    g_sample_seconds_x10 = 2;
  } else {
    std::stringstream list(workers);
    for (std::string c; std::getline(list, c, ',');) {
      cpus.push_back(std::stoi(c));
    }
  }

  bess::InitDpdk();
  bess::PacketPool::CreateDefaultPools(32767);
  PortBuilder::InitDrivers();
  // The control thread measures from its own CPU.
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(std::stoi(control), &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);

  const Scenario scenarios[] = {
      {"1 idle worker (no tasks)", 1, false, 0, false},
      {"1 worker, Source->Bypass->Sink", 1, true, 0, false},
      {"4 workers, one pipeline each", 4, true, 0, false},
      {"1 worker, Bypass 10K cycles/batch", 1, true, 10000, false},
      {"1 worker, Bypass 1M cycles/batch", 1, true, 1000000, false},
      {"2 workers sharing one CPU", 2, true, 0, true},
  };
  const Scenario throughput[] = {
      {"1 worker, no grace periods started", 1, true, 0, false, true, 0},
      {"1 worker, 1M grace periods/s started", 1, true, 0, false, true,
       1000000},
      {"4 workers, 1M grace periods/s started", 4, true, 0, false, true,
       1000000},
  };
  // Quiescent cadences (Decision D-012): the pre-D-012 cadence, every round,
  // and time-based intervals. Read by each worker's scheduler at launch.
  const char *cadences[] = {"rounds", "0", "2", "10"};
  std::printf("## QSBR grace period (us), control CPU %s, worker CPUs %s\n\n"
              "| scenario | quiescent every | samples | p50 | p95 | p99 | "
              "p99.9 | max |\n|---|---|---|---|---|---|---|---|\n",
              control.c_str(), workers.c_str());
  if (smoke) {
    Measure(scenarios[1], cpus, "10");
    return 0;
  }
  for (const Scenario &s : scenarios) {
    for (const char *c : cadences) {
      setenv("BESS_QUIESCENT_INTERVAL_US", c, 1);
      Measure(s, cpus, c);
    }
  }
  std::printf("\n## Throughput (64-byte packets, Source->Bypass->count sink,"
              " per run)\n\n| scenario | quiescent every | total |\n"
              "|---|---|---|\n");
  for (const Scenario &s : throughput) {
    for (const char *c : cadences) {
      setenv("BESS_QUIESCENT_INTERVAL_US", c, 1);
      Measure(s, cpus, c);
    }
  }
  return 0;
}
