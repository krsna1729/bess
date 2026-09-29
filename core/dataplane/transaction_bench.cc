// SPDX-License-Identifier: BSD-3-Clause

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
#include <random>
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

// Scaling: packet-path lookups on several workers while transactions change
// the tables. Each reader, on its own CPU, resolves batches of 32 keys the way
// a pipeline would -- rte_hash rule lookup (pending keys dropped), then the
// action slot, then the meter slot -- and reports quiescence after each
// batch. Half of a 65K-session population is live, so batches mix hits and
// misses. The writer (the benchmark thread) replaces sessions at a paced rate
// (0 = no transactions, the baseline) or as fast as it can (-1): each
// iteration establishes one session and releases the oldest.
//
// Reported: lookups per second per reader and in total (the cost of
// transactions to the packet path) and sessions per second achieved.
void BM_LookupsUnderTransactions(benchmark::State &state) {
  const int readers = static_cast<int>(state.range(0));
  const int64_t rate = state.range(1);  // sessions/s; 0 none; -1 max
  constexpr uint32_t kSessions = 65536;
  bess::rcu::RcuDomain &domain = bess::control::runtime().rcu();

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
  if (!engine.Register(&meters_res) || !engine.Register(&actions_res) ||
      !engine.Register(&rules_res)) {
    state.SkipWithError("resource registration failed");
    return;
  }

  // Session s: rule keys 2s and 2s+1, actions and meters at slot s+1 and
  // s+1+kSessions (the two directions).
  auto establish = [&](uint32_t s) {
    const uint32_t up = s + 1, down = s + 1 + kSessions;
    return std::vector<Op>{
        Op::Upsert("meters", EncodeKey(MeterId(up)), std::any(Meter{1, 1})),
        Op::Upsert("meters", EncodeKey(MeterId(down)), std::any(Meter{2, 1})),
        Op::Upsert("actions", EncodeKey(ActionId(up)),
                   std::any(Action{1, MeterId(up)})),
        Op::Upsert("actions", EncodeKey(ActionId(down)),
                   std::any(Action{2, MeterId(down)})),
        Op::Upsert("rules", EncodeKey(uint64_t{2} * s), std::any(uint64_t{up})),
        Op::Upsert("rules", EncodeKey(uint64_t{2} * s + 1),
                   std::any(uint64_t{down}))};
  };
  auto release = [&](uint32_t s) {
    const uint32_t up = s + 1, down = s + 1 + kSessions;
    return std::vector<Op>{
        Op::Erase("rules", EncodeKey(uint64_t{2} * s)),
        Op::Erase("rules", EncodeKey(uint64_t{2} * s + 1)),
        Op::Erase("actions", EncodeKey(ActionId(up))),
        Op::Erase("actions", EncodeKey(ActionId(down))),
        Op::Erase("meters", EncodeKey(MeterId(up))),
        Op::Erase("meters", EncodeKey(MeterId(down)))};
  };
  // Live sessions: a window [oldest, next) of kSessions / 2, moving forward.
  uint32_t oldest = 0, next = 0;
  for (; next < kSessions / 2; next++) {
    if (engine.Apply(establish(next)).outcome !=
        TransactionEngine::Outcome::kApplied) {
      state.SkipWithError("populate failed");
      return;
    }
  }

  std::atomic<bool> stop{false};
  std::vector<std::atomic<uint64_t>> lookups(readers);
  std::vector<std::thread> threads;
  const int main_cpu = sched_getcpu();
  int cpu = 0;
  for (int r = 0; r < readers; r++) {
    // The next allowed CPU that is not the writer's.
    while (cpu < CPU_SETSIZE &&
           (cpu == main_cpu || !CPU_ISSET(cpu, &kInitialCpus))) {
      cpu++;
    }
    const int reader_cpu = cpu < CPU_SETSIZE ? cpu++ : -1;
    const bess::rcu::ReaderId id = static_cast<bess::rcu::ReaderId>(20 + r);
    (void)domain.Register(id);
    lookups[r] = 0;
    threads.emplace_back([&, r, id, reader_cpu] {
      if (reader_cpu >= 0) {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(reader_cpu, &one);
        pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
      }
      domain.Online(id);
      std::mt19937_64 rng(1234 + r);
      uint64_t keys[32], values[32], local = 0, sink = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        for (auto &k : keys) {
          k = rng() % (uint64_t{2} * kSessions);
        }
        const uint64_t hits = ExactRuleResource::VisibleHits(
            table->LookupBatch(
                bess::classifier::ConstBytes(
                    reinterpret_cast<const bess::classifier::Byte *>(keys),
                    sizeof(keys)),
                8, values, 32),
            values);
        for (uint64_t m = hits; m != 0; m &= m - 1) {
          const int i = __builtin_ctzll(m);
          if (const Action *a =
                  actions.Lookup(ActionId(static_cast<uint32_t>(values[i])))) {
            if (const Meter *meter = meters.Lookup(a->meter)) {
              sink += meter->cir;
            }
          }
        }
        local += 32;
        domain.Quiescent(id);
      }
      benchmark::DoNotOptimize(sink);
      lookups[r] = local;
      domain.Offline(id);
    });
  }

  uint64_t busy = 0, sessions = 0;
  const auto start = std::chrono::steady_clock::now();
  auto due = start;
  for (auto _ : state) {
    if (rate == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (rate > 0) {
      due += std::chrono::nanoseconds(1000000000 / rate);
      while (std::chrono::steady_clock::now() < due) {
      }
    }
    // Establish the next session, release the oldest; kBusy or an id still
    // retiring means readers are behind: try again.
    while (engine.Apply(establish(next % kSessions)).outcome !=
           TransactionEngine::Outcome::kApplied) {
      busy++;
    }
    while (engine.Apply(release(oldest % kSessions)).outcome !=
           TransactionEngine::Outcome::kApplied) {
      busy++;
    }
    next++;
    oldest++;
    sessions++;
  }
  const double seconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();
  stop = true;
  for (auto &t : threads) {
    t.join();
  }
  uint64_t total = 0;
  for (auto &l : lookups) {
    total += l;
  }
  state.counters["Mlookups_per_reader"] =
      static_cast<double>(total) / readers / seconds / 1e6;
  state.counters["Mlookups_total"] = static_cast<double>(total) / seconds / 1e6;
  state.counters["sessions_per_s"] = static_cast<double>(sessions) / seconds;
  state.counters["busy_retries"] = static_cast<double>(busy);
  for (int r = 0; r < readers; r++) {
    domain.Unregister(static_cast<bess::rcu::ReaderId>(20 + r));
  }
  while (engine.ReclaimRetired() != 0) {
  }
  domain.Drain();
}
BENCHMARK(BM_LookupsUnderTransactions)
    ->ArgsProduct({{1, 2, 4}, {0, 10000, 100000, -1}})
    ->UseRealTime()
    ->MinTime(2.0);

}  // namespace
