// SPDX-License-Identifier: BSD-3-Clause

// Packet-path writers (Decision D-028): N threads, each behaving like a
// worker that learns flows -- per step it adds one new flow (insert if
// absent), removes its oldest, looks up L keys in 32-key batches, and
// reports a quiescent state -- against three table layouts:
//
//   variant 0  partitioned: one kSingle ConcurrentExactTable per thread
//              (each thread writes only its own; no lock anywhere)
//   variant 1  shared: one kShared ConcurrentExactTable, writers serialized
//              by its spinlock, InsertIfAbsent
//   variant 3  striped: kStripes kShared tables, a key's stripe chosen by a
//              hash of it (lock striping); a lookup batch is grouped by
//              stripe first
//   variant 2  DPDK multi-writer: one rte_hash with MULTI_WRITER_ADD |
//              RW_CONCURRENCY_LF (writers serialized by DPDK's table lock,
//              per-lcore free-slot caches; overwriting add -- the ceiling,
//              without insert-if-absent semantics)
//
// Args: variant, threads, lookups per insert. Reported: M inserts/s and M
// lookups/s in total (each insert comes with one erase).

// rte_hash_rcu_qsbr_add is experimental in DPDK 25.11.
#define ALLOW_EXPERIMENTAL_API 1

#include <benchmark/benchmark.h>

#include <pthread.h>
#include <sched.h>

#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_lcore.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "classifier/concurrent_exact.h"
#include "runtime/runtime_state.h"
#include "rcu/rcu_domain.h"

namespace {

using bess::classifier::ConcurrentExactTable;
using bess::classifier::ConstBytes;

// The CPUs this process may use, captured before DPDK's EAL starts.
const cpu_set_t kInitialCpus = [] {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  return set;
}();

constexpr uint64_t kWindow = 16384;  // live flows per thread
constexpr int kStripes = 16;

int StripeOf(uint64_t key) {
  return static_cast<int>((key * 0x9e3779b97f4a7c15ull) >> 60);  // 16
}
constexpr size_t kBatch = 32;

ConstBytes Bytes(const uint64_t *key, size_t n = 1) {
  return ConstBytes(reinterpret_cast<const std::byte *>(key), 8 * n);
}

// DPDK's own multi-writer table, for comparison.
struct DpdkMultiWriter {
  rte_hash *h = nullptr;
  explicit DpdkMultiWriter(uint32_t entries, bess::rcu::RcuDomain &domain) {
    static std::atomic<int> seq{0};
    const std::string name = "swb_" + std::to_string(seq++);
    rte_hash_parameters p{};
    p.name = name.c_str();
    p.entries = entries;
    p.key_len = 8;
    p.hash_func = rte_hash_crc;
    p.socket_id = SOCKET_ID_ANY;
    p.extra_flag = RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD |
                   RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;
    h = rte_hash_create(&p);
    if (h != nullptr) {
      rte_hash_rcu_config rcu{};
      rcu.v = domain.dpdk_qsbr();
      rcu.mode = RTE_HASH_QSBR_MODE_DQ;
      if (rte_hash_rcu_qsbr_add(h, &rcu) != 0) {
        rte_hash_free(h);
        h = nullptr;
      }
    }
  }
  ~DpdkMultiWriter() {
    if (h != nullptr) {
      rte_hash_free(h);
    }
  }
};

void BM_PacketPathWriters(benchmark::State &state) {
  const int variant = static_cast<int>(state.range(0));
  const int threads = static_cast<int>(state.range(1));
  const int lookups = static_cast<int>(state.range(2));
  bess::rcu::RcuDomain &domain = bess::runtime::runtime().rcu();

  // Room for every live flow plus deletes waiting out a grace period.
  const uint32_t per_thread = ConcurrentExactTable::CapacityFor(kWindow * 2);
  const uint32_t shared =
      ConcurrentExactTable::CapacityFor(kWindow * 2 * threads);
  std::vector<std::unique_ptr<ConcurrentExactTable>> tables;
  std::unique_ptr<DpdkMultiWriter> dpdk;
  if (variant == 0) {
    for (int t = 0; t < threads; t++) {
      tables.push_back(*ConcurrentExactTable::Create(8, per_thread, domain));
    }
  } else if (variant == 1) {
    tables.push_back(*ConcurrentExactTable::Create(
        8, shared, domain, SOCKET_ID_ANY,
        ConcurrentExactTable::Writers::kShared));
  } else if (variant == 3) {
    const uint32_t stripe = ConcurrentExactTable::CapacityFor(
        kWindow * 2 * threads / kStripes + kWindow / 4);
    for (int i = 0; i < kStripes; i++) {
      tables.push_back(*ConcurrentExactTable::Create(
          8, stripe, domain, SOCKET_ID_ANY,
          ConcurrentExactTable::Writers::kShared));
    }
  } else {
    dpdk = std::make_unique<DpdkMultiWriter>(shared, domain);
    if (dpdk->h == nullptr) {
      state.SkipWithError("rte_hash multi-writer create failed");
      return;
    }
  }

  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::vector<uint64_t> inserts(threads), found(threads);
  std::vector<std::thread> pool;
  const int main_cpu = sched_getcpu();
  int cpu = 0;
  for (int t = 0; t < threads; t++) {
    while (cpu < CPU_SETSIZE &&
           (cpu == main_cpu || !CPU_ISSET(cpu, &kInitialCpus))) {
      cpu++;
    }
    const int my_cpu = cpu < CPU_SETSIZE ? cpu++ : -1;
    const auto id = static_cast<bess::rcu::ReaderId>(30 + t);
    (void)domain.Register(id);
    pool.emplace_back([&, t, id, my_cpu] {
      if (my_cpu >= 0) {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(my_cpu, &one);
        pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
      }
      rte_thread_register();  // DPDK's multi-writer needs an lcore id
      domain.Online(id);
      ConcurrentExactTable *table =
          variant == 0 ? tables[t].get()
                       : variant == 1 ? tables[0].get() : nullptr;
      const bool striped = variant == 3;
      auto stripe = [&](uint64_t key) {
        return tables[StripeOf(key)].get();
      };
      const uint64_t tag = static_cast<uint64_t>(t + 1) << 48;
      std::mt19937_64 rng(t);
      uint64_t next = 0, n_inserts = 0, n_found = 0;
      uint64_t keys[kBatch], values[kBatch];
      void *data[kBatch];
      const void *ptrs[kBatch];
      auto add = [&](uint64_t key) {
        if (striped) {
          stripe(key)->InsertIfAbsent(Bytes(&key), key);
        } else if (table != nullptr) {
          table->InsertIfAbsent(Bytes(&key), key);
        } else {
          rte_hash_add_key_data(dpdk->h, &key,
                                reinterpret_cast<void *>(key));
        }
      };
      auto erase = [&](uint64_t key) {
        if (striped) {
          stripe(key)->Erase(Bytes(&key));
        } else if (table != nullptr) {
          table->Erase(Bytes(&key));
        } else {
          rte_hash_del_key(dpdk->h, &key);
        }
      };
      for (; next < kWindow; next++) {  // the live window
        add(tag | next);
      }
      ready++;
      while (!stop.load(std::memory_order_relaxed)) {
        add(tag | next);
        erase(tag | (next - kWindow));
        next++;
        n_inserts++;
        for (int done = 0; done < lookups; done += kBatch) {
          for (size_t i = 0; i < kBatch; i++) {
            keys[i] = tag | (next - 1 - rng() % kWindow);
            ptrs[i] = &keys[i];
          }
          uint64_t hits = 0;
          if (striped) {
            // Group the batch by stripe, one prehashed bulk lookup each.
            uint64_t grouped[kStripes][kBatch], out[kBatch];
            size_t count[kStripes] = {};
            for (size_t i = 0; i < kBatch; i++) {
              const int st = StripeOf(keys[i]);
              grouped[st][count[st]++] = keys[i];
            }
            for (int st = 0; st < kStripes; st++) {
              if (count[st] != 0) {
                hits += static_cast<uint64_t>(__builtin_popcountll(
                    tables[st]->LookupBatch(Bytes(grouped[st], count[st]), 8,
                                            out, count[st])));
              }
            }
            n_found += hits;
            continue;
          }
          if (table != nullptr) {
            hits = table->LookupBatch(Bytes(keys, kBatch), 8, values, kBatch);
          } else {
            rte_hash_lookup_bulk_data(dpdk->h, ptrs, kBatch, &hits, data);
          }
          n_found += static_cast<uint64_t>(__builtin_popcountll(hits));
        }
        domain.Quiescent(id);
      }
      inserts[t] = n_inserts;
      found[t] = n_found;
      domain.Offline(id);
      rte_thread_unregister();
    });
  }
  while (ready.load() < threads) {
    std::this_thread::yield();
  }
  const auto start = std::chrono::steady_clock::now();
  for (auto _ : state) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const double seconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();
  stop = true;
  for (auto &th : pool) {
    th.join();
  }
  uint64_t total_inserts = 0, total_found = 0;
  for (int t = 0; t < threads; t++) {
    total_inserts += inserts[t];
    total_found += found[t];
    domain.Unregister(static_cast<bess::rcu::ReaderId>(30 + t));
  }
  const uint64_t total_lookups =
      total_inserts * static_cast<uint64_t>(lookups);
  state.counters["Minserts_s"] = total_inserts / seconds / 1e6;
  state.counters["Mlookups_s"] = total_lookups / seconds / 1e6;
  state.counters["hit_ratio"] =
      total_lookups ? static_cast<double>(total_found) / total_lookups : 0;
  static const char *kNames[] = {"partitioned", "shared-lock", "dpdk-mw",
                                 "striped-16"};
  state.SetLabel(kNames[variant]);
  tables.clear();
  dpdk.reset();
  domain.Drain();
}
BENCHMARK(BM_PacketPathWriters)
    ->ArgsProduct({{0, 1, 2, 3}, {1, 2, 4}, {32, 256}})
    ->UseRealTime()
    ->MinTime(1.0);

}  // namespace
