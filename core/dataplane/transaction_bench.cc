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

// Transactions per second through the G1.2b engine (Decision D-021), in
// process, for a session-shaped change set: two meters, two actions naming
// them, two exact rules naming the actions -- created in one transaction and
// removed in another. MODERNIZATION.md section 14.5's target is >= 100K
// sessions/s (establish + release); this measures it without RPC.

#include <benchmark/benchmark.h>

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "classifier/exact_rule_resource.h"
#include "control/runtime_state.h"
#include "dataplane/slot_resource.h"
#include "dataplane/strong_id.h"
#include "dataplane/transaction_engine.h"

namespace {

// The CPUs this process may use, captured before DPDK's EAL starts (it pins
// the main thread to one core, and threads created later inherit that).
const cpu_set_t kInitialCpus = [] {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  return set;
}();

// Puts the calling thread on an allowed CPU other than `avoid`, if any.
void PinAwayFrom(int avoid) {
  for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
    if (cpu != avoid && CPU_ISSET(cpu, &kInitialCpus)) {
      cpu_set_t one;
      CPU_ZERO(&one);
      CPU_SET(cpu, &one);
      pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
      return;
    }
  }
}

using bess::classifier::ConcurrentExactTable;
using bess::classifier::ExactRuleResource;
using namespace bess::dataplane;

struct MeterTag;
struct ActionTag;
using MeterId = StrongId<MeterTag, uint32_t>;
using ActionId = StrongId<ActionTag, uint32_t>;
struct Meter {
  uint64_t cir, cbs;
};
struct Action {
  uint16_t gate;
  MeterId meter;
};

void BM_SessionEstablishRelease(benchmark::State &state) {
  const uint32_t sessions = static_cast<uint32_t>(state.range(0));
  bess::rcu::RcuDomain &domain = bess::control::runtime().rcu();
  auto table = *ConcurrentExactTable::Create(
      8, ConcurrentExactTable::CapacityFor(sessions * 2 + 64), domain);
  SlotTable<MeterId, Meter> meters(sessions * 2 + 2);
  SlotTable<ActionId, Action> actions(sessions * 2 + 2);
  SlotResource<MeterId, Meter> meters_res("meters", meters);
  SlotResource<ActionId, Action> actions_res(
      "actions", actions,
      [](const Action &a) {
        return std::vector<Reference>{{"meters", EncodeKey(a.meter)}};
      },
      {"meters"});
  ExactRuleResource rules_res(
      "rules", *table,
      [](uint64_t v) {
        return std::vector<Reference>{
            {"actions", EncodeKey(ActionId(static_cast<uint32_t>(v)))}};
      },
      {"actions"});
  TransactionEngine engine(domain);
  if (!engine.Register(&meters_res)) {
    state.SkipWithError("resource registration failed");
    return;
  }
  if (!engine.Register(&actions_res)) {
    state.SkipWithError("resource registration failed");
    return;
  }
  if (!engine.Register(&rules_res)) {
    state.SkipWithError("resource registration failed");
    return;
  }

  // Sessions cycle through the id space; ids freed by a release are reused
  // only after their grace period (no worker is online here, so at once).
  uint32_t s = 0;
  std::vector<Op> establish(6), release(6);
  for (auto _ : state) {
    const uint32_t id = 1 + 2 * (s % sessions);
    const uint64_t key = s % sessions;
    establish[0] = Op::Upsert("meters", EncodeKey(MeterId(id)),
                              std::any(Meter{1000000, 65536}));
    establish[1] = Op::Upsert("meters", EncodeKey(MeterId(id + 1)),
                              std::any(Meter{2000000, 65536}));
    establish[2] = Op::Upsert("actions", EncodeKey(ActionId(id)),
                              std::any(Action{1, MeterId(id)}));
    establish[3] = Op::Upsert("actions", EncodeKey(ActionId(id + 1)),
                              std::any(Action{2, MeterId(id + 1)}));
    establish[4] = Op::Upsert("rules", EncodeKey(key * 2),
                              std::any(uint64_t{id}));
    establish[5] = Op::Upsert("rules", EncodeKey(key * 2 + 1),
                              std::any(uint64_t{id + 1}));
    if (engine.Apply(establish).outcome !=
        TransactionEngine::Outcome::kApplied) {
      state.SkipWithError("establish rejected");
      break;
    }
    release[0] = Op::Erase("rules", EncodeKey(key * 2));
    release[1] = Op::Erase("rules", EncodeKey(key * 2 + 1));
    release[2] = Op::Erase("actions", EncodeKey(ActionId(id)));
    release[3] = Op::Erase("actions", EncodeKey(ActionId(id + 1)));
    release[4] = Op::Erase("meters", EncodeKey(MeterId(id)));
    release[5] = Op::Erase("meters", EncodeKey(MeterId(id + 1)));
    if (engine.Apply(release).outcome != TransactionEngine::Outcome::kApplied) {
      state.SkipWithError("release rejected");
      break;
    }
    s++;
  }
  state.SetItemsProcessed(state.iterations());  // sessions (establish+release)
  domain.Drain();
}
BENCHMARK(BM_SessionEstablishRelease)->Arg(1024)->Arg(65536);

// One rule added and removed: straight into the table (what a module
// command does today), against the same through the engine as two
// single-operation transactions. The difference is what a module pays per
// command if its commands go through the engine.
void BM_SingleRule(benchmark::State &state) {
  const bool via_engine = state.range(0) != 0;
  bess::rcu::RcuDomain &domain = bess::control::runtime().rcu();
  auto table = *ConcurrentExactTable::Create(
      8, ConcurrentExactTable::CapacityFor(65536), domain);
  ExactRuleResource rules_res("rules", *table);
  TransactionEngine engine(domain);
  if (!engine.Register(&rules_res)) {
    state.SkipWithError("resource registration failed");
    return;
  }
  uint64_t k = 0;
  std::vector<Op> add(1), del(1);
  for (auto _ : state) {
    const uint64_t key = k++ % 65536;
    const auto bytes = EncodeKey(key);
    if (via_engine) {
      add[0] = Op::Upsert("rules", bytes, std::any(uint64_t{7}));
      del[0] = Op::Erase("rules", bytes);
      engine.Apply(add);
      engine.Apply(del);
    } else {
      const bess::classifier::ConstBytes b(
          reinterpret_cast<const bess::classifier::Byte *>(bytes.data()), 8);
      table->Upsert(b, 7);
      table->Erase(b);
    }
  }
  state.SetItemsProcessed(state.iterations() * 2);  // operations
  state.SetLabel(via_engine ? "engine" : "direct");
  domain.Drain();
}
BENCHMARK(BM_SingleRule)->Arg(0)->Arg(1);

// The session benchmark with a reader online, reporting quiescence the way a
// worker does (every few microseconds): grace periods now take real time, so
// retirement and the removal cascade run behind the transactions, and ids
// freed by a release come back only after their cascade.
void BM_SessionWithOnlineReader(benchmark::State &state) {
  constexpr uint32_t kSessions = 65536;
  bess::rcu::RcuDomain &domain = bess::control::runtime().rcu();
  constexpr bess::rcu::ReaderId kReader = 20;
  (void)domain.Register(kReader);
  std::atomic<bool> stop{false};
  const int main_cpu = sched_getcpu();
  std::thread reader([&] {
    // A worker runs on its own core: keep the reader off the main thread's.
    PinAwayFrom(main_cpu);
    domain.Online(kReader);
    while (!stop.load(std::memory_order_relaxed)) {
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::microseconds(10);
      while (std::chrono::steady_clock::now() < until) {
      }
      domain.Quiescent(kReader);
    }
    domain.Offline(kReader);
  });

  auto table = *ConcurrentExactTable::Create(
      8, ConcurrentExactTable::CapacityFor(kSessions * 2 + 64), domain);
  SlotTable<MeterId, Meter> meters(kSessions * 2 + 2);
  SlotTable<ActionId, Action> actions(kSessions * 2 + 2);
  SlotResource<MeterId, Meter> meters_res("meters", meters);
  SlotResource<ActionId, Action> actions_res(
      "actions", actions,
      [](const Action &a) {
        return std::vector<Reference>{{"meters", EncodeKey(a.meter)}};
      },
      {"meters"});
  ExactRuleResource rules_res(
      "rules", *table,
      [](uint64_t v) {
        return std::vector<Reference>{
            {"actions", EncodeKey(ActionId(static_cast<uint32_t>(v)))}};
      },
      {"actions"});
  TransactionEngine engine(domain);
  if (!engine.Register(&meters_res)) {
    state.SkipWithError("resource registration failed");
    return;
  }
  if (!engine.Register(&actions_res)) {
    state.SkipWithError("resource registration failed");
    return;
  }
  if (!engine.Register(&rules_res)) {
    state.SkipWithError("resource registration failed");
    return;
  }

  uint32_t s = 0;
  uint64_t retries = 0;
  std::vector<Op> establish(6), release(6);
  for (auto _ : state) {
    const uint32_t id = 1 + 2 * (s % kSessions);
    const uint64_t key = s % kSessions;
    establish[0] = Op::Upsert("meters", EncodeKey(MeterId(id)),
                              std::any(Meter{1000000, 65536}));
    establish[1] = Op::Upsert("meters", EncodeKey(MeterId(id + 1)),
                              std::any(Meter{2000000, 65536}));
    establish[2] = Op::Upsert("actions", EncodeKey(ActionId(id)),
                              std::any(Action{1, MeterId(id)}));
    establish[3] = Op::Upsert("actions", EncodeKey(ActionId(id + 1)),
                              std::any(Action{2, MeterId(id + 1)}));
    establish[4] = Op::Upsert("rules", EncodeKey(key * 2),
                              std::any(uint64_t{id}));
    establish[5] = Op::Upsert("rules", EncodeKey(key * 2 + 1),
                              std::any(uint64_t{id + 1}));
    while (engine.Apply(establish).outcome !=
           TransactionEngine::Outcome::kApplied) {
      retries++;  // ids of this slot still in their removal cascade
    }
    release[0] = Op::Erase("rules", EncodeKey(key * 2));
    release[1] = Op::Erase("rules", EncodeKey(key * 2 + 1));
    release[2] = Op::Erase("actions", EncodeKey(ActionId(id)));
    release[3] = Op::Erase("actions", EncodeKey(ActionId(id + 1)));
    release[4] = Op::Erase("meters", EncodeKey(MeterId(id)));
    release[5] = Op::Erase("meters", EncodeKey(MeterId(id + 1)));
    engine.Apply(release);
    s++;
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["retries"] = static_cast<double>(retries);
  const auto stats = domain.Stats();
  state.counters["grace_periods"] = static_cast<double>(
      stats.grace_periods_started);
  state.counters["pending_at_end"] = static_cast<double>(
      stats.pending_retired_objects);
  stop = true;
  reader.join();
  while (engine.ReclaimRetired() != 0) {
  }
  domain.Unregister(kReader);
  domain.Drain();
}
BENCHMARK(BM_SessionWithOnlineReader)->UseRealTime();

}  // namespace
