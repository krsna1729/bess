// SPDX-License-Identifier: BSD-3-Clause

// M16 (D-066): member selection.
//
//   BM_Select/<kind>/<members>   ns per selection over a stream of random
//     hashes. kind 0 range, 1 weighted (weights 1..100), 2 Maglev 65,537
//     entries, 3 Maglev with a table of about 101 entries per member,
//     4 rendezvous, 5 AnySelector(Maglev 65,537) through SelectBatch (32).
//     Counters: bytes per group, max_over_min (the most-chosen member's share
//     over the least-chosen's, 2^24 hashes (2^20 for rendezvous); for
//     weighted, normalised by
//     weight), churn_pct (of flows on members that stay, the percentage that
//     move when the middle member is removed; weighted: when its weight goes
//     to zero).
//   BM_HashLbRange/<form>   HashLB's mapping, 0 the former floating-point form,
//     1 RangeSelect.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#include "dataplane/member_select.h"

namespace {

using namespace bess::dataplane;

constexpr size_t kStream = size_t{1} << 16;

std::vector<uint32_t> Hashes() {
  std::mt19937 rng(0x16);
  std::vector<uint32_t> h(kStream);
  for (auto &x : h) x = rng();
  return h;
}

uint32_t NextPrime(uint32_t v) {
  auto prime = [](uint32_t x) {
    if (x < 2) return false;
    for (uint32_t d = 2; uint64_t{d} * d <= x; d++)
      if (x % d == 0) return false;
    return true;
  };
  while (!prime(v)) v++;
  return v;
}

std::vector<uint64_t> Keys(size_t n) {
  std::vector<uint64_t> k(n);
  std::iota(k.begin(), k.end(), 1000);
  return k;
}
std::vector<uint32_t> Weights(size_t n) {
  std::mt19937 rng(0x17);
  std::vector<uint32_t> w(n);
  for (auto &x : w) x = 1 + rng() % 100;
  return w;
}

AnySelector Make(int kind, size_t n, bool drop_middle) {
  std::vector<uint64_t> keys = Keys(n);
  std::vector<uint32_t> w = Weights(n);
  if (drop_middle) {
    keys.erase(keys.begin() + n / 2);
    w[n / 2] = 0;
  }
  switch (kind) {
    case 0:
      return AnySelector(RangeSelector{static_cast<uint32_t>(keys.size())});
    case 1:
      return AnySelector(WeightedSelector::Build(w).value());
    case 2:
    case 5:
      return AnySelector(MaglevSelector::Build(keys).value());
    case 3:
      return AnySelector(MaglevSelector::Build(keys, NextPrime(static_cast<uint32_t>(101 * n))).value());
    default:
      return AnySelector(RendezvousSelector::Build(keys).value());
  }
}

// Member identity after a removal: index i of the shorter list is original
// member i, or i + 1 past the removed one (weighted keeps every index).
uint32_t Original(int kind, size_t n, uint32_t i) {
  return kind == 1 || i < n / 2 ? i : i + 1;
}

void Quality(benchmark::State &state, int kind, size_t n, const AnySelector &s) {
  // 2^24 hashes (2^20 for rendezvous, which costs O(n) a selection): with
  // 2^20 the sample itself moved Maglev's 128-member ratio to 1.05, where its
  // table is balanced to one entry in 512 (1.005 at 2^24).
  std::vector<double> count(n, 0);
  const uint32_t samples = kind == 4 ? (1u << 20) : (1u << 24);
  for (uint32_t i = 0; i < samples; i++) {
    count[s.Select(i * 2654435761u)]++;
  }
  if (kind == 1) {
    const auto w = Weights(n);
    for (size_t i = 0; i < n; i++) count[i] /= w[i];
  }
  const auto [lo, hi] = std::minmax_element(count.begin(), count.end());
  state.counters["max_over_min"] = *lo > 0 ? *hi / *lo : 0;
  if (n < 2) return;
  const AnySelector fewer = Make(kind, n, true);
  uint64_t stay = 0, moved = 0;
  for (uint32_t i = 0; i < (1u << 18); i++) {
    const uint32_t h = i * 2654435761u;
    const uint32_t before = s.Select(h);
    if (before == n / 2) continue;
    stay++;
    moved += Original(kind, n, fewer.Select(h)) != before;
  }
  state.counters["churn_pct"] = 100.0 * moved / stay;
}

void BM_Select(benchmark::State &state) {
  const int kind = static_cast<int>(state.range(0));
  const auto n = static_cast<size_t>(state.range(1));
  const auto hashes = Hashes();
  const AnySelector any = Make(kind, n, false);
  uint64_t sink = 0;
  size_t pos = 0;
  auto loop = [&](const auto &sel) {
    for (auto _ : state) {
      sink += sel(hashes[pos]);
      pos = (pos + 1) & (kStream - 1);
    }
  };
  size_t bytes = 0;
  if (kind == 0) {
    RangeSelector s{static_cast<uint32_t>(n)};
    bytes = s.memory_bytes();
    loop(s);
  } else if (kind == 1) {
    auto s = WeightedSelector::Build(Weights(n)).value();
    bytes = s.memory_bytes();
    loop(s);
  } else if (kind == 2 || kind == 3) {
    auto s = kind == 2 ? MaglevSelector::Build(Keys(n)).value()
                       : MaglevSelector::Build(Keys(n), NextPrime(static_cast<uint32_t>(101 * n))).value();
    bytes = s.memory_bytes();
    loop(s);
  } else if (kind == 4) {
    auto s = RendezvousSelector::Build(Keys(n)).value();
    bytes = s.memory_bytes();
    loop(s);
  } else {
    uint32_t out[32];
    bytes = MaglevSelector::Build(Keys(n)).value().memory_bytes();
    for (auto _ : state) {
      any.SelectBatch(std::span<const uint32_t>(&hashes[pos], 32), out);
      sink += out[0] + out[31];
      pos = (pos + 32) & (kStream - 1);
    }
  }
  benchmark::DoNotOptimize(sink);
  const double per = kind == 5 ? 32.0 : 1.0;
  state.counters["ns_per_select"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * per,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["bytes_per_group"] = static_cast<double>(bytes);
  Quality(state, kind == 5 ? 2 : kind, n, any);
}

uint16_t FloatRange(uint32_t hashval, uint16_t range) {
  union {
    uint64_t i;
    double d;
  } tmp;
  tmp.i = 0x3ff0000000000000ull | (static_cast<uint64_t>(hashval) << 20);
  return static_cast<uint16_t>((tmp.d - 1.0) * range);
}

void BM_HashLbRange(benchmark::State &state) {
  const auto hashes = Hashes();
  uint64_t sink = 0;
  size_t pos = 0;
  if (state.range(0) == 0) {
    for (auto _ : state) {
      sink += FloatRange(hashes[pos], 7);
      pos = (pos + 1) & (kStream - 1);
    }
  } else {
    for (auto _ : state) {
      sink += RangeSelect(hashes[pos], 7);
      pos = (pos + 1) & (kStream - 1);
    }
  }
  benchmark::DoNotOptimize(sink);
}

BENCHMARK(BM_Select)->ArgsProduct({{0, 1, 2, 3, 4, 5}, {2, 4, 8, 32, 128}})->MinTime(0.2);
BENCHMARK(BM_HashLbRange)->Arg(0)->Arg(1)->MinTime(0.2);

}  // namespace
