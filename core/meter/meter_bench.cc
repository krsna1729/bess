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

// K5: what a BESS meter costs on top of the rte_meter check it delegates to.
//
//   single   -- one hot meter, one packet per call: raw rte_meter trTCM versus
//               a standalone MeterState (exclusive and uncontended shared). The gap is the
//               BESS wrapper: the monotonic clamp, the algorithm switch and,
//               for shared meters, an uncontended spinlock.
//   batch    -- MeterSet::CheckBatch over 32-packet batches with ids drawn
//               across the whole set, against a raw array of rte_meter
//               contexts indexed the same way, with and without the same
//               batch prefetch CheckBatch does. Meter count sweeps the state
//               footprint (64 B per meter) from L1 to DRAM.
//   threads  -- one shared meter hammered by 1..4 threads, against one
//               exclusive meter per thread: the price of kShared under
//               contention, which is why exclusive is the default to reach for.
//
// Time is synthetic (a fixed cycle step per packet or batch) so rows are
// reproducible and refill arithmetic is exercised; reading the TSC is a
// separate, constant cost callers pay once per batch.

#include <benchmark/benchmark.h>
#include <sched.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "meter/meter.h"
#include "meter/meter_set.h"

namespace {

using bess::meter::MeterColor;
using bess::meter::MeterId;
using bess::meter::MeterProfile;
using bess::meter::MeterSet;
using bess::meter::MeterSetBuilder;
using bess::meter::MeterSharing;
using bess::meter::MeterState;
using bess::meter::MeterStatePtr;
using bess::meter::TrTcmSpec;

// Roughly 1 Gb/s committed, 2 Gb/s peak, 64 KiB bursts.
constexpr TrTcmSpec kSpec{125'000'000, 65536, 250'000'000, 65536};
constexpr uint64_t kCyclesPerPacket = 100;
constexpr size_t kBatch = 32;

rte_meter_trtcm_params RteParams() {
  return rte_meter_trtcm_params{kSpec.committed_rate, kSpec.peak_rate,
                                kSpec.committed_burst, kSpec.peak_burst};
}

const MeterProfile &Profile() {
  static const MeterProfile profile = MeterProfile::Create(kSpec).value();
  return profile;
}

std::vector<uint32_t> PacketLengths(size_t n) {
  std::mt19937 rng(0x6b35);
  std::uniform_int_distribution<uint32_t> length(64, 1500);
  std::vector<uint32_t> lengths(n);
  for (auto &l : lengths) {
    l = length(rng);
  }
  return lengths;
}

// -- single meter ------------------------------------------------------------

void BM_SingleRawRte(benchmark::State &state) {
  Profile();  // brings up the EAL for the TSC frequency
  rte_meter_trtcm_params params = RteParams();
  rte_meter_trtcm_profile profile;
  rte_meter_trtcm meter;
  rte_meter_trtcm_profile_config(&profile, &params);
  rte_meter_trtcm_config(&meter, &profile);
  const std::vector<uint32_t> lengths = PacketLengths(1024);

  uint64_t now = rte_get_tsc_cycles();
  size_t i = 0;
  for (auto _ : state) {
    now += kCyclesPerPacket;
    benchmark::DoNotOptimize(rte_meter_trtcm_color_blind_check(
        &meter, &profile, now, lengths[i++ & 1023]));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SingleRawRte);

void BM_SingleState(benchmark::State &state) {
  const auto sharing = static_cast<MeterSharing>(state.range(0));
  const MeterProfile &profile = Profile();
  MeterStatePtr meter = MeterState::Create(profile, sharing).value();
  const std::vector<uint32_t> lengths = PacketLengths(1024);

  uint64_t now = rte_get_tsc_cycles();
  size_t i = 0;
  for (auto _ : state) {
    now += kCyclesPerPacket;
    benchmark::DoNotOptimize(meter->Check(now, lengths[i++ & 1023]));
  }
  state.SetItemsProcessed(state.iterations());
  state.SetLabel(sharing == MeterSharing::kShared ? "shared" : "exclusive");
}
BENCHMARK(BM_SingleState)->Arg(0)->Arg(1);

// -- batches across many meters ----------------------------------------------

// Ids spread over the whole set: at least one pass over every meter, so the
// state footprint of a row is the footprint of its set.
std::vector<uint32_t> BatchIds(size_t meters) {
  std::mt19937 rng(0x4b35);
  std::uniform_int_distribution<uint32_t> id(1, static_cast<uint32_t>(meters));
  std::vector<uint32_t> ids(std::max<size_t>(meters, 32 * kBatch));
  ids.resize(ids.size() - ids.size() % kBatch);
  for (auto &v : ids) {
    v = id(rng);
  }
  return ids;
}

template <bool kPrefetch>
void BM_BatchRawRte(benchmark::State &state) {
  const size_t meters = static_cast<size_t>(state.range(0));
  Profile();
  rte_meter_trtcm_params params = RteParams();
  rte_meter_trtcm_profile profile;
  rte_meter_trtcm_profile_config(&profile, &params);
  std::vector<rte_meter_trtcm> contexts(meters + 1);
  for (auto &c : contexts) {
    rte_meter_trtcm_config(&c, &profile);
  }
  const std::vector<uint32_t> ids = BatchIds(meters);
  const std::vector<uint32_t> lengths = PacketLengths(ids.size());

  uint64_t now = rte_get_tsc_cycles();
  size_t offset = 0;
  std::array<rte_color, kBatch> colors;
  for (auto _ : state) {
    now += kCyclesPerPacket * kBatch;
    if constexpr (kPrefetch) {
      for (size_t i = 0; i < kBatch; i++) {
        __builtin_prefetch(&contexts[ids[offset + i]], 1);
      }
    }
    for (size_t i = 0; i < kBatch; i++) {
      colors[i] = rte_meter_trtcm_color_blind_check(
          &contexts[ids[offset + i]], &profile, now, lengths[offset + i]);
    }
    benchmark::DoNotOptimize(colors);
    offset = (offset + kBatch) % ids.size();
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
  state.counters["state_bytes"] =
      static_cast<double>(meters * sizeof(rte_meter_trtcm));
}
BENCHMARK(BM_BatchRawRte<false>)
    ->Name("BM_BatchRawRte")
    ->Arg(1)->Arg(1024)->Arg(16384)->Arg(262144)->Arg(1048576);
BENCHMARK(BM_BatchRawRte<true>)
    ->Name("BM_BatchRawRtePrefetch")
    ->Arg(1)->Arg(1024)->Arg(16384)->Arg(262144)->Arg(1048576);

void BM_BatchMeterSet(benchmark::State &state) {
  const size_t meters = static_cast<size_t>(state.range(0));
  const auto sharing = static_cast<MeterSharing>(state.range(1));
  MeterSetBuilder builder(meters);
  for (size_t id = 1; id <= meters; id++) {
    if (!builder.Add(MeterId(static_cast<uint32_t>(id)), kSpec, sharing)) {
      state.SkipWithError("meter allocation failed");
      return;
    }
  }
  std::unique_ptr<const MeterSet> set = builder.Build();

  const std::vector<uint32_t> raw_ids = BatchIds(meters);
  std::vector<MeterId> ids;
  ids.reserve(raw_ids.size());
  for (uint32_t id : raw_ids) {
    ids.push_back(MeterId(id));
  }
  const std::vector<uint32_t> lengths = PacketLengths(ids.size());

  uint64_t now = rte_get_tsc_cycles();
  size_t offset = 0;
  std::array<MeterColor, kBatch> colors;
  for (auto _ : state) {
    now += kCyclesPerPacket * kBatch;
    benchmark::DoNotOptimize(set->CheckBatch(
        std::span(ids).subspan(offset, kBatch),
        std::span(lengths).subspan(offset, kBatch), colors, now));
    benchmark::DoNotOptimize(colors);
    offset = (offset + kBatch) % ids.size();
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
  state.counters["state_bytes"] =
      static_cast<double>(meters * sizeof(MeterState));
  state.SetLabel(sharing == MeterSharing::kShared ? "shared" : "exclusive");
}
BENCHMARK(BM_BatchMeterSet)
    ->ArgsProduct({{1, 1024, 16384, 262144, 1048576}, {0, 1}});

// -- contention --------------------------------------------------------------

// Each benchmark thread pins itself to its own CPU of the mask the process was
// launched with. Two things would otherwise put every "parallel" thread on
// one CPU: rte_eal_init() pins the calling thread to its main lcore and later
// threads inherit that mask, and an isolated cpuset partition (how these rows
// are meant to be run, via omarchy-benchmark --isolate) does no load
// balancing. The launch mask is captured by a static initializer, which runs
// before main and so before the EAL.
cpu_set_t LaunchAffinity() {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  return set;
}
const cpu_set_t kLaunchAffinity = LaunchAffinity();

void PinBenchmarkThread(const benchmark::State &state) {
  int seen = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
    if (!CPU_ISSET(cpu, &kLaunchAffinity)) {
      continue;
    }
    if (seen++ == state.thread_index() % CPU_COUNT(&kLaunchAffinity)) {
      cpu_set_t one;
      CPU_ZERO(&one);
      CPU_SET(cpu, &one);
      sched_setaffinity(0, sizeof(one), &one);
      return;
    }
  }
}

MeterState *SharedMeter() {
  static MeterStatePtr meter =
      MeterState::Create(Profile(), MeterSharing::kShared).value();
  return meter.get();
}

void BM_ThreadsOneSharedMeter(benchmark::State &state) {
  PinBenchmarkThread(state);
  MeterState *meter = SharedMeter();
  const std::vector<uint32_t> lengths = PacketLengths(1024);
  size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(meter->Check(rte_get_tsc_cycles(),
                                          lengths[i++ & 1023]));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ThreadsOneSharedMeter)->ThreadRange(1, 4)->UseRealTime();

void BM_ThreadsExclusiveMeterEach(benchmark::State &state) {
  PinBenchmarkThread(state);
  const MeterProfile &profile = Profile();
  MeterStatePtr meter =
      MeterState::Create(profile, MeterSharing::kWorkerExclusive).value();
  const std::vector<uint32_t> lengths = PacketLengths(1024);
  size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(meter->Check(rte_get_tsc_cycles(),
                                          lengths[i++ & 1023]));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ThreadsExclusiveMeterEach)->ThreadRange(1, 4)->UseRealTime();

}  // namespace
