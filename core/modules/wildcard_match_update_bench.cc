// SPDX-License-Identifier: BSD-3-Clause

// G1.2 mode C for WildcardMatch: the packet-path lookup, measured against the
// tables the module uses (Decision D-014). (The module's add/delete commands
// were measured here too; they are gone with the legacy service -- D-140. A
// rule change now goes through the transaction engine.)
//
//   BM_Lookup/impl/tuples/rules/width  the packet-path call: 32 keys of
//        `width` bytes per batch, each matching exactly one tuple (the rest
//        miss); mask t keeps every key byte except byte t. impl 0 = the K3.5
//        generation backend (RuntimeMaskedBackend) WildcardMatch rebuilt per
//        rule, 1 = ConcurrentMaskedTable. Width 13 exercises the masking
//        kernel's 8+4+1-byte steps (D-124).
//   BM_UpsertErase/tuples/rules  the table's in-place writer: one new rule
//        upserted and erased again (both directly, not through a
//        transaction) on a table of `rules` rules over `tuples` masks, 13-byte
//        keys. Time is per add + delete.

#include <benchmark/benchmark.h>

#include <array>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#if __has_include("classifier/concurrent_masked.h")
#include "classifier/concurrent_masked.h"
#include "runtime/runtime_state.h"
#define HAVE_CONCURRENT_MASKED 1
#endif
#include "classifier/masked_exact.h"

namespace {

#ifdef HAVE_CONCURRENT_MASKED

using bess::classifier::ConstBytes;
constexpr size_t kBatch = 32;
constexpr size_t kStream = size_t{1} << 20;

struct LookupFixture {
  bess::classifier::RuntimeMaskedBackend<uint16_t, int64_t> generation;
  std::unique_ptr<bess::classifier::ConcurrentMaskedTable> concurrent;
  std::vector<std::byte> stream;  // kStream keys of `width` bytes
};

LookupFixture &FixtureFor(int tuples, size_t rules, size_t width) {
  static std::unique_ptr<LookupFixture> f;
  static std::tuple<int, size_t, size_t> built{};
  if (f && built == std::make_tuple(tuples, rules, width)) return *f;
  f.reset();
  f = std::make_unique<LookupFixture>();
  built = {tuples, rules, width};
  std::mt19937_64 rng(0x3a5 + rules + tuples);
  // Mask t keeps every byte but byte t.
  std::vector<std::vector<std::byte>> values(rules), masks(tuples);
  for (int t = 0; t < tuples; t++) {
    masks[t].assign(width, std::byte{0xff});
    masks[t][t] = std::byte{0};
  }
  std::vector<bess::classifier::RuntimeMaskedRule<uint16_t, int64_t>> rs;
  auto table = bess::classifier::ConcurrentMaskedTable::Create(
      static_cast<uint32_t>(width), 8, bess::runtime::runtime().rcu());
  if (!table) std::abort();
  f->concurrent = std::move(*table);
  for (size_t r = 0; r < rules; r++) {
    const int t = static_cast<int>(r % tuples);
    values[r].resize(width);
    for (size_t b = 0; b < width; b++) {
      values[r][b] = static_cast<std::byte>(rng()) & masks[t][b];
    }
    rs.push_back({.value = ConstBytes(values[r].data(), width),
                  .mask = ConstBytes(masks[t].data(), width),
                  .priority = static_cast<int64_t>(r),
                  .result = static_cast<uint16_t>(r % 0xffff)});
    // (0xffff is reserved: ConcurrentMaskedTable::kPendingResult, D-024.)
    f->concurrent->Upsert(ConstBytes(masks[t].data(), width),
                          ConstBytes(values[r].data(), width),
                          static_cast<int64_t>(r),
                          static_cast<uint16_t>(r % 0xffff));
  }
  auto gen = bess::classifier::RuntimeMaskedBackend<uint16_t, int64_t>::Build(
      width, rs);
  if (!gen) std::abort();
  f->generation = std::move(*gen);
  f->stream.resize(kStream * width);
  for (size_t k = 0; k < kStream; k++) {
    const size_t r = rng() % rules;
    std::byte *key = f->stream.data() + k * width;
    std::memcpy(key, values[r].data(), width);
    key[r % tuples] = static_cast<std::byte>(rng());  // the wildcard byte
  }
  return *f;
}

void BM_Lookup(benchmark::State &st) {
  const bool concurrent = st.range(0) == 1;
  const int tuples = static_cast<int>(st.range(1));
  const auto width = static_cast<size_t>(st.range(3));
  LookupFixture &f =
      FixtureFor(tuples, static_cast<size_t>(st.range(2)), width);
  size_t offset = 0;
  uint64_t found = 0;
  std::array<uint16_t, kBatch> results;
  // D-141: the masked table reports the winning rule's gate and id, so the
  // concurrent arm needs its own scratch; the generation backend still takes
  // the gates alone.
  std::array<bess::classifier::ConcurrentMaskedTable::Match, kBatch> matches;
  for (auto _ : st) {
    const ConstBytes keys(f.stream.data() + offset * width, kBatch * width);
    const uint64_t hits =
        concurrent
            ? f.concurrent->LookupBatch(keys, width, matches.data(), kBatch)
            : f.generation.lookup_batch(keys, width, results);
    found += static_cast<uint64_t>(__builtin_popcountll(hits));
    if (concurrent) {
      benchmark::DoNotOptimize(matches);
    } else {
      benchmark::DoNotOptimize(results);
    }
    offset = (offset + kBatch) % kStream;
  }
  if (found != st.iterations() * kBatch) st.SkipWithError("missed a key");
  st.SetItemsProcessed(st.iterations() * kBatch);
  st.SetLabel(std::string(concurrent ? "concurrent" : "generation") + " t" +
              std::to_string(tuples) + " w" + std::to_string(width));
}
BENCHMARK(BM_Lookup)->ArgsProduct(
    {{0, 1}, {1, 4, 8}, {1000, 16000, 128000, 1000000}, {8, 13}});

void BM_UpsertErase(benchmark::State &st) {
  const int tuples = static_cast<int>(st.range(0));
  const auto rules = static_cast<size_t>(st.range(1));
  constexpr size_t kWidth = 13;
  LookupFixture &f = FixtureFor(tuples, rules, kWidth);
  // Fresh rules in mask 0 (every byte but byte 0), none of them in the table.
  std::vector<std::byte> mask(kWidth, std::byte{0xff});
  mask[0] = std::byte{0};
  constexpr size_t kFresh = 1024;
  std::vector<std::byte> fresh(kFresh * kWidth);
  std::mt19937_64 rng(0xf7e5);
  for (size_t i = 0; i < kFresh; i++) {
    std::byte *v = fresh.data() + i * kWidth;
    for (size_t b = 1; b < kWidth; b++) v[b] = static_cast<std::byte>(rng());
    v[0] = std::byte{0};
  }
  const ConstBytes m(mask.data(), kWidth);
  size_t i = 0;
  for (auto _ : st) {
    const ConstBytes v(fresh.data() + i * kWidth, kWidth);
    auto r = f.concurrent->Upsert(m, v, 1, 1);
    benchmark::DoNotOptimize(r);
    if (!f.concurrent->Erase(m, v)) st.SkipWithError("erase failed");
    i = (i + 1) % kFresh;
  }
  st.SetItemsProcessed(st.iterations());
}
BENCHMARK(BM_UpsertErase)->ArgsProduct({{1, 8}, {1000, 16000, 128000}});

#endif  // HAVE_CONCURRENT_MASKED

}  // namespace
