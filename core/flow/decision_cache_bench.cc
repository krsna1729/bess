// SPDX-License-Identifier: BSD-3-Clause

// DecisionCache (M12, D-062) costs, one worker thread.
//
//   BM_DecisionLookup/<n>/<stream>/<batch>  ns per lookup; stream 0 one hot
//       key, 1 uniform hit, 2 miss, 3 half hit half miss, 4 half the entries
//       stale (invalidated, not yet reinstalled)
//   BM_FlowBaseline/<n>/<stream>/<batch>     the same streams through the
//       underlying WorkerFlowTable alone: the difference is the cache's own
//       cost (the generation compare and the id copy)
//   BM_DecisionResolve/<n>/<bytes>           uniform hit, then the decision
//       object resolved through a SlotTable and one field read
//   BM_DecisionChurn/<n>                     ns per install-or-replace of a
//       uniformly chosen key after an invalidate (the recompile path's cache
//       work, compile cost excluded)

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"
#include "flow/decision_cache.h"

namespace {

using bess::flow::DecisionCache;
using bess::flow::DecisionGeneration;
using bess::flow::DecisionInstall;
using bess::flow::DecisionLookup;

struct Tuple {
  uint32_t src, dst;
  uint16_t sport, dport;
  uint8_t proto;
  uint8_t pad[3];
};

Tuple T(uint32_t i) { return Tuple{i, i * 2654435761u, static_cast<uint16_t>(i), 443, 6, {}}; }

struct DecIdTag;
using DecId = bess::dataplane::StrongId<DecIdTag, uint32_t>;
using Cache = DecisionCache<Tuple, DecId>;

constexpr size_t kStream = size_t{1} << 18;

enum Stream : int64_t { kHot, kUniform, kMiss, kMix, kStale };

std::vector<Tuple> BuildStream(int64_t stream, uint32_t n) {
  std::mt19937 rng(0xdc12);
  std::vector<Tuple> keys(kStream);
  for (size_t i = 0; i < kStream; i++) {
    switch (stream) {
      case kHot:
        keys[i] = T(1);
        break;
      case kMiss:
        keys[i] = T(n + rng() % n);
        break;
      case kMix:
        keys[i] = T(rng() % (2 * n));  // installed keys are 0..n-1
        break;
      default:
        keys[i] = T(rng() % n);
    }
  }
  return keys;
}

// One cache per size, filled once; the kStale stream invalidates and then
// reinstalls the odd keys, so half the entries are stale.
struct Fixture {
  DecisionGeneration generation;
  std::unique_ptr<Cache> cache;
  uint32_t n = 0;
};

Fixture &CacheFor(uint32_t n, bool half_stale) {
  static Fixture fixtures[2];
  Fixture &f = fixtures[half_stale];
  if (f.n != n) {
    f.cache.reset();
    f.cache = Cache::Create(n, f.generation).value();
    for (uint32_t i = 0; i < n; i++) {
      (void)f.cache->Install(T(i), DecId(i + 1), f.generation.Current());
    }
    if (half_stale) {
      f.generation.Invalidate();
      for (uint32_t i = 1; i < n; i += 2) {
        (void)f.cache->Install(T(i), DecId(i + 1), f.generation.Current());
      }
    }
    f.n = n;
  }
  return f;
}

void BM_DecisionLookup(benchmark::State &state) {
  const auto n = static_cast<uint32_t>(state.range(0));
  const int64_t stream = state.range(1);
  const auto batch = static_cast<size_t>(state.range(2));
  Fixture &f = CacheFor(n, stream == kStale);
  const std::vector<Tuple> keys = BuildStream(stream, n);
  DecId out[Cache::kMaxBatch];
  size_t pos = 0;
  uint64_t hits = 0;
  if (batch == 1) {
    for (auto _ : state) {
      DecId id;
      hits += f.cache->Lookup(keys[pos], &id) == DecisionLookup::kHit;
      benchmark::DoNotOptimize(id);
      pos = (pos + 1) & (kStream - 1);
    }
  } else {
    for (auto _ : state) {
      hits += static_cast<uint64_t>(__builtin_popcountll(f.cache->LookupBatch(
          std::span<const Tuple>(&keys[pos], batch), std::span<DecId>(out, batch))));
      benchmark::DoNotOptimize(out);
      pos = (pos + batch) & (kStream - 1);
    }
  }
  const double lookups = static_cast<double>(state.iterations()) * batch;
  state.counters["ns_per_lookup"] =
      benchmark::Counter(lookups, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["hit_pct"] = 100.0 * static_cast<double>(hits) / lookups;
  state.counters["bytes_per_entry"] =
      static_cast<double>(f.cache->memory_bytes()) / static_cast<double>(n);
}

void BM_FlowBaseline(benchmark::State &state) {
  using Table = Cache::Table;
  const auto n = static_cast<uint32_t>(state.range(0));
  const int64_t stream = state.range(1);
  const auto batch = static_cast<size_t>(state.range(2));
  static std::unique_ptr<Table> table;
  static uint32_t built = 0;
  if (built != n) {
    table.reset();
    table = Table::Create(n).value();
    for (uint32_t i = 0; i < n; i++) {
      (void)table->Emplace(T(i), Cache::Entry{DecId(i + 1), 1});
    }
    built = n;
  }
  const std::vector<Tuple> keys = BuildStream(stream, n);
  const Cache::Entry *out[Cache::kMaxBatch];
  size_t pos = 0;
  uint64_t sum = 0;
  if (batch == 1) {
    for (auto _ : state) {
      const Cache::Entry *e = table->Find(keys[pos]);
      if (e != nullptr) {
        sum += e->id.value();
      }
      pos = (pos + 1) & (kStream - 1);
    }
  } else {
    for (auto _ : state) {
      const uint64_t m = table->FindBatch(std::span<const Tuple>(&keys[pos], batch),
                                          std::span<const Cache::Entry *>(out, batch));
      for (uint64_t b = m; b != 0; b &= b - 1) {
        sum += out[__builtin_ctzll(b)]->id.value();
      }
      pos = (pos + batch) & (kStream - 1);
    }
  }
  benchmark::DoNotOptimize(sum);
  state.counters["ns_per_lookup"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * batch,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

template <size_t kBytes>
struct Decision {
  uint32_t first;
  uint8_t rest[kBytes - sizeof(uint32_t)];
};

template <size_t kBytes>
void BM_DecisionResolve(benchmark::State &state) {
  const auto n = static_cast<uint32_t>(state.range(0));
  Fixture &f = CacheFor(n, false);
  bess::dataplane::SlotTable<DecId, Decision<kBytes>> decisions(n);
  for (uint32_t i = 0; i < n; i++) {
    auto d = std::make_unique<Decision<kBytes>>();
    d->first = i;
    (void)decisions.Publish(DecId(i + 1), std::move(d));
  }
  const std::vector<Tuple> keys = BuildStream(kUniform, n);
  size_t pos = 0;
  uint64_t sum = 0;
  for (auto _ : state) {
    DecId id;
    if (f.cache->Lookup(keys[pos], &id) == DecisionLookup::kHit) {
      sum += decisions.Lookup(id)->first;
    }
    pos = (pos + 1) & (kStream - 1);
  }
  benchmark::DoNotOptimize(sum);
}

void BM_DecisionChurn(benchmark::State &state) {
  const auto n = static_cast<uint32_t>(state.range(0));
  DecisionGeneration generation;
  auto cache = Cache::Create(n, generation).value();
  std::mt19937 rng(0xc4u);
  std::vector<uint32_t> picks(kStream);
  for (auto &p : picks) {
    p = rng() % n;
  }
  size_t pos = 0;
  for (auto _ : state) {
    const uint32_t k = picks[pos];
    if (cache->Install(T(k), DecId(k + 1), generation.Current()) == DecisionInstall::kFull) {
      state.SkipWithError("full");
      break;
    }
    if ((pos & 1023) == 0) {
      generation.Invalidate();  // a policy change every 1,024 installs
    }
    pos = (pos + 1) & (kStream - 1);
  }
}

void Register() {
  for (int64_t n : {int64_t{1} << 16, int64_t{1} << 20}) {
    for (int64_t s : {kHot, kUniform, kMiss, kMix, kStale}) {
      for (int64_t b : {1, 32}) {
        benchmark::RegisterBenchmark("BM_DecisionLookup", BM_DecisionLookup)
            ->Args({n, s, b})->MinTime(0.25);
        if (s != kStale) {
          benchmark::RegisterBenchmark("BM_FlowBaseline", BM_FlowBaseline)
              ->Args({n, s, b})->MinTime(0.25);
        }
      }
    }
    benchmark::RegisterBenchmark("BM_DecisionResolve/16", BM_DecisionResolve<16>)->Arg(n)->MinTime(0.25);
    benchmark::RegisterBenchmark("BM_DecisionResolve/64", BM_DecisionResolve<64>)->Arg(n)->MinTime(0.25);
    benchmark::RegisterBenchmark("BM_DecisionResolve/256", BM_DecisionResolve<256>)->Arg(n)->MinTime(0.25);
    benchmark::RegisterBenchmark("BM_DecisionChurn", BM_DecisionChurn)->Arg(n)->MinTime(0.25);
  }
}
[[maybe_unused]] const bool kRegistered = (Register(), true);

}  // namespace
