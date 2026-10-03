// SPDX-License-Identifier: BSD-3-Clause
// Supplemental M0 baseline benchmark (tools/run_m0_baseline.py): f4fdab03 has
// no SlotTable::Lookup/Publish or StrongId benchmark, so this one source is
// compiled against both trees' core/dataplane/{slot_table,strong_id}.h, which
// the script requires to be byte-identical.
#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"

namespace {

struct SlotTag;
using Id = bess::dataplane::StrongId<SlotTag, uint32_t>;
struct Payload {
  uint64_t value;
};
using Table = bess::dataplane::SlotTable<Id, Payload>;

// Out of line so the assembly snapshot has a StrongId compare kernel.
[[gnu::noinline]] size_t CountEqual(const Id *ids, size_t n, Id want) {
  size_t count = 0;
  for (size_t i = 0; i < n; ++i) {
    count += ids[i] == want;
  }
  return count;
}

constexpr uint32_t kCapacity = 1024;

struct Fixture {
  Fixture() : table(kCapacity) {
    for (uint32_t i = 1; i <= kCapacity; ++i) {
      table.Publish(Id{i}, std::make_unique<const Payload>(Payload{i}));
    }
    for (size_t i = 0; i < ids.size(); ++i) {
      ids[i] = Id{static_cast<uint32_t>(1 + (i * 127) % kCapacity)};
    }
  }
  Table table;
  std::array<Id, 32> ids{};
};

// 32 lookups of live ids per iteration; reports per-lookup items.
void BM_SlotTableLookup(benchmark::State &state) {
  Fixture f;
  uint64_t sink = 0;
  for (auto _ : state) {
    for (Id id : f.ids) {
      const Payload *value = f.table.Lookup(id);
      benchmark::DoNotOptimize(value);
      sink += value->value;
    }
  }
  benchmark::DoNotOptimize(sink);
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(f.ids.size()));
}
BENCHMARK(BM_SlotTableLookup);

// Replaces the object at one live id, taking the old one back (no allocation).
void BM_SlotTablePublish(benchmark::State &state) {
  Fixture f;
  auto next = std::make_unique<const Payload>(Payload{7});
  for (auto _ : state) {
    next = f.table.Publish(Id{1}, std::move(next));
    benchmark::DoNotOptimize(next.get());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SlotTablePublish);

// 32 StrongId equality compares per iteration through the out-of-line kernel.
void BM_StrongIdCompare(benchmark::State &state) {
  Fixture f;
  Id want = f.ids[5];
  for (auto _ : state) {
    benchmark::DoNotOptimize(want);
    benchmark::DoNotOptimize(CountEqual(f.ids.data(), f.ids.size(), want));
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(f.ids.size()));
}
BENCHMARK(BM_StrongIdCompare);

}  // namespace

BENCHMARK_MAIN();
