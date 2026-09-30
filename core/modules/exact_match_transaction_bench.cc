// SPDX-License-Identifier: BSD-3-Clause

// ExactMatch's packet path measured through the module itself
// (ExactMatch::ClassifyBatch runs the code ProcessBatch runs, minus the
// emit), over real packets.
//
//   BM_ModuleClassify/N/mix  one 32-packet batch per iteration against a
//                            module holding N rules; mix 0 = all hits,
//                            1 = all misses. Items = packets.
//   BM_ClassifyUnderTransactions/readers/rate
//                            `readers` threads (registered RCU readers,
//                            each on its own CPU) classify packets through
//                            two modules, "ul" and "dl", while this thread
//                            establishes and releases sessions -- one
//                            transaction adds a rule to each module, another
//                            removes them -- at `rate` sessions/s (0: none,
//                            -1: as fast as it can). Reported: Mpps per
//                            reader (a packet = one lookup in one module),
//                            sessions/s achieved.

#include <benchmark/benchmark.h>

#include <pthread.h>
#include <sched.h>

#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "runtime/runtime_state.h"
#include "dataplane/transaction_engine.h"
#include "module.h"
#include "module_graph.h"
#include "modules/exact_match.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_domain.h"

namespace {

// The CPUs this process may use, captured before DPDK's EAL starts (it pins
// the main thread to one core, and threads created later inherit that).
const cpu_set_t kInitialCpus = [] {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  return set;
}();

constexpr size_t kBatch = 32;
constexpr size_t kBatches = 1024;  // 32K packets: keys spread over the table

bess::pb::ExactMatchCommandAddArg Rule(uint32_t a, uint16_t b, uint64_t gate) {
  bess::pb::ExactMatchCommandAddArg arg;
  arg.set_gate(gate);
  arg.add_fields()->set_value_int(a);
  arg.add_fields()->set_value_int(b);
  return arg;
}

// Fields: 4 bytes at offset 26 (IPv4 source), 2 bytes at offset 34 (UDP
// source port). Rule r matches (r, r & 0xffff) and goes to gate 1 + r % 63;
// misses take the default gate, 0.
ExactMatch *CreateModule(const char *name, size_t rules) {
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  f = arg.add_fields();
  f->set_offset(34);
  f->set_num_bytes(2);
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) {
    std::abort();
  }
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(
      ModuleBuilder::all_module_builders().at("ExactMatch"), name, packed,
      &perr);
  if (m == nullptr || perr.code() != 0) {
    std::abort();
  }
  auto *em = static_cast<ExactMatch *>(m);
  bess::pb::ExactMatchConfig config;
  for (size_t r = 0; r < rules; r++) {
    *config.add_rules() = Rule(static_cast<uint32_t>(r),
                               static_cast<uint16_t>(r), 1 + r % 63);
  }
  if (em->SetRuntimeConfig(config).has_error()) {
    std::abort();
  }
  return em;
}

// A UDP packet whose key is (a, b).
void SetKey(bess::PacketHandle pkt, uint32_t a, uint16_t b) {
  uint8_t *p = bess::PacketRef(pkt).head_data<uint8_t *>();
  std::memcpy(p + 26, &a, sizeof(a));
  std::memcpy(p + 34, &b, sizeof(b));
}

struct Packets {
  explicit Packets(size_t n) : pool(n + 64) {
    for (size_t i = 0; i < n; i++) {
      bess::PacketHandle pkt = pool.Alloc(60);
      if (pkt == nullptr) {
        std::abort();
      }
      uint8_t *p = bess::PacketRef(pkt).head_data<uint8_t *>();
      std::memset(p, 0, 60);
      p[12] = 0x08;
      p[14] = 0x45;
      p[23] = 17;
      handles.push_back(pkt);
    }
  }
  ~Packets() {
    for (bess::PacketHandle pkt : handles) {
      bess::PacketFree(pkt);
    }
  }
  bess::PlainPacketPool pool;
  std::vector<bess::PacketHandle> handles;
};

void Load(bess::PacketBatch *batch, const Packets &packets, size_t first) {
  batch->clear();
  for (size_t i = 0; i < kBatch; i++) {
    batch->add(bess::PacketRef(packets.handles[first + i]));
  }
}

void BM_ModuleClassify(benchmark::State &state) {
  const size_t rules = static_cast<size_t>(state.range(0));
  const bool miss = state.range(1) == 1;
  ModuleGraph::DestroyAllModules();
  ExactMatch *em = CreateModule("em", rules);
  Packets packets(kBatch * kBatches);
  std::mt19937_64 rng(42);
  for (bess::PacketHandle pkt : packets.handles) {
    const uint32_t r = static_cast<uint32_t>(rng() % rules);
    SetKey(pkt, miss ? r | 0x80000000u : r, static_cast<uint16_t>(r));
  }
  std::vector<bess::PacketBatch> batches(kBatches);
  for (size_t b = 0; b < kBatches; b++) {
    Load(&batches[b], packets, b * kBatch);
  }
  std::array<gate_idx_t, kBatch> gates;
  uint64_t hits = 0;
  size_t b = 0;
  for (auto _ : state) {
    em->ClassifyBatch(&batches[b], gates.data());
    benchmark::DoNotOptimize(gates);
    for (gate_idx_t g : gates) {
      hits += g != 0;
    }
    b = (b + 1) % kBatches;
  }
  if (hits != (miss ? 0 : state.iterations() * kBatch)) {
    state.SkipWithError("wrong hit count");
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
  ModuleGraph::DestroyAllModules();
}
BENCHMARK(BM_ModuleClassify)
    ->ArgsProduct({{1000, 128000, 1000000}, {0, 1}});

std::string Key(uint32_t a, uint16_t b) {
  std::string key(6, '\0');
  std::memcpy(key.data(), &a, sizeof(a));
  std::memcpy(key.data() + 4, &b, sizeof(b));
  return key;
}

void BM_ClassifyUnderTransactions(benchmark::State &state) {
  using bess::dataplane::Op;
  using bess::dataplane::TransactionEngine;
  const int readers = static_cast<int>(state.range(0));
  const int64_t rate = state.range(1);  // sessions/s; 0 none; -1 max
  constexpr uint32_t kSessions = 65536;
  ModuleGraph::DestroyAllModules();
  // Session s: rule (s, 1) in "ul" and (s, 2) in "dl", both to gate
  // 1 + s % 63. Half the sessions are live at any time.
  ExactMatch *ul = CreateModule("ul", 0);
  ExactMatch *dl = CreateModule("dl", 0);
  TransactionEngine &engine = bess::runtime::runtime().transactions();
  auto establish = [](uint32_t s) {
    const uint64_t gate = 1 + s % 63;
    return std::vector<Op>{
        Op::Upsert("ul/rules", Key(s, 1), std::any(gate)),
        Op::Upsert("dl/rules", Key(s, 2), std::any(gate))};
  };
  auto release = [](uint32_t s) {
    return std::vector<Op>{Op::Erase("ul/rules", Key(s, 1)),
                           Op::Erase("dl/rules", Key(s, 2))};
  };
  uint32_t oldest = 0, next = 0;
  for (; next < kSessions / 2; next++) {
    if (engine.Apply(establish(next)).outcome !=
        TransactionEngine::Outcome::kApplied) {
      state.SkipWithError("populate failed");
      return;
    }
  }

  // Per reader: packets for random sessions (live or not), half for each
  // module.
  std::vector<std::unique_ptr<Packets>> packets;
  std::mt19937_64 rng(42);
  for (int r = 0; r < readers; r++) {
    packets.push_back(std::make_unique<Packets>(kBatch * kBatches));
    for (size_t i = 0; i < kBatch * kBatches; i++) {
      const uint32_t session = static_cast<uint32_t>(rng() % kSessions);
      SetKey(packets[r]->handles[i], session, (i / kBatch) % 2 ? 2 : 1);
    }
  }

  bess::rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  std::atomic<bool> stop{false};
  std::vector<std::atomic<uint64_t>> classified(readers);
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
    classified[r] = 0;
    threads.emplace_back([&, r, id, reader_cpu] {
      if (reader_cpu >= 0) {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(reader_cpu, &one);
        pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
      }
      std::vector<bess::PacketBatch> batches(kBatches);
      for (size_t b = 0; b < kBatches; b++) {
        Load(&batches[b], *packets[r], b * kBatch);
      }
      domain.Online(id);
      std::array<gate_idx_t, kBatch> gates;
      uint64_t local = 0, sink = 0;
      size_t b = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        (b % 2 ? dl : ul)->ClassifyBatch(&batches[b], gates.data());
        for (gate_idx_t g : gates) {
          sink += g;
        }
        local += kBatch;
        b = (b + 1) % kBatches;
        domain.Quiescent(id);
      }
      benchmark::DoNotOptimize(sink);
      classified[r] = local;
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
    // kBusy means readers are behind on quiescing: try again.
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
    domain.ReclaimReady();
  }
  const double seconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();
  stop = true;
  for (auto &t : threads) {
    t.join();
  }
  uint64_t total = 0;
  for (auto &c : classified) {
    total += c;
  }
  state.counters["Mpps_per_reader"] =
      static_cast<double>(total) / readers / seconds / 1e6;
  state.counters["sessions_per_s"] = static_cast<double>(sessions) / seconds;
  state.counters["busy_retries"] = static_cast<double>(busy);
  for (int r = 0; r < readers; r++) {
    domain.Unregister(static_cast<bess::rcu::ReaderId>(20 + r));
  }
  packets.clear();
  ModuleGraph::DestroyAllModules();
  domain.Drain();
}
BENCHMARK(BM_ClassifyUnderTransactions)
    ->ArgsProduct({{1, 2, 4}, {0, 10000, 100000, -1}})
    ->UseRealTime()
    ->MinTime(2.0);

}  // namespace
