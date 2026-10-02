// SPDX-License-Identifier: BSD-3-Clause

// Flow-state tables (roadmap M9, Decision D-052).
//
// What is measured, and what is deliberately not:
//
//  * BM_WorkerLookup   WorkerFlowTable::Find / FindBatch, per key width, State
//                      size, table size, access distribution and batch size.
//  * BM_Baseline*      the same key streams through the backends the roadmap
//                      says to evaluate before writing a table: CuckooMap,
//                      rte_hash in position mode, ConcurrentExactTable,
//                      std::unordered_map, and this library's FlowIndex used
//                      raw (no slot record, no generation), which isolates
//                      what the typed wrapper itself adds.
//  * BM_WorkerChurn    lookups with 1% / 10% of operations replaced by a
//                      create + erase pair, and create/erase flat out.
//  * BM_WorkerCreateEraseLatency  per-operation latency tail of the above.
//  * BM_SharedReaders  N threads reading one SharedFlowTable, against N
//                      per-thread WorkerFlowTables partitioning the same keys.
//  * BM_SharedReaderWriter  readers plus a writer churning flows: reader
//                      throughput, writer rate and latency tail, reclamation
//                      backlog.
//
// Each lookup reads State::counter, so a hit pays for reaching the State, not
// just for finding its address. Query streams are precomputed (262,144
// queries, replayed): the loop does not generate keys. "cycles" are TSC ticks,
// not core cycles (the TSC runs at a fixed rate; core clocks vary), so they
// are comparable only with each other.
//
// Table sizes are bounded by this machine's RAM; see docs/flow-state.md for
// the sizes run and the ones that were not.

// rte_hash_rcu_qsbr_add is experimental in DPDK 25.11.
#define ALLOW_EXPERIMENTAL_API 1

#include <benchmark/benchmark.h>

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <rte_hash.h>
#include <rte_malloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>

#include "classifier/concurrent_exact.h"
#include "dpdk.h"
#include "flow/flow_index.h"
#include "flow/shared_flow_table.h"
#include "flow/worker_flow_table.h"
#include "rcu/rcu_domain.h"
#include "utils/cuckoo_map.h"

namespace {

using bess::flow::DefaultFlowEqual;
using bess::flow::DefaultFlowHash;
using bess::flow::DefaultFlowTableTraits;
using bess::flow::SharedFlowTable;
using bess::flow::UncheckedOwner;
using bess::flow::WorkerFlowTable;

// -- keys and states ------------------------------------------------------------

template <size_t N>
struct KeyN {
  uint64_t w[N / 8];
};
using Key8 = KeyN<8>;
using Key16 = KeyN<16>;
using Key32 = KeyN<32>;
// A tunnel key plus the inner five-tuple: the "larger realistic appliance key".
using Key48 = KeyN<48>;

// A representative five-tuple. Thirteen bytes of fields and three explicit
// pad bytes that are always zero, so its bytes are canonical.
struct FiveTuple {
  uint32_t src, dst;
  uint16_t sport, dport;
  uint8_t proto;
  uint8_t pad[3];
};
static_assert(sizeof(FiveTuple) == 16);

template <size_t N>
struct StateN {
  StateN() : counter(0) {}
  explicit StateN(uint64_t c) : counter(c) {}
  uint64_t counter;
  std::byte pad[N - 8] = {};
};

// 16 bytes of table State pointing at 256 bytes elsewhere: "indirect larger
// state", the application-managed second array.
struct IndirectState {
  IndirectState() : counter(0), index(0) {}
  explicit IndirectState(uint64_t c) : counter(c), index(static_cast<uint32_t>(c)) {}
  uint64_t counter;
  uint32_t index;
  uint32_t pad = 0;
};
struct Record256 {
  uint64_t words[32];
};

uint64_t SplitMix(uint64_t& s) {
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// A distinct key for every id; all bytes populated, pad bytes zero.
template <typename K>
K MakeKey(uint64_t id) {
  K k;
  std::memset(&k, 0, sizeof(K));
  uint64_t s = id * 0x2545f4914f6cdd1dull + 1;
  auto* bytes = reinterpret_cast<unsigned char*>(&k);
  for (size_t off = 0; off + 8 <= sizeof(K); off += 8) {
    const uint64_t w = SplitMix(s);
    std::memcpy(bytes + off, &w, 8);
  }
  if constexpr (std::is_same_v<K, FiveTuple>) {
    k.proto = 6;
    std::memset(k.pad, 0, sizeof(k.pad));
  }
  return k;
}

struct NoOwnerTraits : DefaultFlowTableTraits {
  using Owner = UncheckedOwner;
};
// The batch stages with the slot-record prefetch, to show what it buys.
struct SlotPrefetchTraits : NoOwnerTraits {
  static constexpr bool kPrefetchSlots = true;
};

template <typename K>
struct StdHash {
  size_t operator()(const K& k) const noexcept { return DefaultFlowHash<K>{}(k); }
};
template <typename K>
struct StdEq {
  bool operator()(const K& a, const K& b) const noexcept {
    return DefaultFlowEqual<K>{}(a, b);
  }
};

// -- access patterns ------------------------------------------------------------

enum Dist : int { kHot = 0, kZipf = 1, kUniform = 2, kMiss = 3, kMixed = 4 };
const char* DistName(Dist d) {
  static const char* names[] = {"hot1", "zipf", "uniform", "miss", "mix50"};
  return names[d];
}

constexpr size_t kStream = 1 << 18;

// kStream queries over a table of ids [0, n): ids >= n are absent.
template <typename K>
std::vector<K> BuildStream(Dist dist, size_t n, uint64_t seed = 1) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::vector<K> stream;
  stream.reserve(kStream);
  for (size_t q = 0; q < kStream; q++) {
    uint64_t id = 0;
    switch (dist) {
      case kHot:
        id = 0;
        break;
      case kZipf:
        // Zipf(1) by inverse transform of the log-uniform approximation:
        // rank r has probability ~ 1/r, so a few flows take most lookups.
        id = std::min<uint64_t>(
            n - 1, static_cast<uint64_t>(std::pow(static_cast<double>(n), unit(rng))) - 1);
        break;
      case kUniform:
        id = rng() % n;
        break;
      case kMiss:
        id = n + rng() % n;
        break;
      case kMixed:
        id = (rng() & 1) ? rng() % n : n + rng() % n;
        break;
    }
    stream.push_back(MakeKey<K>(id));
  }
  return stream;
}

inline uint64_t Tsc() { return __rdtsc(); }

// Serialised timestamps for timing a single operation: rdtsc alone can be
// executed early or late relative to the work it brackets. Their own cost is
// measured once and subtracted.
inline uint64_t TscBegin() {
  _mm_lfence();
  const uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}
inline uint64_t TscEnd() {
  unsigned aux;
  const uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}
uint32_t TscOverhead() {
  static const uint32_t overhead = [] {
    std::vector<uint32_t> samples(10001);
    for (auto& sample : samples) {
      const uint64_t a = TscBegin();
      const uint64_t b = TscEnd();
      sample = static_cast<uint32_t>(b - a);
    }
    std::nth_element(samples.begin(), samples.begin() + 5000, samples.end());
    return samples[5000];
  }();
  return overhead;
}
inline uint32_t Elapsed(uint64_t begin, uint64_t end) {
  const auto ticks = static_cast<uint32_t>(end - begin);
  return ticks > TscOverhead() ? ticks - TscOverhead() : 0;
}

void Report(benchmark::State& state, uint64_t lookups, uint64_t tsc_ticks,
            uint64_t hits, double bytes_per_flow) {
  state.SetItemsProcessed(static_cast<int64_t>(lookups));
  state.counters["ns_per_lookup"] = benchmark::Counter(
      static_cast<double>(lookups) * 1e-9,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["tsc_per_lookup"] =
      lookups ? static_cast<double>(tsc_ticks) / static_cast<double>(lookups) : 0;
  state.counters["hit_pct"] =
      lookups ? 100.0 * static_cast<double>(hits) / static_cast<double>(lookups) : 0;
  state.counters["bytes_per_flow"] = bytes_per_flow;
}

double EalHeapAllocated() {
  rte_malloc_socket_stats stats{};
  if (rte_malloc_get_socket_stats(0, &stats) != 0) return 0;
  return static_cast<double>(stats.heap_allocsz_bytes);
}

void EnsureDpdk() {
  if (!bess::IsDpdkInitialized()) bess::InitDpdk(2048);
}

// Only one fixture lives at a time: a 1M-flow table of any one shape is
// hundreds of megabytes, and this machine does not have room for the
// baselines' fixtures side by side. Building a fixture releases the previous
// one, whatever its type.
std::function<void()>& ReleasePrevious() {
  static std::function<void()> release;
  return release;
}
void Adopt(std::function<void()> release) {
  if (ReleasePrevious()) ReleasePrevious()();
  ReleasePrevious() = std::move(release);
}

// One table kept between benchmarks of the same shape (building 1M flows takes
// longer than measuring them). Benchmarks of one shape are registered
// consecutively.
template <typename K, typename S, typename Traits = NoOwnerTraits>
struct WorkerFixture {
  using Table = WorkerFlowTable<K, S, DefaultFlowHash<K>, DefaultFlowEqual<K>, Traits>;
  std::unique_ptr<Table> table;
  std::vector<Record256> records;  // used by IndirectState only
  size_t n = 0;
};

template <typename K, typename S, typename Traits>
WorkerFixture<K, S, Traits>& GetWorkerFixture(size_t n) {
  static std::unique_ptr<WorkerFixture<K, S, Traits>> cached;
  if (cached == nullptr || cached->n != n) {
    Adopt([] { cached.reset(); });
    auto fixture = std::make_unique<WorkerFixture<K, S, Traits>>();
    fixture->n = n;
    auto table = WorkerFixture<K, S, Traits>::Table::Create(n);
    if (!table.has_value()) {
      std::abort();
    }
    fixture->table = std::move(*table);
    for (uint64_t id = 0; id < n; id++) {
      fixture->table->Emplace(MakeKey<K>(id), id);
    }
    if constexpr (std::is_same_v<S, IndirectState>) {
      fixture->records.resize(n);
    }
    cached = std::move(fixture);
  }
  return *cached;
}

// -- BM_WorkerLookup -------------------------------------------------------------

template <typename K, typename S, typename Traits = NoOwnerTraits>
void BM_WorkerLookup(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const auto dist = static_cast<Dist>(state.range(1));
  state.SetLabel(DistName(dist));
  const size_t batch = static_cast<size_t>(state.range(2));
  auto& fixture = GetWorkerFixture<K, S, Traits>(n);
  auto& table = *fixture.table;
  const std::vector<K> stream = BuildStream<K>(dist, n);
  const double bytes_per_flow =
      static_cast<double>(table.memory_bytes()) / static_cast<double>(table.capacity()) +
      (std::is_same_v<S, IndirectState> ? sizeof(Record256) : 0);

  S* out[64];
  uint64_t pos = 0, sum = 0, hits = 0;
  const uint64_t t0 = Tsc();
  if (batch == 1) {
    for (auto _ : state) {
      S* s = table.Find(stream[pos]);
      pos = (pos + 1) & (kStream - 1);
      if (s != nullptr) {
        hits++;
        sum += s->counter;
        if constexpr (std::is_same_v<S, IndirectState>) {
          sum += fixture.records[s->index].words[0];
        }
      }
    }
  } else {
    for (auto _ : state) {
      const uint64_t mask = table.FindBatch(
          std::span<const K>(&stream[pos], batch), std::span<S*>(out, batch));
      pos = (pos + batch) & (kStream - 1);
      for (uint64_t m = mask; m != 0; m &= m - 1) {
        S* s = out[__builtin_ctzll(m)];
        hits++;
        sum += s->counter;
        if constexpr (std::is_same_v<S, IndirectState>) {
          sum += fixture.records[s->index].words[0];
        }
      }
    }
  }
  const uint64_t ticks = Tsc() - t0;
  benchmark::DoNotOptimize(sum);
  Report(state, state.iterations() * batch, ticks, hits, bytes_per_flow);
}

// -- baselines -------------------------------------------------------------------

template <typename K, typename S>
struct Slab {
  std::vector<S> states;
  explicit Slab(size_t n) {
    states.reserve(n);
    for (uint64_t id = 0; id < n; id++) states.emplace_back(id);
  }
};

// CuckooMap<K, State>: the existing worker-side table, State stored in the map.
template <typename K, typename S>
void BM_BaselineCuckoo(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const auto dist = static_cast<Dist>(state.range(1));
  state.SetLabel(DistName(dist));
  const size_t batch = static_cast<size_t>(state.range(2));
  using Map = bess::utils::CuckooMap<K, S, StdHash<K>, StdEq<K>>;
  static std::unique_ptr<Map> map;
  static size_t map_n = 0;
  if (map == nullptr || map_n != n) {
    Adopt([] { map.reset(); });
    // Half-full buckets of four, as the existing users size it.
    size_t buckets = 1;
    while (buckets * 4 < n * 2) buckets <<= 1;
    map = std::make_unique<Map>(buckets, n + 8);
    for (uint64_t id = 0; id < n; id++) map->Insert(MakeKey<K>(id), S(id));
    map_n = n;
  }
  const std::vector<K> stream = BuildStream<K>(dist, n);
  uint64_t pos = 0, sum = 0, hits = 0;
  const StdHash<K> hasher;
  const StdEq<K> eq;
  const uint64_t t0 = Tsc();
  if (batch == 1) {
    for (auto _ : state) {
      const auto* e = map->Find(stream[pos], hasher, eq);
      pos = (pos + 1) & (kStream - 1);
      if (e != nullptr) {
        hits++;
        sum += e->second.counter;
      }
    }
  } else {
    bess::utils::HashResult hashes[64];
    for (auto _ : state) {
      for (size_t i = 0; i < batch; i++) {
        hashes[i] = static_cast<bess::utils::HashResult>(hasher(stream[pos + i]));
        map->PrefetchBucketPrehashed(hashes[i]);
      }
      for (size_t i = 0; i < batch; i++) map->PrefetchEntryPrehashed(hashes[i]);
      for (size_t i = 0; i < batch; i++) {
        const auto* e = map->FindPrehashedAs(hashes[i], stream[pos + i], eq);
        if (e != nullptr) {
          hits++;
          sum += e->second.counter;
        }
      }
      pos = (pos + batch) & (kStream - 1);
    }
  }
  const uint64_t ticks = Tsc() - t0;
  benchmark::DoNotOptimize(sum);
  Report(state, state.iterations() * batch, ticks, hits,
         static_cast<double>(map->MemoryBytes()) / static_cast<double>(n));
}

// rte_hash in position mode (no concurrency flags, default CRC hash), State in
// a slab indexed by position.
template <typename K, typename S>
void BM_BaselineRteHash(benchmark::State& state) {
  EnsureDpdk();
  const size_t n = static_cast<size_t>(state.range(0));
  const auto dist = static_cast<Dist>(state.range(1));
  state.SetLabel(DistName(dist));
  const size_t batch = static_cast<size_t>(state.range(2));
  static rte_hash* hash = nullptr;
  static size_t hash_n = 0;
  static std::unique_ptr<Slab<K, S>> slab;
  static double bytes_per_flow = 0;
  if (hash == nullptr || hash_n != n) {
    Adopt([] {
      if (hash != nullptr) rte_hash_free(hash);
      hash = nullptr;
      slab.reset();
    });
    const double before = EalHeapAllocated();
    static int seq = 0;
    const std::string name = "flowbench_" + std::to_string(seq++);
    rte_hash_parameters p{};
    p.name = name.c_str();
    // rte_hash degrades well before it is full; the same 4/3 headroom the
    // classifier uses.
    p.entries = static_cast<uint32_t>(n + n / 3 + 8);
    p.key_len = sizeof(K);
    p.hash_func = nullptr;
    p.socket_id = 0;
    hash = rte_hash_create(&p);
    if (hash == nullptr) {
      state.SkipWithError("rte_hash_create failed");
      return;
    }
    slab = std::make_unique<Slab<K, S>>(p.entries);
    for (uint64_t id = 0; id < n; id++) {
      const K key = MakeKey<K>(id);
      const int pos = rte_hash_add_key(hash, &key);
      if (pos < 0) {
        state.SkipWithError("rte_hash_add_key failed");
        return;
      }
      slab->states[static_cast<size_t>(pos)].counter = id;
    }
    bytes_per_flow = (EalHeapAllocated() - before + static_cast<double>(p.entries * sizeof(S))) /
                     static_cast<double>(n);
    hash_n = n;
  }
  const std::vector<K> stream = BuildStream<K>(dist, n);
  uint64_t pos = 0, sum = 0, hits = 0;
  const uint64_t t0 = Tsc();
  if (batch == 1) {
    for (auto _ : state) {
      const int p = rte_hash_lookup(hash, &stream[pos]);
      pos = (pos + 1) & (kStream - 1);
      if (p >= 0) {
        hits++;
        sum += slab->states[static_cast<size_t>(p)].counter;
      }
    }
  } else {
    const void* keys[64];
    int32_t positions[64];
    for (auto _ : state) {
      for (size_t i = 0; i < batch; i++) keys[i] = &stream[pos + i];
      rte_hash_lookup_bulk(hash, keys, static_cast<uint32_t>(batch), positions);
      pos = (pos + batch) & (kStream - 1);
      for (size_t i = 0; i < batch; i++) {
        if (positions[i] >= 0) {
          hits++;
          sum += slab->states[static_cast<size_t>(positions[i])].counter;
        }
      }
    }
  }
  const uint64_t ticks = Tsc() - t0;
  benchmark::DoNotOptimize(sum);
  Report(state, state.iterations() * batch, ticks, hits, bytes_per_flow);
}

// The classifier's lock-free rte_hash table with the inline CRC kernel; value =
// slab index. Also the directory of SharedFlowTable.
template <typename K, typename S>
void BM_BaselineConcurrentExact(benchmark::State& state) {
  EnsureDpdk();
  const size_t n = static_cast<size_t>(state.range(0));
  const auto dist = static_cast<Dist>(state.range(1));
  state.SetLabel(DistName(dist));
  const size_t batch = static_cast<size_t>(state.range(2));
  static bess::rcu::RcuDomain domain(8);
  static std::unique_ptr<bess::classifier::ConcurrentExactTable> table;
  static size_t table_n = 0;
  static std::unique_ptr<Slab<K, S>> slab;
  static double bytes_per_flow = 0;
  if (table == nullptr || table_n != n) {
    Adopt([] {
      table.reset();
      slab.reset();
      domain.Drain();
    });
    const double before = EalHeapAllocated();
    auto created = bess::classifier::ConcurrentExactTable::Create(
        sizeof(K), bess::classifier::ConcurrentExactTable::CapacityFor(n), domain);
    if (!created.has_value()) {
      state.SkipWithError("ConcurrentExactTable::Create failed");
      return;
    }
    table = std::move(*created);
    slab = std::make_unique<Slab<K, S>>(n);
    for (uint64_t id = 0; id < n; id++) {
      const K key = MakeKey<K>(id);
      table->Upsert(bess::classifier::ConstBytes(
                        reinterpret_cast<const std::byte*>(&key), sizeof(K)),
                    id);
    }
    bytes_per_flow =
        (EalHeapAllocated() - before + static_cast<double>(n * sizeof(S))) / static_cast<double>(n);
    table_n = n;
  }
  const std::vector<K> stream = BuildStream<K>(dist, n);
  uint64_t pos = 0, sum = 0, hits = 0;
  uint64_t values[64];
  const uint64_t t0 = Tsc();
  for (auto _ : state) {
    const uint64_t mask = table->LookupBatch(
        bess::classifier::ConstBytes(
            reinterpret_cast<const std::byte*>(&stream[pos]), batch * sizeof(K)),
        sizeof(K), values, batch);
    pos = (pos + batch) & (kStream - 1);
    for (uint64_t m = mask; m != 0; m &= m - 1) {
      hits++;
      sum += slab->states[values[__builtin_ctzll(m)]].counter;
    }
  }
  const uint64_t ticks = Tsc() - t0;
  benchmark::DoNotOptimize(sum);
  Report(state, state.iterations() * batch, ticks, hits, bytes_per_flow);
}

// FlowIndex with the keys and states in two flat arrays and nothing else: the
// directory without WorkerFlowTable's slot record, generation or batch stages.
template <typename K, typename S>
void BM_BaselineFlowIndexRaw(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const auto dist = static_cast<Dist>(state.range(1));
  state.SetLabel(DistName(dist));
  const size_t batch = static_cast<size_t>(state.range(2));
  using bess::flow::detail::Bucket;
  using bess::flow::detail::FlowIndex;
  static std::vector<Bucket> buckets;
  static std::vector<K> keys;
  static std::unique_ptr<Slab<K, S>> slab;
  static FlowIndex index;
  static size_t index_n = 0;
  if (index_n != n) {
    Adopt([] {
      std::vector<Bucket>().swap(buckets);
      std::vector<K>().swap(keys);
      slab.reset();
      index_n = 0;
    });
    const size_t count = FlowIndex::BucketsFor(n);
    buckets.assign(count, Bucket{});
    index = FlowIndex(buckets.data(), static_cast<uint32_t>(count));
    keys.clear();
    slab = std::make_unique<Slab<K, S>>(n);
    const DefaultFlowHash<K> hash;
    for (uint64_t id = 0; id < n; id++) {
      keys.push_back(MakeKey<K>(id));
      index.Insert(index.Split(hash(keys.back())), static_cast<uint32_t>(id));
    }
    index_n = n;
  }
  const std::vector<K> stream = BuildStream<K>(dist, n);
  const DefaultFlowHash<K> hash;
  const DefaultFlowEqual<K> eq;
  uint64_t pos = 0, sum = 0, hits = 0;
  const uint64_t t0 = Tsc();
  for (auto _ : state) {
    for (size_t i = 0; i < batch; i++) {
      const K& key = stream[pos + i];
      const uint32_t id = index.Find(index.Split(hash(key)), [&](uint32_t candidate) {
        return eq(keys[candidate], key);
      });
      if (id != FlowIndex::kNone) {
        hits++;
        sum += slab->states[id].counter;
      }
    }
    pos = (pos + batch) & (kStream - 1);
  }
  const uint64_t ticks = Tsc() - t0;
  benchmark::DoNotOptimize(sum);
  Report(state, state.iterations() * batch, ticks, hits,
         static_cast<double>(buckets.size() * sizeof(Bucket) + n * (sizeof(K) + sizeof(S))) /
             static_cast<double>(n));
}

template <typename K, typename S>
void BM_BaselineStdMap(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const auto dist = static_cast<Dist>(state.range(1));
  state.SetLabel(DistName(dist));
  static std::unique_ptr<std::unordered_map<K, S, StdHash<K>, StdEq<K>>> map;
  static size_t map_n = 0;
  if (map == nullptr || map_n != n) {
    Adopt([] { map.reset(); });
    map = std::make_unique<std::unordered_map<K, S, StdHash<K>, StdEq<K>>>();
    map->reserve(n);
    for (uint64_t id = 0; id < n; id++) map->emplace(MakeKey<K>(id), S(id));
    map_n = n;
  }
  const std::vector<K> stream = BuildStream<K>(dist, n);
  uint64_t pos = 0, sum = 0, hits = 0;
  const uint64_t t0 = Tsc();
  for (auto _ : state) {
    const auto it = map->find(stream[pos]);
    pos = (pos + 1) & (kStream - 1);
    if (it != map->end()) {
      hits++;
      sum += it->second.counter;
    }
  }
  const uint64_t ticks = Tsc() - t0;
  benchmark::DoNotOptimize(sum);
  Report(state, state.iterations(), ticks, hits, 0);
}

// -- churn -----------------------------------------------------------------------

// A pool of 2n keys; the live window is a sliding range of n * 3/4 of them.
// One step creates the key after the window and erases the one before it.
template <typename K, typename S>
void BM_WorkerChurn(benchmark::State& state) {
  const size_t capacity = static_cast<size_t>(state.range(0));
  const int churn_pct = static_cast<int>(state.range(1));  // 0 .. 100
  using Table = WorkerFlowTable<K, S, DefaultFlowHash<K>, DefaultFlowEqual<K>,
                                NoOwnerTraits>;
  auto created = Table::Create(capacity);
  if (!created.has_value()) {
    state.SkipWithError("Create failed");
    return;
  }
  auto& table = **created;
  const size_t window = capacity * 3 / 4;
  const size_t pool = capacity * 2;
  std::vector<K> keys(pool);
  for (uint64_t id = 0; id < pool; id++) keys[id] = MakeKey<K>(id);
  for (size_t i = 0; i < window; i++) table.Emplace(keys[i], i);
  size_t head = 0;  // oldest live key; live = [head, head + window) mod pool
  std::mt19937_64 rng(5);
  std::vector<uint32_t> offsets(kStream);
  for (auto& o : offsets) o = static_cast<uint32_t>(rng());
  constexpr size_t kBatch = 32;
  K batch_keys[kBatch];
  S* out[kBatch];
  uint64_t sum = 0, lookups = 0, steps = 0, off_pos = 0;
  const uint64_t t0 = Tsc();
  if (churn_pct >= 100) {
    for (auto _ : state) {
      table.Erase(keys[head]);
      table.Emplace(keys[(head + window) % pool], head);
      head = (head + 1) % pool;
      steps++;
    }
  } else {
    double debt = 0;
    const double per_batch = kBatch * churn_pct / 100.0;
    for (auto _ : state) {
      for (size_t i = 0; i < kBatch; i++) {
        const uint64_t r = (static_cast<uint64_t>(offsets[(off_pos + i) & (kStream - 1)]) * window) >> 32;
        batch_keys[i] = keys[(head + r) % pool];
      }
      off_pos += kBatch;
      const uint64_t mask = table.FindBatch(std::span<const K>(batch_keys, kBatch),
                                            std::span<S*>(out, kBatch));
      for (uint64_t m = mask; m != 0; m &= m - 1) sum += out[__builtin_ctzll(m)]->counter;
      lookups += kBatch;
      debt += per_batch;
      while (debt >= 1.0) {
        table.Erase(keys[head]);
        table.Emplace(keys[(head + window) % pool], head);
        head = (head + 1) % pool;
        steps++;
        debt -= 1.0;
      }
    }
  }
  const uint64_t ticks = Tsc() - t0;
  benchmark::DoNotOptimize(sum);
  const uint64_t ops = lookups + steps * 2;  // a create and an erase each
  state.SetItemsProcessed(static_cast<int64_t>(ops));
  state.counters["ns_per_op"] = benchmark::Counter(
      static_cast<double>(ops) * 1e-9,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["tsc_per_op"] = ops ? static_cast<double>(ticks) / static_cast<double>(ops) : 0;
  state.counters["Mcreate_erase_pairs_s"] =
      benchmark::Counter(static_cast<double>(steps) * 1e-6, benchmark::Counter::kIsRate);
  state.counters["bytes_per_flow"] =
      static_cast<double>(table.memory_bytes()) / static_cast<double>(table.capacity());
}

template <typename K, typename S>
void BM_WorkerCreateEraseLatency(benchmark::State& state) {
  const size_t capacity = static_cast<size_t>(state.range(0));
  using Table = WorkerFlowTable<K, S, DefaultFlowHash<K>, DefaultFlowEqual<K>,
                                NoOwnerTraits>;
  auto created = Table::Create(capacity);
  if (!created.has_value()) {
    state.SkipWithError("Create failed");
    return;
  }
  auto& table = **created;
  const size_t window = capacity * 3 / 4;
  const size_t pool = capacity * 2;
  std::vector<K> keys(pool);
  for (uint64_t id = 0; id < pool; id++) keys[id] = MakeKey<K>(id);
  for (size_t i = 0; i < window; i++) table.Emplace(keys[i], i);
  size_t head = 0;
  std::vector<uint32_t> create_ticks, erase_ticks;
  create_ticks.reserve(1 << 22);
  erase_ticks.reserve(1 << 22);
  for (auto _ : state) {
    const uint64_t a = TscBegin();
    table.Erase(keys[head]);
    const uint64_t b = TscEnd();
    const uint64_t c = TscBegin();
    table.Emplace(keys[(head + window) % pool], head);
    const uint64_t d = TscEnd();
    head = (head + 1) % pool;
    if (create_ticks.size() < create_ticks.capacity()) {
      erase_ticks.push_back(Elapsed(a, b));
      create_ticks.push_back(Elapsed(c, d));
    }
  }
  auto pct = [](std::vector<uint32_t>& v, double p) {
    if (v.empty()) return 0.0;
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return static_cast<double>(v[k]);
  };
  // TSC ticks; the TSC runs at the nominal rate printed with the results.
  state.counters["create_p50"] = pct(create_ticks, 0.50);
  state.counters["create_p99"] = pct(create_ticks, 0.99);
  state.counters["create_p999"] = pct(create_ticks, 0.999);
  state.counters["create_max"] = pct(create_ticks, 1.0);
  state.counters["erase_p50"] = pct(erase_ticks, 0.50);
  state.counters["erase_p99"] = pct(erase_ticks, 0.99);
  state.counters["erase_p999"] = pct(erase_ticks, 0.999);
  state.counters["erase_max"] = pct(erase_ticks, 1.0);
}

// -- aliases ---------------------------------------------------------------------

// Two ways to find one flow by either of two keys. Variant 0: one table whose
// flows carry an alias key (kAliases = 1). Variant 1: the application keeps a
// second table from the reverse key to a pointer at the first table's State,
// and keeps the two in step itself. (A third shape, a secondary index inside
// the first table, would cost what variant 1's second probe does and was not
// built.)
struct AliasTraits : NoOwnerTraits {
  static constexpr size_t kAliases = 1;
};
struct ConnPtr {
  ConnPtr() = default;
  explicit ConnPtr(StateN<32>* p) : conn(p) {}
  StateN<32>* conn = nullptr;
};

void BM_AliasLookup(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const int variant = static_cast<int>(state.range(1));
  const bool reverse = state.range(2) != 0;
  constexpr size_t kBatch = 32;
  using K = FiveTuple;
  using Aliased = WorkerFlowTable<K, StateN<32>, DefaultFlowHash<K>, DefaultFlowEqual<K>,
                                  AliasTraits>;
  using Plain = WorkerFlowTable<K, StateN<32>, DefaultFlowHash<K>, DefaultFlowEqual<K>,
                                NoOwnerTraits>;
  using Reverse = WorkerFlowTable<K, ConnPtr, DefaultFlowHash<K>, DefaultFlowEqual<K>,
                                  NoOwnerTraits>;
  std::unique_ptr<Aliased> one;
  std::unique_ptr<Plain> fwd;
  std::unique_ptr<Reverse> rev;
  double bytes = 0;
  if (variant == 0) {
    one = std::move(*Aliased::Create(n));
    for (uint64_t id = 0; id < n; id++) {
      one->EmplaceAliased(MakeKey<K>(id), MakeKey<K>(n + id), id);
    }
    bytes = static_cast<double>(one->memory_bytes()) / static_cast<double>(n);
  } else {
    fwd = std::move(*Plain::Create(n));
    rev = std::move(*Reverse::Create(n));
    for (uint64_t id = 0; id < n; id++) {
      StateN<32>* conn = fwd->Emplace(MakeKey<K>(id), id).state;
      rev->Emplace(MakeKey<K>(n + id), conn);
    }
    bytes = static_cast<double>(fwd->memory_bytes() + rev->memory_bytes()) /
            static_cast<double>(n);
  }
  std::mt19937_64 rng(3);
  std::vector<K> stream(kStream);
  for (auto& q : stream) q = MakeKey<K>((reverse ? n : 0) + rng() % n);
  StateN<32>* out[kBatch];
  ConnPtr* rout[kBatch];
  uint64_t pos = 0, sum = 0;
  const uint64_t t0 = Tsc();
  for (auto _ : state) {
    const std::span<const K> keys(&stream[pos], kBatch);
    pos = (pos + kBatch) & (kStream - 1);
    if (variant == 0) {
      const uint64_t mask = one->FindBatch(keys, std::span<StateN<32>*>(out, kBatch));
      for (uint64_t m = mask; m != 0; m &= m - 1) sum += out[__builtin_ctzll(m)]->counter;
    } else if (!reverse) {
      const uint64_t mask = fwd->FindBatch(keys, std::span<StateN<32>*>(out, kBatch));
      for (uint64_t m = mask; m != 0; m &= m - 1) sum += out[__builtin_ctzll(m)]->counter;
    } else {
      const uint64_t mask = rev->FindBatch(keys, std::span<ConnPtr*>(rout, kBatch));
      for (uint64_t m = mask; m != 0; m &= m - 1) sum += rout[__builtin_ctzll(m)]->conn->counter;
    }
  }
  const uint64_t ticks = Tsc() - t0;
  benchmark::DoNotOptimize(sum);
  state.SetLabel(std::string(variant == 0 ? "alias-in-table" : "second-table") +
                 (reverse ? " via-reverse-key" : " via-forward-key"));
  Report(state, state.iterations() * kBatch, ticks, state.iterations() * kBatch, bytes);
}

// Create and erase of a flow with two keys, flat out, over a sliding window.
void BM_AliasChurn(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const int variant = static_cast<int>(state.range(1));
  using K = FiveTuple;
  using Aliased = WorkerFlowTable<K, StateN<32>, DefaultFlowHash<K>, DefaultFlowEqual<K>,
                                  AliasTraits>;
  using Plain = WorkerFlowTable<K, StateN<32>, DefaultFlowHash<K>, DefaultFlowEqual<K>,
                                NoOwnerTraits>;
  using Reverse = WorkerFlowTable<K, ConnPtr, DefaultFlowHash<K>, DefaultFlowEqual<K>,
                                  NoOwnerTraits>;
  const size_t window = n * 3 / 4;
  const size_t pool = n * 2;
  std::vector<K> fk(pool), rk(pool);
  for (uint64_t id = 0; id < pool; id++) {
    fk[id] = MakeKey<K>(id);
    rk[id] = MakeKey<K>(pool + id);
  }
  std::unique_ptr<Aliased> one;
  std::unique_ptr<Plain> fwd;
  std::unique_ptr<Reverse> rev;
  if (variant == 0) {
    one = std::move(*Aliased::Create(n));
    for (size_t i = 0; i < window; i++) one->EmplaceAliased(fk[i], rk[i], i);
  } else {
    fwd = std::move(*Plain::Create(n));
    rev = std::move(*Reverse::Create(n));
    for (size_t i = 0; i < window; i++) rev->Emplace(rk[i], fwd->Emplace(fk[i], i).state);
  }
  size_t head = 0;
  uint64_t pairs = 0;
  for (auto _ : state) {
    const size_t add = (head + window) % pool;
    if (variant == 0) {
      one->Erase(fk[head]);
      one->EmplaceAliased(fk[add], rk[add], head);
    } else {
      rev->Erase(rk[head]);
      fwd->Erase(fk[head]);
      rev->Emplace(rk[add], fwd->Emplace(fk[add], head).state);
    }
    head = (head + 1) % pool;
    pairs++;
  }
  state.SetLabel(variant == 0 ? "alias-in-table" : "second-table");
  state.counters["Mflow_cycles_s"] =
      benchmark::Counter(static_cast<double>(pairs) * 1e-6, benchmark::Counter::kIsRate);
  state.counters["ns_per_flow_cycle"] = benchmark::Counter(
      static_cast<double>(pairs) * 1e-9,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// -- shared lookup ---------------------------------------------------------------

const cpu_set_t kInitialCpus = [] {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  return set;
}();

void Pin(int cpu);

// CPUs for `count` threads: those this process may use, except the one the
// benchmark's own (sleeping) thread is on. FLOW_BENCH_MAIN_CPU moves that
// thread to a CPU of the caller's choosing first, so a taskset mask of N+1
// CPUs gives N to the workers.
std::vector<int> WorkerCpus(size_t count) {
  std::vector<int> cpus;
  if (const char* main = std::getenv("FLOW_BENCH_MAIN_CPU")) Pin(std::atoi(main));
  const int main_cpu = sched_getcpu();
  for (int cpu = 0; cpu < CPU_SETSIZE && cpus.size() < count; cpu++) {
    if (cpu != main_cpu && CPU_ISSET(cpu, &kInitialCpus)) cpus.push_back(cpu);
  }
  return cpus;
}

void Pin(int cpu) {
  if (cpu < 0) return;
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(cpu, &one);
  pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
}

using SharedKey = Key16;
using SharedState = StateN<32>;

// Readers only. variant 0: one SharedFlowTable all readers share; variant 1:
// one WorkerFlowTable per reader (a partitioned table, the steerable case),
// each reader looking up its own partition's keys.
void BM_SharedReaders(benchmark::State& state) {
  EnsureDpdk();
  const size_t n = static_cast<size_t>(state.range(0));
  const int readers = static_cast<int>(state.range(1));
  const int variant = static_cast<int>(state.range(2));
  constexpr size_t kBatch = 32;
  bess::rcu::RcuDomain domain(64);
  using Shared = SharedFlowTable<SharedKey, SharedState>;
  using Part = WorkerFlowTable<SharedKey, SharedState, DefaultFlowHash<SharedKey>,
                               DefaultFlowEqual<SharedKey>, NoOwnerTraits>;
  std::unique_ptr<Shared> shared;
  std::vector<std::unique_ptr<Part>> parts;
  double slab_bytes_per_flow = 0;
  const double before = EalHeapAllocated();
  if (variant == 0) {
    auto created = Shared::Create(n, domain, 0);
    if (!created.has_value()) {
      state.SkipWithError("SharedFlowTable::Create failed");
      return;
    }
    shared = std::move(*created);
    for (uint64_t id = 0; id < n; id++) shared->Emplace(MakeKey<SharedKey>(id), id);
    slab_bytes_per_flow =
        (EalHeapAllocated() - before + static_cast<double>(shared->slab_bytes())) /
        static_cast<double>(n);
  } else {
    for (int r = 0; r < readers; r++) {
      auto created = Part::Create(n / static_cast<size_t>(readers));
      if (!created.has_value()) {
        state.SkipWithError("Create failed");
        return;
      }
      parts.push_back(std::move(*created));
    }
    for (uint64_t id = 0; id < n; id++) {
      parts[id % static_cast<size_t>(readers)]->Emplace(MakeKey<SharedKey>(id), id);
    }
    slab_bytes_per_flow = static_cast<double>(parts[0]->memory_bytes()) /
                          static_cast<double>(parts[0]->capacity());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::vector<uint64_t> done(static_cast<size_t>(readers), 0);
  std::vector<std::thread> pool;
  const std::vector<int> cpus = WorkerCpus(static_cast<size_t>(readers));
  for (int r = 0; r < readers; r++) {
    const auto id = static_cast<bess::rcu::ReaderId>(r);
    (void)domain.Register(id);
    pool.emplace_back([&, r, id] {
      Pin(static_cast<size_t>(r) < cpus.size() ? cpus[static_cast<size_t>(r)] : -1);
      domain.Online(id);
      std::mt19937_64 rng(r + 1);
      // This reader's queries: any key (shared) or its own partition's.
      std::vector<SharedKey> stream(kStream);
      for (auto& q : stream) {
        uint64_t key_id = rng() % n;
        if (variant == 1) key_id = key_id / static_cast<uint64_t>(readers) * static_cast<uint64_t>(readers) + static_cast<uint64_t>(r);
        q = MakeKey<SharedKey>(key_id % n);
      }
      SharedState* out[kBatch];
      const SharedState* cout_[kBatch];
      uint64_t pos = 0, sum = 0, count = 0;
      ready++;
      while (!stop.load(std::memory_order_relaxed)) {
        for (int rep = 0; rep < 8; rep++) {  // a task invocation's worth
          uint64_t mask;
          if (variant == 0) {
            mask = shared->PeekBatch(std::span<const SharedKey>(&stream[pos], kBatch),
                                     std::span<const SharedState*>(cout_, kBatch));
            for (uint64_t m = mask; m != 0; m &= m - 1) sum += cout_[__builtin_ctzll(m)]->counter;
          } else {
            mask = parts[static_cast<size_t>(r)]->FindBatch(
                std::span<const SharedKey>(&stream[pos], kBatch), std::span<SharedState*>(out, kBatch));
            for (uint64_t m = mask; m != 0; m &= m - 1) sum += out[__builtin_ctzll(m)]->counter;
          }
          pos = (pos + kBatch) & (kStream - 1);
          count += kBatch;
        }
        domain.Quiescent(id);
      }
      benchmark::DoNotOptimize(sum);
      done[static_cast<size_t>(r)] = count;
      domain.Offline(id);
    });
  }
  while (ready.load() < readers) std::this_thread::yield();
  const auto start = std::chrono::steady_clock::now();
  for (auto _ : state) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  stop = true;
  for (auto& th : pool) th.join();
  uint64_t total = 0;
  for (int r = 0; r < readers; r++) {
    total += done[static_cast<size_t>(r)];
    domain.Unregister(static_cast<bess::rcu::ReaderId>(r));
  }
  state.counters["Mlookups_s"] = static_cast<double>(total) / seconds / 1e6;
  state.counters["Mlookups_s_per_thread"] =
      static_cast<double>(total) / seconds / 1e6 / readers;
  state.counters["bytes_per_flow"] = slab_bytes_per_flow;
  state.SetLabel(variant == 0 ? "shared-directory" : "partitioned-worker-tables");
  shared.reset();
  parts.clear();
  domain.Drain();
}

// Readers plus one writer cycling a window of extra flows through a
// SharedFlowTable as fast as it can. Reader throughput against the no-writer
// case is the writer's interference; the writer's per-operation latency tail
// and the reclamation backlog are reported.
void BM_SharedReaderWriter(benchmark::State& state) {
  EnsureDpdk();
  const size_t n = static_cast<size_t>(state.range(0));
  const int readers = static_cast<int>(state.range(1));
  const bool writing = state.range(2) != 0;
  constexpr size_t kBatch = 32;
  constexpr size_t kWindow = 4096;
  bess::rcu::RcuDomain domain(64);
  using Shared = SharedFlowTable<SharedKey, SharedState>;
  auto created = Shared::Create(n + kWindow * 3, domain, 0);
  if (!created.has_value()) {
    state.SkipWithError("SharedFlowTable::Create failed");
    return;
  }
  auto& table = **created;
  for (uint64_t id = 0; id < n; id++) table.Emplace(MakeKey<SharedKey>(id), id);
  std::vector<SharedKey> churn_keys(kWindow * 4);
  for (size_t i = 0; i < churn_keys.size(); i++) churn_keys[i] = MakeKey<SharedKey>(n + i);

  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::vector<uint64_t> done(static_cast<size_t>(readers), 0);
  std::vector<std::thread> pool;
  const std::vector<int> cpus = WorkerCpus(static_cast<size_t>(readers) + 1);
  for (int r = 0; r < readers; r++) {
    const auto id = static_cast<bess::rcu::ReaderId>(r);
    (void)domain.Register(id);
    pool.emplace_back([&, r, id] {
      Pin(static_cast<size_t>(r) < cpus.size() ? cpus[static_cast<size_t>(r)] : -1);
      domain.Online(id);
      std::mt19937_64 rng(r + 1);
      std::vector<SharedKey> stream(kStream);
      for (auto& q : stream) q = MakeKey<SharedKey>(rng() % n);
      const SharedState* out[kBatch];
      uint64_t pos = 0, sum = 0, count = 0;
      ready++;
      while (!stop.load(std::memory_order_relaxed)) {
        for (int rep = 0; rep < 8; rep++) {
          const uint64_t mask = table.PeekBatch(
              std::span<const SharedKey>(&stream[pos], kBatch),
              std::span<const SharedState*>(out, kBatch));
          for (uint64_t m = mask; m != 0; m &= m - 1) sum += out[__builtin_ctzll(m)]->counter;
          pos = (pos + kBatch) & (kStream - 1);
          count += kBatch;
        }
        domain.Quiescent(id);
      }
      benchmark::DoNotOptimize(sum);
      done[static_cast<size_t>(r)] = count;
      domain.Offline(id);
    });
  }
  std::vector<uint32_t> create_ticks, erase_ticks;
  uint64_t writer_ops = 0, max_backlog = 0;
  std::thread writer;
  if (writing) {
    create_ticks.reserve(1 << 21);
    erase_ticks.reserve(1 << 21);
    writer = std::thread([&] {
      Pin(static_cast<size_t>(readers) < cpus.size() ? cpus[static_cast<size_t>(readers)] : -1);
      size_t head = 0;
      for (size_t i = 0; i < kWindow; i++) table.Emplace(churn_keys[i], i);
      ready++;
      const size_t pool_size = churn_keys.size();
      while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t a = TscBegin();
        table.Erase(churn_keys[head]);
        const uint64_t b = TscEnd();
        const uint64_t c = TscBegin();
        table.Emplace(churn_keys[(head + kWindow) % pool_size], head);
        const uint64_t d = TscEnd();
        head = (head + 1) % pool_size;
        writer_ops += 2;
        if (create_ticks.size() < create_ticks.capacity()) {
          erase_ticks.push_back(Elapsed(a, b));
          create_ticks.push_back(Elapsed(c, d));
        }
        if ((head & 63) == 0) {
          max_backlog = std::max<uint64_t>(max_backlog, table.pending_reclaim());
          table.Reclaim();
        }
      }
    });
  } else {
    ready++;
  }
  while (ready.load() < readers + 1) std::this_thread::yield();
  const auto start = std::chrono::steady_clock::now();
  for (auto _ : state) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  stop = true;
  for (auto& th : pool) th.join();
  if (writing) writer.join();
  uint64_t total = 0;
  for (int r = 0; r < readers; r++) {
    total += done[static_cast<size_t>(r)];
    domain.Unregister(static_cast<bess::rcu::ReaderId>(r));
  }
  auto pct = [](std::vector<uint32_t>& v, double p) {
    if (v.empty()) return 0.0;
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return static_cast<double>(v[k]);
  };
  state.counters["Mlookups_s"] = static_cast<double>(total) / seconds / 1e6;
  state.counters["Mwriter_ops_s"] = static_cast<double>(writer_ops) / seconds / 1e6;
  state.counters["max_backlog"] = static_cast<double>(max_backlog);
  state.counters["create_p50"] = pct(create_ticks, 0.50);
  state.counters["create_p99"] = pct(create_ticks, 0.99);
  state.counters["create_max"] = pct(create_ticks, 1.0);
  state.counters["erase_p50"] = pct(erase_ticks, 0.50);
  state.counters["erase_p99"] = pct(erase_ticks, 0.99);
  state.counters["erase_max"] = pct(erase_ticks, 1.0);
  state.SetLabel(writing ? "readers+writer" : "readers only");
  table.Reclaim();
  created->reset();
  domain.Drain();
}

// -- registration ----------------------------------------------------------------

constexpr benchmark::TimeUnit kUnit = benchmark::kNanosecond;

template <typename K, typename S, typename Traits = NoOwnerTraits>
void RegisterWorker(const std::string& tag, std::vector<int64_t> sizes,
                    std::vector<int64_t> dists, std::vector<int64_t> batches) {
  benchmark::RegisterBenchmark(("BM_WorkerLookup/" + tag).c_str(), BM_WorkerLookup<K, S, Traits>)
      ->ArgsProduct({sizes, dists, batches})
      ->Unit(kUnit)
      ->MinTime(0.25);
}

template <typename K, typename S>
void RegisterBaselines(const std::string& tag, std::vector<int64_t> sizes,
                       std::vector<int64_t> dists) {
  auto reg = [&](const char* name, auto fn, std::vector<int64_t> batches) {
    benchmark::RegisterBenchmark((std::string("BM_Baseline") + name + "/" + tag).c_str(), fn)
        ->ArgsProduct({sizes, dists, batches})
        ->Unit(kUnit)
        ->MinTime(0.25);
  };
  reg("Cuckoo", BM_BaselineCuckoo<K, S>, {1, 32});
  reg("RteHash", BM_BaselineRteHash<K, S>, {1, 32});
  reg("ConcurrentExact", BM_BaselineConcurrentExact<K, S>, {1, 32});
  reg("FlowIndexRaw", BM_BaselineFlowIndexRaw<K, S>, {1, 32});
  benchmark::RegisterBenchmark(("BM_BaselineStdMap/" + tag).c_str(), BM_BaselineStdMap<K, S>)
      ->ArgsProduct({sizes, dists})
      ->Unit(kUnit)
      ->MinTime(0.25);
}

[[maybe_unused]] const bool kRegistered = [] {
  const std::vector<int64_t> kSizes = {1 << 10, 1 << 16, 1 << 20};
  const std::vector<int64_t> kAllDists = {kHot, kZipf, kUniform, kMiss, kMixed};
  const std::vector<int64_t> kBatches = {1, 8, 16, 32};

  // Reference configuration: a 16-byte five-tuple and 32 bytes of State, over
  // every size, distribution and batch size.
  RegisterWorker<FiveTuple, StateN<32>>("tuple16_state32", kSizes, kAllDists, kBatches);

  RegisterWorker<FiveTuple, StateN<32>, SlotPrefetchTraits>(
      "tuple16_state32_slotprefetch", {1 << 16, 1 << 20}, {kUniform, kMiss}, {8, 16, 32});

  // Key width and State size sweeps at the sizes where caches stop helping.
  const std::vector<int64_t> kBig = {1 << 16, 1 << 20};
  RegisterWorker<Key8, StateN<32>>("key8_state32", kBig, {kUniform, kMiss}, {32});
  RegisterWorker<Key16, StateN<32>>("key16_state32", kBig, {kUniform, kMiss}, {32});
  RegisterWorker<Key32, StateN<32>>("key32_state32", kBig, {kUniform, kMiss}, {32});
  RegisterWorker<Key48, StateN<32>>("key48_state32", kBig, {kUniform, kMiss}, {32});
  RegisterWorker<FiveTuple, StateN<16>>("tuple16_state16", kBig, {kUniform}, {32});
  RegisterWorker<FiveTuple, StateN<64>>("tuple16_state64", kBig, {kUniform}, {32});
  RegisterWorker<FiveTuple, StateN<128>>("tuple16_state128", kBig, {kUniform}, {32});
  RegisterWorker<FiveTuple, IndirectState>("tuple16_indirect256", kBig, {kUniform}, {32});

  // 10M flows, only on request (FLOW_BENCH_LARGE=1) and only with the smallest
  // key and State: about 0.5 GB of table plus the fixture's transient peak.
  if (std::getenv("FLOW_BENCH_LARGE") != nullptr) {
    RegisterWorker<Key8, StateN<16>>("key8_state16_10M", {10'000'000},
                                     {kUniform, kMiss}, {1, 32});
  }

  // The backends the roadmap says to evaluate first, on the same streams.
  RegisterBaselines<FiveTuple, StateN<32>>("tuple16_state32", kBig, {kUniform, kMiss});
  RegisterBaselines<Key8, StateN<32>>("key8_state32", {1 << 20}, {kUniform});

  // Churn: 1%, 10% and flat-out create/erase, 64K and 1M capacity.
  for (int64_t size : {int64_t{1} << 16, int64_t{1} << 20}) {
    for (int64_t pct : {1, 10, 100}) {
      benchmark::RegisterBenchmark("BM_WorkerChurn/tuple16_state32",
                                   BM_WorkerChurn<FiveTuple, StateN<32>>)
          ->Args({size, pct})
          ->Unit(kUnit)
          ->MinTime(0.25);
    }
    benchmark::RegisterBenchmark("BM_WorkerCreateEraseLatency/tuple16_state32",
                                 BM_WorkerCreateEraseLatency<FiveTuple, StateN<32>>)
        ->Args({size})
        ->Unit(kUnit)
        ->MinTime(0.5);
  }

  // Alias representation: the key a reverse-direction packet carries.
  for (int64_t size : {int64_t{1} << 16, int64_t{1} << 20}) {
    for (int64_t variant : {0, 1}) {
      for (int64_t reverse : {0, 1}) {
        benchmark::RegisterBenchmark("BM_AliasLookup", BM_AliasLookup)
            ->Args({size, variant, reverse})
            ->Unit(kUnit)
            ->MinTime(0.25);
      }
      benchmark::RegisterBenchmark("BM_AliasChurn", BM_AliasChurn)
          ->Args({size, variant})
          ->Unit(kUnit)
          ->MinTime(0.25);
    }
  }

  // Shared lookup: readers 1/2/4/8, shared directory and partitioned tables.
  for (int64_t size : {int64_t{1} << 16, int64_t{1} << 20}) {
    for (int64_t readers : {1, 2, 4, 8}) {
      for (int64_t variant : {0, 1}) {
        benchmark::RegisterBenchmark("BM_SharedReaders", BM_SharedReaders)
            ->Args({size, readers, variant})
            ->UseRealTime()
            ->Iterations(200);
      }
    }
    for (int64_t readers : {1, 4}) {
      for (int64_t writing : {0, 1}) {
        benchmark::RegisterBenchmark("BM_SharedReaderWriter", BM_SharedReaderWriter)
            ->Args({size, readers, writing})
            ->UseRealTime()
            ->Iterations(200);
      }
    }
  }
  return true;
}();

}  // namespace
