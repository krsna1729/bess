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


// G1.2a mode C for ExactMatch, measured through the module and the tables it
// uses.
//
//   BM_ModuleAddDelete/N  a module holding N rules (2 fields, 4+2 bytes):
//                         one `add` of a new rule and one `delete` of it per
//                         iteration, through the command API. Items = ops.
//                         This file builds against the pre-G1.2 module as
//                         well (public commands only), for the before/after.
//   BM_Lookup/impl/N      the per-batch lookup the packet path makes: 32
//                         random 8-byte keys against N rules. impl 0 = the
//                         K3 cuckoo generation backend ExactMatch used, 1 =
//                         ConcurrentExactTable (rte_hash LF + QSBR). arg 3:
//                         0 = all hits, 1 = all misses.

#include <benchmark/benchmark.h>

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "modules/exact_match.h"
#include "pb/module_msg.pb.h"

#if __has_include("classifier/concurrent_exact.h")
#include "classifier/concurrent_exact.h"
#include "classifier/cuckoo_exact.h"
#include "control/runtime_state.h"
#define HAVE_CONCURRENT_EXACT 1
#endif

namespace {

bess::pb::ExactMatchCommandAddArg Rule(uint32_t a, uint16_t b, uint64_t gate) {
  bess::pb::ExactMatchCommandAddArg arg;
  arg.set_gate(gate);
  arg.add_fields()->set_value_int(a);
  arg.add_fields()->set_value_int(b);
  return arg;
}

ExactMatch *CreateModule(size_t rules) {
  ModuleGraph::DestroyAllModules();
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  f = arg.add_fields();
  f->set_offset(34);
  f->set_num_bytes(2);
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) {
    std::abort();
  }
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(
      ModuleBuilder::all_module_builders().at("ExactMatch"), "em", packed,
      &perr);
  if (m == nullptr || perr.code() != 0) {
    std::abort();
  }
  auto *em = static_cast<ExactMatch *>(m);
  // Prefill by restore: one bulk build in either implementation.
  bess::pb::ExactMatchConfig config;
  for (size_t r = 0; r < rules; r++) {
    *config.add_rules() =
        Rule(static_cast<uint32_t>(r), static_cast<uint16_t>(r), r % 64);
  }
  if (em->SetRuntimeConfig(config).has_error()) {
    std::abort();
  }
  return em;
}

void BM_ModuleAddDelete(benchmark::State &st) {
  const size_t rules = static_cast<size_t>(st.range(0));
  ExactMatch *m = CreateModule(rules);
  uint32_t next = 0x80000000u;
  for (auto _ : st) {
    const auto add = Rule(next, 7, 3);
    bess::pb::ExactMatchCommandDeleteArg del;
    *del.mutable_fields() = add.fields();
    if (m->CommandAdd(add).has_error() || m->CommandDelete(del).has_error()) {
      st.SkipWithError("command failed");
      break;
    }
    next++;
  }
  st.SetItemsProcessed(st.iterations() * 2);
  ModuleGraph::DestroyAllModules();
}
BENCHMARK(BM_ModuleAddDelete)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Arg(1000000)
    ->Unit(benchmark::kMicrosecond);

#ifdef HAVE_CONCURRENT_EXACT

using bess::classifier::ConcurrentExactTable;
using bess::classifier::ConstBytes;

constexpr size_t kBatch = 32;
constexpr size_t kStream = size_t{1} << 20;

struct LookupFixture {
  bess::classifier::RuntimeExactBackend<gate_idx_t> cuckoo;
  std::unique_ptr<ConcurrentExactTable> concurrent;
  std::vector<uint64_t> stream;
};

LookupFixture &FixtureFor(size_t n, bool miss) {
  static std::unique_ptr<LookupFixture> f;
  static std::pair<size_t, bool> built{};
  if (f && built == std::make_pair(n, miss)) return *f;
  f.reset();
  f = std::make_unique<LookupFixture>();
  built = {n, miss};
  std::mt19937_64 rng(0x9e37 + n);
  std::vector<uint64_t> keys(n);
  for (auto &k : keys) k = rng() | 1;  // odd = present, even = absent
  std::vector<bess::classifier::RuntimeExactRule<gate_idx_t>> rules;
  rules.reserve(n);
  for (size_t i = 0; i < n; i++) {
    rules.push_back({.key = ConstBytes(
                         reinterpret_cast<const std::byte *>(&keys[i]), 8),
                     .result = static_cast<gate_idx_t>(i & 0xff)});
  }
  auto cuckoo = bess::classifier::BuildRuntimeCuckooBackend<gate_idx_t>(
      8, rules);
  if (!cuckoo) std::abort();
  f->cuckoo = std::move(*cuckoo);
  uint32_t cap = 1024;
  while (cap * 3 / 4 < n) cap *= 2;  // the module's load limit
  auto table = ConcurrentExactTable::Create(8, cap,
                                            bess::control::runtime().rcu());
  if (!table) std::abort();
  f->concurrent = std::move(*table);
  for (size_t i = 0; i < n; i++) {
    f->concurrent->Upsert(rules[i].key, i & 0xff);
  }
  f->stream.resize(kStream);
  for (auto &k : f->stream) {
    k = keys[rng() % n];
    if (miss) k &= ~uint64_t{1};
  }
  return *f;
}

void BM_Lookup(benchmark::State &st) {
  const int impl = static_cast<int>(st.range(0));
  const bool miss = st.range(2) == 1;
  LookupFixture &f = FixtureFor(static_cast<size_t>(st.range(1)), miss);
  size_t offset = 0;
  uint64_t found = 0;
  std::array<gate_idx_t, kBatch> gates;
  std::array<uint64_t, kBatch> values;
  for (auto _ : st) {
    const ConstBytes keys(
        reinterpret_cast<const std::byte *>(&f.stream[offset]), kBatch * 8);
    const uint64_t hits =
        impl == 1 ? f.concurrent->LookupBatch(keys, 8, values.data(), kBatch)
                  : f.cuckoo.lookup_batch(keys, 8, gates);
    found += static_cast<uint64_t>(__builtin_popcountll(hits));
    benchmark::DoNotOptimize(gates);
    benchmark::DoNotOptimize(values);
    offset = (offset + kBatch) % kStream;
  }
  if (found != (miss ? 0 : st.iterations() * kBatch)) {
    st.SkipWithError("wrong hit count");
  }
  st.SetItemsProcessed(st.iterations() * kBatch);
  static const char *const kImpl[] = {"cuckoo-gen", "rte_hash-LF"};
  st.SetLabel(std::string(kImpl[impl]) +
              (miss ? " miss" : " hit"));
}
BENCHMARK(BM_Lookup)->ArgsProduct(
    {{0, 1}, {1000, 16000, 128000, 1000000}, {0, 1}});

#endif  // HAVE_CONCURRENT_EXACT

}  // namespace
