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


// G1.2 mode C for WildcardMatch, measured through the module and the tables it
// uses (Decision D-014).
//
//   BM_ModuleAddDelete/rules/tuples  a module (two 4-byte fields) holding
//        `rules` rules spread over `tuples` masks: one `add` of a new rule and
//        one `delete` of it per iteration, through the command API. Items =
//        ops. Builds against the pre-D-014 module too (public commands only).
//   BM_Lookup/impl/tuples/rules  the packet-path call: 32 keys per batch, each
//        matching exactly one tuple (the rest miss). impl 0 = the K3.5
//        generation backend (RuntimeMaskedBackend) WildcardMatch rebuilt per
//        rule, 1 = ConcurrentMaskedTable.

#include <benchmark/benchmark.h>

#include <array>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "modules/wildcard_match.h"
#include "pb/module_msg.pb.h"

#if __has_include("classifier/concurrent_masked.h")
#include "classifier/concurrent_masked.h"
#include "control/runtime_state.h"
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
  std::vector<std::array<std::byte, 8>> stream;
};

LookupFixture &FixtureFor(int tuples, size_t rules) {
  static std::unique_ptr<LookupFixture> f;
  static std::pair<int, size_t> built{};
  if (f && built == std::make_pair(tuples, rules)) return *f;
  f.reset();
  f = std::make_unique<LookupFixture>();
  built = {tuples, rules};
  std::mt19937_64 rng(0x3a5 + rules + tuples);
  std::vector<std::array<std::byte, 8>> values(rules), masks(tuples);
  for (int t = 0; t < tuples; t++) {
    const auto m = MaskFor(t);
    std::memcpy(masks[t].data(), m.data(), 8);
  }
  std::vector<bess::classifier::RuntimeMaskedRule<uint16_t, int64_t>> rs;
  auto table = bess::classifier::ConcurrentMaskedTable::Create(
      8, 8, bess::control::runtime().rcu());
  if (!table) std::abort();
  f->concurrent = std::move(*table);
  for (size_t r = 0; r < rules; r++) {
    const uint64_t v = rng();
    std::memcpy(values[r].data(), &v, 8);
    const int t = static_cast<int>(r % tuples);
    for (int b = 0; b < 8; b++) values[r][b] &= masks[t][b];
    rs.push_back({.value = ConstBytes(values[r].data(), 8),
                  .mask = ConstBytes(masks[t].data(), 8),
                  .priority = static_cast<int64_t>(r),
                  .result = static_cast<uint16_t>(r)});
    f->concurrent->Upsert(ConstBytes(masks[t].data(), 8),
                          ConstBytes(values[r].data(), 8),
                          static_cast<int64_t>(r), static_cast<uint16_t>(r));
  }
  auto gen = bess::classifier::RuntimeMaskedBackend<uint16_t, int64_t>::Build(
      8, rs);
  if (!gen) std::abort();
  f->generation = std::move(*gen);
  f->stream.resize(kStream);
  for (auto &k : f->stream) {
    const size_t r = rng() % rules;
    k = values[r];
    k[r % tuples] = static_cast<std::byte>(rng());  // the wildcard byte
  }
  return *f;
}

void BM_Lookup(benchmark::State &st) {
  const bool concurrent = st.range(0) == 1;
  const int tuples = static_cast<int>(st.range(1));
  LookupFixture &f = FixtureFor(tuples, static_cast<size_t>(st.range(2)));
  size_t offset = 0;
  uint64_t found = 0;
  std::array<uint16_t, kBatch> results;
  for (auto _ : st) {
    const ConstBytes keys(f.stream[offset].data(), kBatch * 8);
    const uint64_t hits =
        concurrent ? f.concurrent->LookupBatch(keys, 8, results.data(), kBatch)
                   : f.generation.lookup_batch(keys, 8, results);
    found += static_cast<uint64_t>(__builtin_popcountll(hits));
    benchmark::DoNotOptimize(results);
    offset = (offset + kBatch) % kStream;
  }
  if (found != st.iterations() * kBatch) st.SkipWithError("missed a key");
  st.SetItemsProcessed(st.iterations() * kBatch);
  st.SetLabel(std::string(concurrent ? "concurrent" : "generation") + " t" +
              std::to_string(tuples));
}
BENCHMARK(BM_Lookup)->ArgsProduct(
    {{0, 1}, {1, 4, 8}, {1000, 16000, 128000, 1000000}});

#endif  // HAVE_CONCURRENT_MASKED

}  // namespace
