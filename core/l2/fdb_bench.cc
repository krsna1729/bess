// SPDX-License-Identifier: BSD-3-Clause

// M14 (D-064): L2 lookup and learning across the three candidate backends.
//
//   BM_L2Lookup/<backend>/<n>/<stream>/<batch>   ns per lookup
//     backend 0 l2::Fdb (generic WorkerFlowTable), 1 legacy l2_table
//     (MAC-specialised 4-way cuckoo, L2Forward's), 2 std::unordered_map (the
//     pre-M14 Bridge), 3 the generic flow substrate (WorkerFlowTable<FdbKey,
//     InterfaceId>, the backend D-064 measured and rejected); 4/5/6 l2::BasicFdb
//     over PackedMacTable with OwnerWrites / SingleWriter / MultiWriter (table
//     policy TP2: one-word slots, lock-free shared readers); 7 the raw
//     PackedMacTable<SingleWriter> as L2Forward calls it (the direct
//     comparison with 1); stream 0 one hot
//     MAC, 1 uniform hit, 2 miss
//   BM_L2Learn/<backend>/<n>   ns per learn of a uniformly chosen MAC on a
//     table holding n, half of them already present (refresh or move) and half
//     new (with the table at its limit the new ones are refused or, for the
//     map, inserted then erased to hold the size)
//   BM_FdbAge/<n>               ns per expired entry when n age out at once

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "flow/worker_flow_table.h"
#include "l2/fdb.h"
#include "modules/legacy_l2_table_bench.h"
#include "l2/packed_mac_table.h"

namespace {

using bess::dataplane::InterfaceId;
using bess::l2::BridgeDomainId;
using bess::l2::Fdb;
using bess::l2::FdbKey;
using bess::l2::MacAddress;

constexpr size_t kStream = size_t{1} << 18;
const BridgeDomainId kD(0);

MacAddress Mac(uint32_t i) {
  const uint32_t h = i * 2654435761u;
  return MacAddress{{0x02, static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16),
                     static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h),
                     static_cast<uint8_t>(i)}};
}
uint64_t U64(const MacAddress &m) {
  uint64_t v = 0;
  std::memcpy(&v, m.bytes.data(), 6);
  return v;
}

std::vector<uint32_t> Stream(int64_t s, uint32_t n) {
  std::mt19937 rng(0x12);
  std::vector<uint32_t> ids(kStream);
  for (auto &id : ids) {
    id = s == 0 ? 7 : s == 1 ? rng() % n : n + rng() % n;
  }
  return ids;
}

template <typename Sync>
using PackedFdb =
    bess::l2::BasicFdb<bess::l2::PackedMacTable<bess::dataplane::ExpiryHandle, Sync>>;

template <typename F = Fdb>
std::unique_ptr<F> FdbWith(uint32_t n) {
  typename F::Config c;
  c.capacity = n;
  c.aging = ~uint64_t{0} >> 4;
  c.max_domains = 1;
  auto f = F::Create(c).value();
  for (uint32_t i = 0; i < n; i++) {
    (void)f->AddStatic(kD, Mac(i), InterfaceId(static_cast<uint16_t>(1 + i % 8)));
  }
  return f;
}

// l2_table sized like L2Forward's: buckets of 4, size = next power of two that
// holds n at <= 50% load.
struct Legacy {
  l2_table t{};
  explicit Legacy(uint32_t n) {
    uint32_t size = 2;
    while (size * 4 < 2 * n) size <<= 1;
    l2_init(&t, static_cast<int>(size), 4);
    for (uint32_t i = 0; i < n; i++) {
      (void)l2_add_entry(&t, U64(Mac(i)), static_cast<gate_idx_t>(i % 8));
    }
  }
  ~Legacy() { l2_deinit(&t); }
};

void BM_L2Lookup(benchmark::State &state) {
  const int backend = static_cast<int>(state.range(0));
  const auto n = static_cast<uint32_t>(state.range(1));
  const auto ids = Stream(state.range(2), n);
  const auto batch = static_cast<size_t>(state.range(3));
  std::vector<MacAddress> macs(kStream);
  std::vector<FdbKey> keys(kStream);
  std::vector<uint64_t> words(kStream);
  for (size_t i = 0; i < kStream; i++) {
    macs[i] = Mac(ids[i]);
    keys[i] = bess::l2::MakeKey(kD, macs[i]);
    words[i] = U64(macs[i]);
  }
  uint64_t sink = 0, hits = 0;
  size_t pos = 0;
  // The loop's counters are the lambda's locals (written back after), as in
  // the other backends' inline loops: captured by reference they would be
  // loaded and stored through memory every iteration.
  const auto run_fdb = [&](auto f) {
    InterfaceId out[Fdb::kMaxBatch];
    uint64_t s = 0, h = 0;
    size_t p = 0;
    const MacAddress *m = macs.data();
    const FdbKey *k = keys.data();
    for (auto _ : state) {
      if (batch == 1) {
        const InterfaceId got = f->Lookup(kD, m[p]);
        s += got.value();
        h += got != bess::dataplane::kInvalidInterfaceId;
      } else {
        s += f->LookupBatch(std::span<const FdbKey>(&k[p], batch),
                            std::span<InterfaceId>(out, batch));
      }
      p = (p + batch) & (kStream - 1);
    }
    sink += s;
    hits += h;
  };
  if (backend == 0) {
    run_fdb(FdbWith(n));
  } else if (backend == 4) {
    run_fdb(FdbWith<PackedFdb<bess::dataplane::OwnerWrites>>(n));
  } else if (backend == 5) {
    run_fdb(FdbWith<PackedFdb<bess::dataplane::SingleWriter>>(n));
  } else if (backend == 6) {
    run_fdb(FdbWith<PackedFdb<bess::dataplane::MultiWriter>>(n));
  } else if (backend == 7) {
    using Table = bess::l2::PackedMacTable<uint8_t, bess::dataplane::SingleWriter>;
    auto t = Table::Create(n);
    for (uint32_t i = 0; i < n; i++) {
      (void)t->Insert(U64(Mac(i)) << 16, static_cast<uint16_t>(1 + i % 8), 0);
    }
    std::vector<uint64_t> shifted(kStream);
    for (size_t i = 0; i < kStream; i++) shifted[i] = words[i] << 16;
    uint16_t values[64];
    for (auto _ : state) {
      if (batch == 1) {
        const uint32_t v = t->Lookup(shifted[pos]);
        sink += v;
        hits += v != 0;
      } else {
        sink += t->LookupBatch(std::span<const uint64_t>(&shifted[pos], batch), values);
      }
      pos = (pos + batch) & (kStream - 1);
    }
  } else if (backend == 1) {
    Legacy l(n);
    gate_idx_t gates[64];
    for (auto _ : state) {
      if (batch == 1) {
        gate_idx_t g = 0;
        const bool hit = l2_find(&l.t, words[pos], &g) == 0;
        sink += hit ? g : 0;
        hits += hit;
      } else {
        sink += l2_find_batch(&l.t, &words[pos], gates, batch);
      }
      pos = (pos + batch) & (kStream - 1);
    }
  } else if (backend == 3) {
    auto t = bess::flow::WorkerFlowTable<FdbKey, InterfaceId>::Create(n).value();
    for (uint32_t i = 0; i < n; i++) {
      (void)t->Emplace(bess::l2::MakeKey(kD, Mac(i)), InterfaceId(static_cast<uint16_t>(1 + i % 8)));
    }
    const InterfaceId *out[64];
    for (auto _ : state) {
      if (batch == 1) {
        const InterfaceId *s = t->Find(keys[pos]);
        sink += s == nullptr ? 0 : s->value();
        hits += s != nullptr;
      } else {
        sink += t->FindBatch(std::span<const FdbKey>(&keys[pos], batch),
                             std::span<const InterfaceId *>(out, batch));
      }
      pos = (pos + batch) & (kStream - 1);
    }
  } else {
    if (batch != 1) {
      state.SkipWithMessage("scalar only");
      return;
    }
    std::unordered_map<uint64_t, uint16_t> m;
    m.reserve(n);
    for (uint32_t i = 0; i < n; i++) m[U64(Mac(i))] = static_cast<uint16_t>(i % 8);
    for (auto _ : state) {
      auto it = m.find(words[pos]);
      sink += it == m.end() ? 0 : it->second;
      hits += it != m.end();
      pos = (pos + 1) & (kStream - 1);
    }
  }
  benchmark::DoNotOptimize(sink);
  if (batch == 1) {
    state.counters["hit_pct"] = 100.0 * static_cast<double>(hits) /
                                static_cast<double>(state.iterations());
  }
  state.counters["ns_per_lookup"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * batch,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

void BM_L2Learn(benchmark::State &state) {
  const int backend = static_cast<int>(state.range(0));
  const auto n = static_cast<uint32_t>(state.range(1));
  std::mt19937 rng(0x34);
  std::vector<uint32_t> ids(kStream);
  for (auto &id : ids) id = rng() % (2 * n);  // half present, half new
  std::vector<MacAddress> macs(kStream);
  for (size_t i = 0; i < kStream; i++) macs[i] = Mac(ids[i]);
  size_t pos = 0;
  uint64_t now = 1;
  const auto run_learn = [&]<typename F>() {
    typename F::Config c;
    c.capacity = n;
    c.aging = uint64_t{1} << 40;
    c.max_domains = 1;
    auto f = F::Create(c).value();
    for (uint32_t i = 0; i < n; i++) (void)f->Learn(kD, Mac(i), InterfaceId(1), now);
    size_t p = 0;
    uint64_t t = now;
    const MacAddress *m = macs.data();
    for (auto _ : state) {
      benchmark::DoNotOptimize(
          f->Learn(kD, m[p], InterfaceId(static_cast<uint16_t>(1 + (p & 3))), ++t));
      p = (p + 1) & (kStream - 1);
    }
  };
  if (backend == 0) {
    run_learn.template operator()<Fdb>();
  } else if (backend == 4) {
    run_learn.template operator()<PackedFdb<bess::dataplane::OwnerWrites>>();
  } else if (backend == 5) {
    run_learn.template operator()<PackedFdb<bess::dataplane::SingleWriter>>();
  } else if (backend == 6) {
    run_learn.template operator()<PackedFdb<bess::dataplane::MultiWriter>>();
  } else if (backend == 2) {
    struct Entry { uint16_t gate; uint64_t last; bool is_static; };
    std::unordered_map<uint64_t, Entry> m;
    m.reserve(n);
    for (uint32_t i = 0; i < n; i++) m[U64(Mac(i))] = {1, now, false};
    for (auto _ : state) {
      const uint64_t k = U64(macs[pos]);
      auto it = m.find(k);
      if (it != m.end()) {
        it->second.gate = static_cast<uint16_t>(1 + (pos & 3));
        it->second.last = ++now;
      } else if (m.size() < n) {
        m[k] = {1, ++now, false};
      }
      pos = (pos + 1) & (kStream - 1);
    }
    benchmark::DoNotOptimize(m.size());
  } else {
    state.SkipWithMessage("l2_table has no learning path (L2Forward programs it)");
  }
}

void BM_FdbAge(benchmark::State &state) {
  const auto n = static_cast<uint32_t>(state.range(0));
  for (auto _ : state) {
    state.PauseTiming();
    Fdb::Config c;
    c.capacity = n;
    c.aging = 1000;
    c.granularity_shift = 0;
    c.max_domains = 1;
    auto f = Fdb::Create(c).value();
    for (uint32_t i = 0; i < n; i++) (void)f->Learn(kD, Mac(i), InterfaceId(1), i % 997);
    state.ResumeTiming();
    benchmark::DoNotOptimize(f->Age(1u << 30, ~size_t{0}));
  }
  state.counters["ns_per_entry"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * n,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

BENCHMARK(BM_L2Lookup)
    ->ArgsProduct({{0, 1, 2, 3, 4, 5, 6, 7}, {1024, 65536, 1048576}, {0, 1, 2}, {1, 32}})
    ->MinTime(0.25);
BENCHMARK(BM_L2Learn)->ArgsProduct({{0, 2, 4, 5, 6}, {1024, 65536, 1048576}})->MinTime(0.25);
BENCHMARK(BM_FdbAge)->Arg(65536)->Arg(1048576)->Iterations(5);

}  // namespace
