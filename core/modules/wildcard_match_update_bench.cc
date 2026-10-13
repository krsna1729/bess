// SPDX-License-Identifier: BSD-3-Clause

// G1.2 mode C for WildcardMatch, measured through the module and the tables it
// uses (Decision D-014).
//
//   BM_ModuleAddDelete/rules/tuples  a module (two 4-byte fields) holding
//        `rules` rules spread over `tuples` masks: one `add` of a new rule and
//        one `delete` of it per iteration, through the command API. Items =
//        ops. Builds against the pre-D-014 module too (public commands only).
//   BM_Lookup/impl/tuples/rules/width  the packet-path call: 32 keys of
//        `width` bytes per batch, each matching exactly one tuple (the rest
//        miss); mask t keeps every key byte except byte t. impl 0 = the K3.5
//        generation backend (RuntimeMaskedBackend) WildcardMatch rebuilt per
//        rule, 1 = ConcurrentMaskedTable. Width 13 exercises the masking
//        kernel's 8+4+1-byte steps (D-124).

#include <benchmark/benchmark.h>

#include <array>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "modules/wildcard_match.h"
#include "pb/module_msg.pb.h"

#if __has_include("classifier/concurrent_masked.h")
#include "classifier/concurrent_masked.h"
#include "runtime/runtime_state.h"
#define HAVE_CONCURRENT_MASKED 1
#endif
#include "classifier/masked_exact.h"

namespace {

// Mask t keeps both fields except byte t of the 8-byte key.
std::array<uint8_t, 8> MaskFor(int t) {
  std::array<uint8_t, 8> m;
  m.fill(0xff);
  m[t] = 0;
  return m;
}

bess::pb::WildcardMatchCommandAddArg Rule(uint64_t value, int tuple,
                                          int64_t priority, uint64_t gate) {
  std::array<uint8_t, 8> v;
  std::memcpy(v.data(), &value, 8);
  const auto m = MaskFor(tuple);
  for (int b = 0; b < 8; b++) v[b] &= m[b];
  bess::pb::WildcardMatchCommandAddArg arg;
  arg.set_gate(gate);
  arg.set_priority(priority);
  for (int f = 0; f < 2; f++) {
    arg.add_values()->set_value_bin(
        std::string(reinterpret_cast<const char *>(v.data() + 4 * f), 4));
    arg.add_masks()->set_value_bin(
        std::string(reinterpret_cast<const char *>(m.data() + 4 * f), 4));
  }
  return arg;
}

WildcardMatch *CreateModule(size_t rules, int tuples) {
  ModuleGraph::DestroyAllModules();
  bess::pb::WildcardMatchArg arg;
  for (int f = 0; f < 2; f++) {
    auto *field = arg.add_fields();
    field->set_offset(26 + 4 * f);
    field->set_num_bytes(4);
  }
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) std::abort();
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(
      ModuleBuilder::all_module_builders().at("WildcardMatch"), "wm", packed,
      &perr);
  if (m == nullptr || perr.code() != 0) std::abort();
  auto *wm = static_cast<WildcardMatch *>(m);
  bess::pb::WildcardMatchConfig config;
  std::mt19937_64 rng(rules * 31 + tuples);
  for (size_t r = 0; r < rules; r++) {
    *config.add_rules() =
        Rule(rng() | 1, static_cast<int>(r % tuples),
             static_cast<int64_t>(r % 100), r % 64);
  }
  if (wm->SetRuntimeConfig(config).has_error()) std::abort();
  return wm;
}

void BM_ModuleAddDelete(benchmark::State &st) {
  WildcardMatch *m = CreateModule(static_cast<size_t>(st.range(0)),
                                  static_cast<int>(st.range(1)));
  uint64_t next = 0;
  for (auto _ : st) {
    const auto add = Rule((next++ << 1), 0, 5, 3);  // even: never prefilled
    bess::pb::WildcardMatchCommandDeleteArg del;
    *del.mutable_values() = add.values();
    *del.mutable_masks() = add.masks();
    if (m->CommandAdd(add).has_error() || m->CommandDelete(del).has_error()) {
      st.SkipWithError("command failed");
      break;
    }
  }
  st.SetItemsProcessed(st.iterations() * 2);
  ModuleGraph::DestroyAllModules();
}
BENCHMARK(BM_ModuleAddDelete)
    ->ArgsProduct({{1000, 10000, 100000}, {1, 4, 8}})
    ->Unit(benchmark::kMicrosecond);

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
  for (auto _ : st) {
    const ConstBytes keys(f.stream.data() + offset * width, kBatch * width);
    const uint64_t hits =
        concurrent
            ? f.concurrent->LookupBatch(keys, width, results.data(), kBatch)
            : f.generation.lookup_batch(keys, width, results);
    found += static_cast<uint64_t>(__builtin_popcountll(hits));
    benchmark::DoNotOptimize(results);
    offset = (offset + kBatch) % kStream;
  }
  if (found != st.iterations() * kBatch) st.SkipWithError("missed a key");
  st.SetItemsProcessed(st.iterations() * kBatch);
  st.SetLabel(std::string(concurrent ? "concurrent" : "generation") + " t" +
              std::to_string(tuples) + " w" + std::to_string(width));
}
BENCHMARK(BM_Lookup)->ArgsProduct(
    {{0, 1}, {1, 4, 8}, {1000, 16000, 128000, 1000000}, {8, 13}});

#endif  // HAVE_CONCURRENT_MASKED

}  // namespace
