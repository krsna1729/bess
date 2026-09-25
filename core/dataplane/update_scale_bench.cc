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

// G1.2a experiments: how BESS should take dataplane updates at scale.
//
// E1 -- one table, one thread. Today's generation cuckoo (CuckooMap with the
//       staged batch probe ExactMatch uses) vs DPDK rte_hash plain vs
//       rte_hash lock-free (RW_CONCURRENCY_LF + QSBR defer queue): batch
//       lookup cost for hits and misses, and single-writer churn
//       (delete one live key + insert a new one) cost.
// E2 -- the partitioning question, with W worker threads pinned to the given
//       CPUs and a control thread on its own CPU generating churn at a target
//       rate:
//         shared      one lock-free rte_hash of N keys, read by every
//                     worker; the control thread is its only writer and
//                     applies the churn directly (mode C);
//         partitioned W plain rte_hash shards of N/W keys, each owned by
//                     one worker; the control thread routes each op by key
//                     to the owner's SPSC ring, and the worker drains its
//                     ring between lookup rounds and applies the ops itself
//                     (mode W, sharded). No synchronization on lookups.
//       Workers look up keys they own (partitioned) or any key (shared);
//       reported: lookups/s per worker and in total, and modifications/s
//       actually applied.
//
// Usage: update_scale_bench [--sizes=1000,65536,1048576,4194304,10000000]
//          [--cpus=0,2,8,10] [--control-cpu=4] [--seconds=2]
//          [--rates=0,1000000,max] [--experiments=e1,e2]
// With --benchmark_min_time=... (the Meson benchmark smoke run) it runs a
// tiny configuration for a few milliseconds.

// rte_hash_rcu_qsbr_dq_reclaim is experimental in DPDK 25.11.
#define ALLOW_EXPERIMENTAL_API 1

#include <pthread.h>
#include <sched.h>

#include <rte_cycles.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_malloc.h>
#include <rte_pause.h>
#include <rte_rcu_qsbr.h>
#include <rte_ring.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dpdk.h"
#include "utils/cuckoo_map.h"

namespace {

constexpr uint32_t kBatch = 32;
constexpr size_t kStream = size_t{1} << 22;
constexpr uint64_t kDeleteBit = uint64_t{1} << 63;

uint64_t Mix(uint64_t x) {  // splitmix64
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return (x ^ (x >> 31)) & ~kDeleteBit;  // keys never use the op bit
}

// Partition by a hash independent of the table's own. (A CRC with another
// seed is NOT independent: for fixed-length keys it differs by a constant
// XOR, so every shard's keys would share hash bits and crowd a fraction of
// the buckets -- the first version of this experiment did exactly that. Real
// partitioning, NIC RSS/Toeplitz, is independent of the table hash.)
uint32_t Owner(uint64_t key, uint32_t workers) {
  return static_cast<uint32_t>(Mix(key ^ 0xa5a5a5a5deadbeefull) % workers);
}

void Pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

std::vector<uint64_t> ParseList(const std::string &s) {
  std::vector<uint64_t> out;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t comma = s.find(',', pos);
    if (comma == std::string::npos) comma = s.size();
    const std::string item = s.substr(pos, comma - pos);
    out.push_back(item == "max" ? UINT64_MAX : std::stoull(item));
    pos = comma + 1;
  }
  return out;
}

struct Options {
  std::vector<uint64_t> sizes = {1000, 65536, 1048576, 4194304, 10000000};
  std::vector<uint64_t> cpus = {0, 2, 8, 10};
  int control_cpu = 4;
  double seconds = 2.0;
  std::vector<uint64_t> rates = {0, 1000000, UINT64_MAX};
  bool e1 = true, e2 = true, e3 = false;
};

// -- tables ------------------------------------------------------------------------

enum class HashKind { kPlain, kLockFree };

struct RteHash {
  rte_hash *h = nullptr;
  rte_rcu_qsbr *qsbr = nullptr;
  ~RteHash() {
    if (h) rte_hash_free(h);
    if (qsbr) rte_free(qsbr);
  }
};

std::unique_ptr<RteHash> MakeRteHash(size_t entries, HashKind kind,
                                     uint32_t readers, int socket = 0) {
  static std::atomic<int> seq{0};
  auto t = std::make_unique<RteHash>();
  const std::string name = "usb_" + std::to_string(seq++);
  rte_hash_parameters p{};
  p.name = name.c_str();
  // 25% headroom: a full cuckoo table spends its time displacing.
  p.entries = static_cast<uint32_t>(entries + entries / 4 + 64);
  p.key_len = sizeof(uint64_t);
  p.hash_func = rte_hash_crc;
  p.socket_id = socket;
  p.extra_flag = kind == HashKind::kLockFree
                     ? RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF
                     : 0;
  t->h = rte_hash_create(&p);
  if (t->h == nullptr) {
    fprintf(stderr, "rte_hash_create(%zu) failed\n", entries);
    return nullptr;
  }
  if (kind == HashKind::kLockFree) {
    const size_t sz = rte_rcu_qsbr_get_memsize(readers);
    t->qsbr = static_cast<rte_rcu_qsbr *>(
        rte_zmalloc(nullptr, sz, RTE_CACHE_LINE_SIZE));
    rte_rcu_qsbr_init(t->qsbr, readers);
    rte_hash_rcu_config rcu{};
    rcu.v = t->qsbr;
    rcu.mode = RTE_HASH_QSBR_MODE_DQ;
    if (rte_hash_rcu_qsbr_add(t->h, &rcu) != 0) {
      fprintf(stderr, "rte_hash_rcu_qsbr_add failed\n");
      return nullptr;
    }
  }
  return t;
}

uint64_t LookupBatch(const rte_hash *h, const uint64_t *keys) {
  const void *kp[kBatch];
  void *data[kBatch];
  for (uint32_t i = 0; i < kBatch; i++) kp[i] = &keys[i];
  uint64_t hits = 0;
  rte_hash_lookup_bulk_data(h, kp, kBatch, &hits, data);
  return static_cast<uint64_t>(__builtin_popcountll(hits));
}

double Now() { return static_cast<double>(rte_rdtsc()) / rte_get_tsc_hz(); }

// -- E1 ----------------------------------------------------------------------------

void RunE1(const Options &o) {
  printf("\n== E1: one table, one thread (ns per key; churn ns per modification)\n");
  printf("%10s  %-12s %10s %10s %10s\n", "entries", "table", "hit", "miss",
         "churn");
  for (uint64_t n : o.sizes) {
    std::vector<uint64_t> keys(n);
    for (uint64_t i = 0; i < n; i++) keys[i] = Mix(i + 1);
    std::vector<uint64_t> hit(kStream), miss(kStream);
    uint64_t r = 0x1234;
    for (size_t i = 0; i < kStream; i++) {
      hit[i] = keys[Mix(r++) % n];
      miss[i] = Mix(n + 1 + (r++));
    }
    const size_t probes = std::min<size_t>(kStream, std::max<uint64_t>(n * 4, 1 << 20));

    // Today's ExactMatch shape: CuckooMap + staged batch probe.
    {
      auto map = std::make_unique<bess::utils::CuckooMap<uint64_t, uint64_t>>();
      for (uint64_t k : keys) map->Insert(k, k);
      auto run = [&](const std::vector<uint64_t> &s) {
        uint64_t found = 0;
        const double t0 = Now();
        for (size_t b = 0; b + kBatch <= probes; b += kBatch) {
          map->PrefetchBatch(std::span<const uint64_t>(&s[b], kBatch));
          for (uint32_t i = 0; i < kBatch; i++) found += map->Find(s[b + i]) != nullptr;
        }
        const double dt = Now() - t0;
        if (found == 0 && &s == &hit) fprintf(stderr, "cuckoo: no hits?\n");
        return dt * 1e9 / probes;
      };
      const double h = run(hit), m = run(miss);
      printf("%10" PRIu64 "  %-12s %10.2f %10.2f %10s\n", n, "cuckoo-gen", h,
             m, "O(n) rebuild");
    }

    for (HashKind kind : {HashKind::kPlain, HashKind::kLockFree}) {
      auto t = MakeRteHash(n, kind, 1);
      if (!t) continue;
      for (uint64_t k : keys) rte_hash_add_key_data(t->h, &k, (void *)k);
      auto run = [&](const std::vector<uint64_t> &s) {
        uint64_t found = 0;
        const double t0 = Now();
        for (size_t b = 0; b + kBatch <= probes; b += kBatch) {
          found += LookupBatch(t->h, &s[b]);
        }
        return (Now() - t0) * 1e9 / probes;
      };
      const double h = run(hit), m = run(miss);
      // Churn: replace a random live key with a fresh one (delete + insert).
      // No concurrent reader, so the QSBR defer queue reclaims immediately.
      const size_t churn = std::min<size_t>(1 << 20, n * 2);
      std::vector<uint64_t> live = keys;
      uint64_t fresh = n + 0x10000000ull;
      const double t0 = Now();
      for (size_t i = 0; i < churn; i++) {
        const size_t slot = Mix(fresh) % n;
        rte_hash_del_key(t->h, &live[slot]);
        live[slot] = Mix(fresh++);
        rte_hash_add_key_data(t->h, &live[slot], (void *)live[slot]);
      }
      const double c = (Now() - t0) * 1e9 / (2.0 * churn);
      printf("%10" PRIu64 "  %-12s %10.2f %10.2f %10.1f\n", n,
             kind == HashKind::kPlain ? "rte_hash" : "rte_hash-LF", h, m, c);
    }
  }
}

// -- E2 ----------------------------------------------------------------------------

struct Result {
  double lookups_per_s = 0;
  double mods_per_s = 0;
  double hit_ratio = 0;
  uint64_t stalls = 0;  // adds that had to wait for slot reclamation
};

// The control thread's churn generator: replace live keys with fresh ones.
// Emits (delete old, insert new) pairs; `emit` returns false to stop.
template <typename Emit>
uint64_t Churn(std::vector<uint64_t> &live, uint64_t rate,
               std::atomic<bool> &stop, Emit &&emit) {
  uint64_t fresh = 0x40000000ull, ops = 0;
  const uint64_t hz = rte_get_tsc_hz();
  const uint64_t start = rte_rdtsc();
  while (!stop.load(std::memory_order_relaxed)) {
    if (rate != UINT64_MAX && rate != 0) {
      const uint64_t due = (rte_rdtsc() - start) * rate / hz;
      if (ops >= due) {
        rte_pause();
        continue;
      }
    } else if (rate == 0) {
      break;
    }
    const size_t slot = Mix(fresh) % live.size();
    const uint64_t old = live[slot];
    const uint64_t neu = Mix(fresh++);
    live[slot] = neu;
    if (!emit(old, neu)) break;
    ops += 2;
  }
  return ops;
}

Result RunShared(const Options &o, uint64_t n, uint64_t rate) {
  const uint32_t w = static_cast<uint32_t>(o.cpus.size());
  auto t = MakeRteHash(n, HashKind::kLockFree, w);
  if (!t) return {};
  std::vector<uint64_t> keys(n);
  for (uint64_t i = 0; i < n; i++) {
    keys[i] = Mix(i + 1);
    rte_hash_add_key_data(t->h, &keys[i], (void *)keys[i]);
  }
  std::atomic<bool> go{false}, stop{false};
  std::atomic<uint32_t> ready{0};
  std::vector<uint64_t> lookups(w, 0), hits(w, 0);
  std::vector<std::thread> threads;
  for (uint32_t id = 0; id < w; id++) {
    threads.emplace_back([&, id] {
      Pin(static_cast<int>(o.cpus[id]));
      rte_rcu_qsbr_thread_register(t->qsbr, id);
      rte_rcu_qsbr_thread_online(t->qsbr, id);
      std::vector<uint64_t> s(kStream);
      uint64_t r = 0x777 + id;
      for (auto &k : s) k = keys[Mix(r++) % n];
      ready++;
      while (!go.load()) rte_pause();
      size_t off = 0;
      uint64_t lk = 0, ht = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        for (int rep = 0; rep < 8; rep++) {  // one scheduler round
          ht += LookupBatch(t->h, &s[off]);
          lk += kBatch;
          off = (off + kBatch) % (kStream - kBatch);
        }
        rte_rcu_qsbr_quiescent(t->qsbr, id);
      }
      lookups[id] = lk;
      hits[id] = ht;
      rte_rcu_qsbr_thread_offline(t->qsbr, id);
      rte_rcu_qsbr_thread_unregister(t->qsbr, id);
    });
  }
  uint64_t mods = 0, stalls = 0;
  std::thread control([&] {
    Pin(o.control_cpu);
    std::vector<uint64_t> live = keys;
    while (!go.load()) rte_pause();
    mods = Churn(live, rate, stop, [&](uint64_t old, uint64_t neu) {
      rte_hash_del_key(t->h, &old);
      // A deleted slot is reusable only after the readers' grace period, so
      // an add can transiently find no free slot: reclaim and retry.
      while (rte_hash_add_key_data(t->h, &neu, (void *)neu) != 0) {
        stalls++;
        unsigned freed, pending, available;
        rte_hash_rcu_qsbr_dq_reclaim(t->h, &freed, &pending, &available);
        if (stop.load(std::memory_order_relaxed)) return false;
        rte_pause();
      }
      return true;
    });
  });
  // Time only once every worker has built its key stream.
  while (ready.load() < w) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  go = true;
  const double t0 = Now();
  while (Now() - t0 < o.seconds) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stop = true;
  control.join();
  for (auto &th : threads) th.join();
  const double dt = Now() - t0;
  Result res;
  uint64_t lk = 0, ht = 0;
  for (uint32_t i = 0; i < w; i++) { lk += lookups[i]; ht += hits[i]; }
  res.lookups_per_s = lk / dt;
  res.mods_per_s = mods / dt;
  res.hit_ratio = lk ? static_cast<double>(ht) / lk : 0;
  res.stalls = stalls;
  return res;
}

Result RunPartitioned(const Options &o, uint64_t n, uint64_t rate) {
  const uint32_t w = static_cast<uint32_t>(o.cpus.size());
  std::vector<std::vector<uint64_t>> owned(w);
  for (uint64_t i = 0; i < n; i++) {
    const uint64_t k = Mix(i + 1);
    owned[Owner(k, w)].push_back(k);
  }
  std::vector<std::unique_ptr<RteHash>> shards(w);
  std::vector<rte_ring *> rings(w);
  static std::atomic<int> ring_seq{0};
  for (uint32_t i = 0; i < w; i++) {
    // Each shard lives on the socket of... (single-socket host here).
    shards[i] = MakeRteHash(owned[i].size() + 1024, HashKind::kPlain, 1);
    if (!shards[i]) return {};
    for (uint64_t k : owned[i]) rte_hash_add_key_data(shards[i]->h, &k, (void *)k);
    const std::string rn = "usbr_" + std::to_string(ring_seq++);
    rings[i] = rte_ring_create(rn.c_str(), 1 << 16, 0,
                               RING_F_SP_ENQ | RING_F_SC_DEQ);
  }
  std::atomic<bool> go{false}, stop{false};
  std::atomic<uint32_t> ready{0};
  std::vector<uint64_t> lookups(w, 0), hits(w, 0), applied(w, 0);
  std::vector<std::thread> threads;
  for (uint32_t id = 0; id < w; id++) {
    threads.emplace_back([&, id] {
      Pin(static_cast<int>(o.cpus[id]));
      rte_hash *h = shards[id]->h;
      std::vector<uint64_t> s(kStream);
      uint64_t r = 0x777 + id;
      for (auto &k : s) k = owned[id][Mix(r++) % owned[id].size()];
      ready++;
      while (!go.load()) rte_pause();
      size_t off = 0;
      uint64_t lk = 0, ht = 0, ap = 0;
      void *ops[1024];
      while (!stop.load(std::memory_order_relaxed)) {
        for (int rep = 0; rep < 8; rep++) {  // one scheduler round
          ht += LookupBatch(h, &s[off]);
          lk += kBatch;
          off = (off + kBatch) % (kStream - kBatch);
        }
        // End of round: apply this worker's pending ops. The table is
        // private, so a delete frees its slot at once -- no grace period.
        const unsigned got = rte_ring_sc_dequeue_burst(rings[id], ops, 1024, nullptr);
        for (unsigned i = 0; i < got; i++) {
          uint64_t v = reinterpret_cast<uint64_t>(ops[i]);
          if (v & kDeleteBit) {
            v &= ~kDeleteBit;
            rte_hash_del_key(h, &v);
          } else {
            rte_hash_add_key_data(h, &v, (void *)v);
          }
        }
        ap += got;
      }
      lookups[id] = lk;
      hits[id] = ht;
      applied[id] = ap;
    });
  }
  std::thread control([&] {
    Pin(o.control_cpu);
    std::vector<uint64_t> live;
    for (auto &v : owned) live.insert(live.end(), v.begin(), v.end());
    while (!go.load()) rte_pause();
    Churn(live, rate, stop, [&](uint64_t old, uint64_t neu) {
      const uint32_t a = Owner(old, w), b = Owner(neu, w);
      void *del = reinterpret_cast<void *>(old | kDeleteBit);
      void *add = reinterpret_cast<void *>(neu);
      while (rte_ring_sp_enqueue(rings[a], del) != 0) {
        if (stop.load(std::memory_order_relaxed)) return false;
        rte_pause();
      }
      while (rte_ring_sp_enqueue(rings[b], add) != 0) {
        if (stop.load(std::memory_order_relaxed)) return false;
        rte_pause();
      }
      return true;
    });
  });
  // Time only once every worker has built its key stream.
  while (ready.load() < w) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  go = true;
  const double t0 = Now();
  while (Now() - t0 < o.seconds) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stop = true;
  control.join();
  for (auto &th : threads) th.join();
  const double dt = Now() - t0;
  Result res;
  uint64_t lk = 0, ht = 0, ap = 0;
  for (uint32_t i = 0; i < w; i++) { lk += lookups[i]; ht += hits[i]; ap += applied[i]; }
  res.lookups_per_s = lk / dt;
  res.mods_per_s = ap / dt;
  res.hit_ratio = lk ? static_cast<double>(ht) / lk : 0;
  for (auto *r : rings) rte_ring_free(r);
  return res;
}

void RunE2(const Options &o) {
  const size_t w = o.cpus.size();
  printf("\n== E2: %zu workers + 1 control thread (lookups: Mlookups/s total, per worker; mods: M/s applied)\n", w);
  printf("%10s %8s  %-12s %10s %10s %8s %8s %8s\n", "entries", "rate", "mode",
         "total", "/worker", "mods", "hit%", "stalls");
  for (uint64_t n : o.sizes) {
    for (uint64_t rate : o.rates) {
      const std::string rs = rate == UINT64_MAX ? "max" : std::to_string(rate);
      for (int mode = 0; mode < 2; mode++) {
        const Result r = mode == 0 ? RunShared(o, n, rate) : RunPartitioned(o, n, rate);
        printf("%10" PRIu64 " %8s  %-12s %10.1f %10.1f %8.3f %7.1f%% %8" PRIu64 "\n", n,
               rs.c_str(), mode == 0 ? "shared-LF" : "partitioned",
               r.lookups_per_s / 1e6, r.lookups_per_s / 1e6 / w,
               r.mods_per_s / 1e6, r.hit_ratio * 100, r.stalls);
        fflush(stdout);
      }
    }
  }
}

// E3 -- diagnostic: is a shard's table itself slower, or is it multi-worker
// interaction? Single thread: a plain table holding one worker's shard
// (Owner == 0 of n keys) vs a plain table of the same number of unfiltered
// keys, and the same shard table allocated alone vs next to three siblings.
void RunE3(const Options &o) {
  const uint32_t w = static_cast<uint32_t>(o.cpus.size());
  printf("\n== E3: single-thread lookup ns/key -- shard table vs same-size unfiltered table\n");
  printf("%10s %12s %12s %12s\n", "entries", "shard", "unfiltered", "shard+3sib");
  for (uint64_t n : o.sizes) {
    std::vector<uint64_t> shard, plain;
    for (uint64_t i = 0; i < n; i++) {
      const uint64_t k = Mix(i + 1);
      if (Owner(k, w) == 0) shard.push_back(k);
    }
    for (uint64_t i = 0; i < shard.size(); i++) plain.push_back(Mix(i + 1));
    auto measure = [&](const std::vector<uint64_t> &keys, int siblings) {
      std::vector<std::unique_ptr<RteHash>> sib;
      for (int j = 0; j < siblings; j++) {
        sib.push_back(MakeRteHash(keys.size() + 1024, HashKind::kPlain, 1));
      }
      auto t = MakeRteHash(keys.size() + 1024, HashKind::kPlain, 1);
      for (uint64_t k : keys) rte_hash_add_key_data(t->h, &k, (void *)k);
      std::vector<uint64_t> st(kStream);
      uint64_t r = 0x999;
      for (auto &k : st) k = keys[Mix(r++) % keys.size()];
      const size_t probes = kStream;
      const double t0 = Now();
      uint64_t found = 0;
      for (size_t b = 0; b + kBatch <= probes; b += kBatch) found += LookupBatch(t->h, &st[b]);
      const double ns = (Now() - t0) * 1e9 / probes;
      if (found < probes / 2) fprintf(stderr, "E3: low hits\n");
      return ns;
    };
    printf("%10" PRIu64 " %12.2f %12.2f %12.2f\n", n, measure(shard, 0),
           measure(plain, 0), measure(shard, 3));
    fflush(stdout);
  }
}

}  // namespace

int main(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto val = [&](const char *p) { return a.substr(strlen(p)); };
    if (a.rfind("--sizes=", 0) == 0) o.sizes = ParseList(val("--sizes="));
    else if (a.rfind("--cpus=", 0) == 0) o.cpus = ParseList(val("--cpus="));
    else if (a.rfind("--control-cpu=", 0) == 0) o.control_cpu = std::stoi(val("--control-cpu="));
    else if (a.rfind("--seconds=", 0) == 0) o.seconds = std::stod(val("--seconds="));
    else if (a.rfind("--rates=", 0) == 0) o.rates = ParseList(val("--rates="));
    else if (a.rfind("--experiments=", 0) == 0) {
      o.e1 = a.find("e1") != std::string::npos;
      o.e2 = a.find("e2") != std::string::npos;
      o.e3 = a.find("e3") != std::string::npos;
    } else if (a.rfind("--benchmark_min_time", 0) == 0) {
      // Meson smoke run: tiny and quick.
      o.sizes = {1000};
      o.cpus = {0, 1};
      o.control_cpu = 0;
      o.seconds = 0.05;
      o.rates = {0, 100000};
    }
  }
  bess::InitDpdk(0);
  if (o.e1) RunE1(o);
  if (o.e2) RunE2(o);
  if (o.e3) RunE3(o);
  return 0;
}
