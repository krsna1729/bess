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


// How full do BESS's hash tables get before an add fails, and what do adds
// cost on the way there? Recorded for docs/decisions.md D-001/D-002 (the
// ConcurrentExactTable growth threshold) and D-009 (backend consolidation).
//
//   fill    Insert distinct keys until the first failure (occupancy at first
//           failure), then keep going until 64 consecutive failures
//           (occupancy reachable). Also the mean and worst insert cost per
//           tenth of the fill.
//   churn   Hold a target load and replace keys (erase one, insert a new
//           one). Reports adds that fail per million, with and without a
//           registered reader that reports quiescence every `q` operations
//           (so deleted slots wait out a grace period, as with live workers).
//
// Tables:
//   rte_hash      LF + QSBR (DQ), as ConcurrentExactTable creates it, with
//                 `entries` a power of two (bucket positions == key slots)
//   rte_hash/0.75 same, entries = 3/4 of a power of two (positions = 4/3 of
//                 slots)
//   rte_hash/ext  entries a power of two, plus RTE_HASH_EXTRA_FLAGS_EXT_TABLE
//   CuckooMap     utils/cuckoo_map.h: never fails, doubles its buckets when
//                 displacement fails; reports the load at each doubling
//
// Keys (8 bytes): random (splitmix64), sequential (base + i: TEIDs, ids), and
// ip-port (4096 addresses x sequential ports: NAT/5-tuple-like).
//
// Usage: occupancy_bench [fill|churn|all] [max_log2_entries (default 20)]
// Run pinned to one CPU. Needs DPDK hugepages (BESS_DPDK_HUGEPAGE_MB).

#define ALLOW_EXPERIMENTAL_API 1

#include <rte_cycles.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "control/runtime_state.h"
#include "dpdk.h"
#include "rcu/rcu_domain.h"
#include "utils/cuckoo_map.h"

namespace {

enum class Dist { kRandom, kSequential, kIpPort };
const char *DistName(Dist d) {
  return d == Dist::kRandom ? "random" : d == Dist::kSequential ? "sequential"
                                                                : "ip-port";
}

uint64_t SplitMix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// The i-th distinct key of a stream. Distinct by construction for i < 2^32.
uint64_t KeyAt(Dist d, uint64_t seed, uint64_t i) {
  switch (d) {
    case Dist::kRandom:
      return SplitMix(seed * 0x1000000000ull + i);
    case Dist::kSequential:
      return (seed << 40) + i;
    case Dist::kIpPort:
    default: {
      const uint64_t ip = 0x0a000000u + seed * 0x10000 + (i % 4096);
      const uint64_t port = 1024 + i / 4096;
      return (ip << 16) | port;
    }
  }
}

enum class Kind { kPow2, kThreeQuarter, kExt };
const char *KindName(Kind k) {
  return k == Kind::kPow2 ? "rte_hash" : k == Kind::kThreeQuarter
                                             ? "rte_hash/0.75"
                                             : "rte_hash/ext";
}

rte_hash *Create(Kind kind, uint32_t pow2) {
  static int seq = 0;
  const std::string name = "occ" + std::to_string(seq++);
  rte_hash_parameters p{};
  p.name = name.c_str();
  p.entries = kind == Kind::kThreeQuarter ? pow2 / 4 * 3 : pow2;
  p.key_len = sizeof(uint64_t);
  p.hash_func = rte_hash_crc;
  p.socket_id = SOCKET_ID_ANY;
  p.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                 (kind == Kind::kExt ? RTE_HASH_EXTRA_FLAGS_EXT_TABLE : 0);
  rte_hash *h = rte_hash_create(&p);
  if (h == nullptr) {
    std::fprintf(stderr, "rte_hash_create failed\n");
    std::abort();
  }
  rte_hash_rcu_config rcu{};
  rcu.v = bess::control::runtime().rcu().dpdk_qsbr();
  rcu.mode = RTE_HASH_QSBR_MODE_DQ;
  if (rte_hash_rcu_qsbr_add(h, &rcu) != 0) {
    std::abort();
  }
  return h;
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// -- fill ------------------------------------------------------------------

struct FillResult {
  double first_fail = 0;  // fraction of key slots used at the first failure
  double reachable = 0;   // after 64 consecutive failures
  double ns_mean[10] = {};
  double ns_max[10] = {};
};

FillResult FillRteHash(Kind kind, uint32_t pow2, Dist dist, uint64_t seed) {
  rte_hash *h = Create(kind, pow2);
  const double slots = kind == Kind::kThreeQuarter ? pow2 / 4 * 3 : pow2;
  const double ns_per_cycle = 1e9 / static_cast<double>(rte_get_tsc_hz());
  FillResult r;
  uint64_t count = 0, sum[10] = {}, n[10] = {}, worst[10] = {};
  int consecutive = 0;
  bool failed = false;
  for (uint64_t i = 0; consecutive < 64 && i < 4ull * pow2; i++) {
    const uint64_t key = KeyAt(dist, seed, i);
    const size_t decile =
        std::min<size_t>(9, static_cast<size_t>(10.0 * count / slots));
    const uint64_t t0 = rte_rdtsc();
    const int ret = rte_hash_add_key_data(h, &key, nullptr);
    const uint64_t dt = rte_rdtsc() - t0;
    if (ret == 0) {
      count++;
      consecutive = 0;
      sum[decile] += dt;
      n[decile]++;
      worst[decile] = std::max(worst[decile], dt);
    } else {
      if (!failed) {
        r.first_fail = count / slots;
        failed = true;
      }
      consecutive++;
    }
  }
  if (!failed) r.first_fail = count / slots;
  r.reachable = count / slots;
  for (int d = 0; d < 10; d++) {
    r.ns_mean[d] = n[d] ? sum[d] * ns_per_cycle / n[d] : 0;
    r.ns_max[d] = worst[d] * ns_per_cycle;
  }
  rte_hash_free(h);
  return r;
}

struct Crc64 {
  uint32_t operator()(const uint64_t &k) const {
    return rte_hash_crc_8byte(k, 0);
  }
};
struct Eq64 {
  bool operator()(const uint64_t &a, const uint64_t &b) const { return a == b; }
};
struct Inspectable : bess::utils::CuckooMap<uint64_t, uint64_t, Crc64, Eq64> {
  size_t buckets() const { return this->buckets_.size(); }
};

// Load (entries / (buckets * 4)) just before each bucket doubling, over the
// last few doublings of a fill to `target` keys, and the final load.
void FillCuckooMap(uint64_t target, Dist dist, uint64_t seed,
                   std::vector<double> *at_doubling, double *final_load) {
  Inspectable m;
  size_t buckets = m.buckets();
  for (uint64_t i = 0; i < target; i++) {
    const uint64_t key = KeyAt(dist, seed, i);
    m.Insert(key, i);
    if (m.buckets() != buckets) {
      // Count() includes the key that triggered the doubling.
      if (buckets >= 1024) {
        at_doubling->push_back((m.Count() - 1) / (buckets * 4.0));
      }
      buckets = m.buckets();
    }
  }
  *final_load = m.Count() / (m.buckets() * 4.0);
}

void RunFill(int max_log2) {
  std::printf("## fill: occupancy of key slots at the first failed add, and "
              "reachable after 64 consecutive failures\n\n");
  std::printf("| table | entries | keys | first failure (min / median / max) "
              "| reachable (median) |\n|---|---|---|---|---|\n");
  std::vector<std::pair<std::string, FillResult>> cost_rows;
  for (int lg = 10; lg <= max_log2; lg += 4) {
    const uint32_t pow2 = 1u << lg;
    for (Kind kind : {Kind::kPow2, Kind::kThreeQuarter, Kind::kExt}) {
      for (Dist dist : {Dist::kRandom, Dist::kSequential, Dist::kIpPort}) {
        std::vector<double> first, reach;
        FillResult last;
        for (uint64_t seed = 1; seed <= 5; seed++) {
          last = FillRteHash(kind, pow2, dist, seed);
          first.push_back(last.first_fail);
          reach.push_back(last.reachable);
        }
        std::sort(first.begin(), first.end());
        std::printf("| %s | %u | %s | %.1f%% / %.1f%% / %.1f%% | %.1f%% |\n",
                    KindName(kind),
                    kind == Kind::kThreeQuarter ? pow2 / 4 * 3 : pow2,
                    DistName(dist), 100 * first.front(), 100 * Median(first),
                    100 * first.back(), 100 * Median(reach));
        if (lg == max_log2 && dist == Dist::kRandom) {
          cost_rows.emplace_back(KindName(kind), last);
        }
      }
    }
  }
  std::printf("\n## fill: add cost by load (ns, mean / worst), %u entries, "
              "random keys, last seed\n\n| table |", 1u << max_log2);
  for (int d = 0; d < 10; d++) std::printf(" %d-%d%% |", d * 10, d * 10 + 10);
  std::printf("\n|---|");
  for (int d = 0; d < 10; d++) std::printf("---|");
  std::printf("\n");
  for (const auto &[name, r] : cost_rows) {
    std::printf("| %s |", name.c_str());
    for (int d = 0; d < 10; d++) {
      if (r.ns_mean[d] == 0) {
        std::printf(" - |");
      } else {
        std::printf(" %.0f / %.0f |", r.ns_mean[d], r.ns_max[d]);
      }
    }
    std::printf("\n");
  }

  std::printf("\n## CuckooMap: load at each bucket doubling (tables >= 1K "
              "buckets), filling to 2^%d keys\n\n| keys | load at doubling "
              "(min / median / max) | final load |\n|---|---|---|\n",
              max_log2);
  for (Dist dist : {Dist::kRandom, Dist::kSequential, Dist::kIpPort}) {
    std::vector<double> at;
    std::vector<double> finals;
    for (uint64_t seed = 1; seed <= 5; seed++) {
      double final_load;
      FillCuckooMap(1ull << max_log2, dist, seed, &at, &final_load);
      finals.push_back(final_load);
    }
    std::sort(at.begin(), at.end());
    if (at.empty()) {
      std::printf("| %s | - | %.1f%% |\n", DistName(dist),
                  100 * Median(finals));
      continue;
    }
    std::printf("| %s | %.1f%% / %.1f%% / %.1f%% | %.1f%% |\n",
                DistName(dist), 100 * at.front(), 100 * Median(at),
                100 * at.back(), 100 * Median(finals));
  }
}

// -- churn -----------------------------------------------------------------

// Replace keys at a steady load: each operation erases a random live key
// when the table is at the target, then adds a new key. A failed add leaves
// the table below target until a later add succeeds. `quiesce_every` = 0: no
// reader registered (a deleted slot is reclaimable at once); otherwise one
// reader is online and reports quiescence every that many operations, so
// deleted slots wait out a grace period as they do with live workers.
struct ChurnResult {
  double fails_per_million = 0;
  double mean_load = 0;  // of key slots, over the run
};

ChurnResult Churn(Kind kind, uint32_t pow2, double load,
                  uint64_t quiesce_every, Dist dist, uint64_t seed) {
  rte_hash *h = Create(kind, pow2);
  const uint64_t slots = kind == Kind::kThreeQuarter ? pow2 / 4 * 3 : pow2;
  const uint64_t target = static_cast<uint64_t>(load * slots);
  bess::rcu::RcuDomain &domain = bess::control::runtime().rcu();
  constexpr uint32_t kReader = 30;
  if (quiesce_every) {
    if (!domain.Register(kReader).has_value()) std::abort();
    domain.Online(kReader);
  }
  std::vector<uint64_t> live;
  live.reserve(target);
  uint64_t next = 0;
  while (live.size() < target && next < 4ull * pow2) {
    const uint64_t key = KeyAt(dist, seed, next++);
    if (rte_hash_add_key_data(h, &key, nullptr) == 0) live.push_back(key);
  }
  constexpr uint64_t kOps = 1000000;
  uint64_t fails = 0, rng = seed;
  double load_sum = 0;
  for (uint64_t op = 0; op < kOps; op++) {
    if (live.size() >= target && !live.empty()) {
      rng = SplitMix(rng);
      const size_t at = rng % live.size();
      rte_hash_del_key(h, &live[at]);
      live[at] = live.back();
      live.pop_back();
    }
    const uint64_t key = KeyAt(dist, seed, next++);
    if (rte_hash_add_key_data(h, &key, nullptr) == 0) {
      live.push_back(key);
    } else {
      fails++;
    }
    load_sum += static_cast<double>(live.size()) / slots;
    if (quiesce_every && op % quiesce_every == 0) domain.Quiescent(kReader);
  }
  if (quiesce_every) {
    domain.Offline(kReader);
    domain.Unregister(kReader);
  }
  rte_hash_free(h);
  return {.fails_per_million = static_cast<double>(fails) * 1e6 / kOps,
          .mean_load = load_sum / kOps};
}

void RunChurn(int max_log2) {
  for (const uint32_t pow2 : {1u << 10, 1u << std::min(max_log2, 16)}) {
  std::printf("\n## churn: failed adds per million replacements (and the mean "
              "load held), %u entries, median of 3 seeds\n\n| table | keys | "
              "target load | no reader | reader quiescent every 64 ops | "
              "every 1024 ops |\n|---|---|---|---|---|---|\n",
              pow2);
  for (Kind kind : {Kind::kPow2, Kind::kThreeQuarter, Kind::kExt}) {
    for (Dist dist : {Dist::kRandom, Dist::kSequential}) {
      for (double load : {0.75, 0.85, 0.90, 0.95}) {
        std::printf("| %s | %s | %.0f%% |", KindName(kind), DistName(dist),
                    100 * load);
        for (uint64_t q : {uint64_t{0}, uint64_t{64}, uint64_t{1024}}) {
          std::vector<double> f, l;
          for (uint64_t seed = 1; seed <= 3; seed++) {
            const ChurnResult r = Churn(kind, pow2, load, q, dist, seed);
            f.push_back(r.fails_per_million);
            l.push_back(r.mean_load);
          }
          std::printf(" %.0f (%.1f%%) |", Median(f), 100 * Median(l));
        }
        std::printf("\n");
      }
    }
  }
  }
}

}  // namespace

int main(int argc, char **argv) {
  const std::string what = argc > 1 ? argv[1] : "all";
  const int max_log2 = argc > 2 ? std::atoi(argv[2]) : 20;
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  bess::InitDpdk();
  if (what == "fill" || what == "all") RunFill(max_log2);
  if (what == "churn" || what == "all") RunChurn(max_log2);
  return 0;
}
