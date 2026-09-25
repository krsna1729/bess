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

// K4.6 cross-structure sweep: plain vs stage-major batch lookup on the tables
// behind L2Forward, NAT and WildcardMatch, from cache-resident to far beyond
// L3. Each row looks up 32 keys per batch from a 4M-key stream of existing
// keys in random order (a stream much larger than the caches; see K7/K4.5c).
//
//   L2Forward  l2_table, 4-way buckets of 8-byte slots (inline entries): a
//              hit is hash -> primary bucket line (-> alternate bucket).
//              Staged: hash all + prefetch primary buckets, then l2_find.
//   NAT        CuckooMap<Endpoint, NatEntry>: hash -> bucket -> entry.
//              Staged: hash + PrefetchBucketPrehashed, then FindPrehashedAs.
//   Wildcard   RuntimeMaskedBackend, 16-byte keys, 1/4/8 tuples (masks); each
//              lookup probes every tuple (one hit, the rest misses). Plain vs
//              staged is the per-tuple cuckoo body, forced at build.
//
// Second argument: total entries.

#include <benchmark/benchmark.h>

#include <array>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "classifier/masked_exact.h"
#include "modules/l2_table.h"
#include "modules/nat.h"

namespace {

constexpr size_t kBatch = 32;
constexpr size_t kStream = size_t{1} << 22;

// -- L2Forward ----------------------------------------------------------------------

struct L2Fixture {
  l2_table table = {};
  std::vector<uint64_t> stream;
  ~L2Fixture() { l2_deinit(&table); }
};

L2Fixture &L2For(size_t entries) {
  static std::unique_ptr<L2Fixture> f;
  static size_t built = 0;
  if (f && built == entries) return *f;
  f.reset();
  f = std::make_unique<L2Fixture>();
  built = entries;
  // ~50% load: size * 4 slots = 2 * entries.
  size_t size = 1;
  while (size * 4 < entries * 2) size <<= 1;
  l2_init(&f->table, static_cast<int>(size), 4);
  std::mt19937_64 rng(0x1234 + entries);
  std::vector<uint64_t> macs;
  while (macs.size() < entries) {
    const uint64_t mac = rng() & 0xffffffffffffull;
    if (l2_add_entry(&f->table, mac, static_cast<gate_idx_t>(macs.size() & 0x3ff)) == 0) {
      macs.push_back(mac);
    }
  }
  f->stream.resize(kStream);
  for (auto &k : f->stream) k = macs[rng() % macs.size()];
  return *f;
}

using bess::dataplane::LookupBody;

LookupBody BodyArg(int64_t arg, const bess::dataplane::LookupShape &shape) {
  return bess::dataplane::ResolveLookupBody(
      arg == 0 ? LookupBody::kPlain
               : arg == 1 ? LookupBody::kStaged : LookupBody::kAuto,
      shape);
}

const char *BodyLabel(int64_t arg, LookupBody chosen) {
  if (arg != 2) return chosen == LookupBody::kStaged ? "staged" : "plain";
  return chosen == LookupBody::kStaged ? "auto=staged" : "auto=plain";
}

void BM_L2Forward(benchmark::State &st) {
  L2Fixture &f = L2For(static_cast<size_t>(st.range(1)));
  l2_table *t = &f.table;
  const LookupBody body = BodyArg(
      st.range(0), {.table_bytes = l2_table_bytes(t), .dependent_lines = 1,
                    .branches_on_loaded_data = true});
  size_t offset = 0;
  uint64_t found = 0;
  gate_idx_t gates[kBatch];
  for (auto _ : st) {
    const uint64_t *keys = &f.stream[offset];
    bess::dataplane::RunBatch(
        body, kBatch, [&](size_t i) { l2_prefetch(t, keys[i]); },
        [&](size_t i) { found += l2_find(t, keys[i], &gates[i]) == 0; });
    benchmark::DoNotOptimize(gates);
    offset = (offset + kBatch) % f.stream.size();
  }
  if (found != st.iterations() * kBatch) st.SkipWithError("L2 lookup missed");
  st.SetItemsProcessed(st.iterations() * kBatch);
  st.counters["table_bytes"] = static_cast<double>(
      sizeof(l2_entry) * t->size * t->bucket);
  st.SetLabel(BodyLabel(st.range(0), body));
}
BENCHMARK(BM_L2Forward)
    ->ArgsProduct({{0, 1, 2}, {4096, 65536, 1048576, 4194304}});

// -- NAT ------------------------------------------------------------------------------

using NatMap = bess::utils::CuckooMap<Endpoint, NatEntry, Endpoint::Hash,
                                      Endpoint::EqualTo>;

struct NatFixture {
  NatMap map;
  std::vector<Endpoint> stream;
};

NatFixture &NatFor(size_t entries) {
  static std::unique_ptr<NatFixture> f;
  static size_t built = 0;
  if (f && built == entries) return *f;
  f.reset();
  f = std::make_unique<NatFixture>();
  built = entries;
  std::mt19937_64 rng(0x4321 + entries);
  std::vector<Endpoint> keys;
  while (keys.size() < entries) {
    const uint64_t r = rng();
    Endpoint e;
    e.addr = be32_t(static_cast<uint32_t>(r));
    e.port = be16_t(static_cast<uint16_t>(r >> 32));
    e.protocol = 6;
    if (f->map.Find(e) != nullptr) continue;
    NatEntry v{};
    v.endpoint = e;
    if (f->map.Insert(e, v) == nullptr) std::abort();
    keys.push_back(e);
  }
  f->stream.resize(kStream);
  for (auto &k : f->stream) k = keys[rng() % keys.size()];
  return *f;
}

void BM_Nat(benchmark::State &st) {
  NatFixture &f = NatFor(static_cast<size_t>(st.range(1)));
  const LookupBody body = BodyArg(
      st.range(0), {.table_bytes = f.map.MemoryBytes(), .dependent_lines = 2,
                    .branches_on_loaded_data = true});
  const Endpoint::Hash hash;
  const Endpoint::EqualTo eq;
  size_t offset = 0;
  uint64_t found = 0;
  const NatMap::Entry *hits[kBatch];
  bess::utils::HashResult h[kBatch];
  for (auto _ : st) {
    const Endpoint *keys = &f.stream[offset];
    bess::dataplane::RunBatch(
        body, kBatch,
        [&](size_t i) {
          h[i] = static_cast<bess::utils::HashResult>(hash(keys[i]));
          f.map.PrefetchBucketPrehashed(h[i]);
        },
        [&](size_t i) {
          hits[i] = std::as_const(f.map).FindPrehashedAs(h[i], keys[i], eq);
          found += hits[i] != nullptr;
        });
    benchmark::DoNotOptimize(hits);
    offset = (offset + kBatch) % f.stream.size();
  }
  if (found != st.iterations() * kBatch) st.SkipWithError("NAT lookup missed");
  st.SetItemsProcessed(st.iterations() * kBatch);
  st.counters["table_bytes"] = static_cast<double>(f.map.MemoryBytes());
  st.SetLabel(BodyLabel(st.range(0), body));
}
BENCHMARK(BM_Nat)->ArgsProduct({{0, 1, 2}, {4096, 65536, 1048576, 4194304}});

// -- WildcardMatch ------------------------------------------------------------------

constexpr size_t kWmKey = 16;
using Masked = bess::classifier::RuntimeMaskedBackend<uint16_t>;

struct WmFixture {
  Masked backend;
  std::vector<std::array<std::byte, kWmKey>> stream;
  size_t table_bytes = 0;
};

// Tuple t's mask is all-ones except byte t; rules are spread evenly.
WmFixture &WmFor(int tuples, size_t entries, int64_t body_arg) {
  static std::unique_ptr<WmFixture> f;
  static std::tuple<int, size_t, int64_t> built{};
  if (f && built == std::make_tuple(tuples, entries, body_arg)) return *f;
  f.reset();
  f = std::make_unique<WmFixture>();
  built = {tuples, entries, body_arg};
  std::mt19937_64 rng(0x5678 + entries + tuples);
  std::vector<std::array<std::byte, kWmKey>> masks(tuples), values;
  std::vector<int> tuple_of;
  for (int t = 0; t < tuples; t++) {
    masks[t].fill(std::byte{0xff});
    masks[t][t] = std::byte{0};
  }
  values.reserve(entries);
  for (size_t r = 0; r < entries; r++) {
    std::array<std::byte, kWmKey> v;
    for (size_t b = 0; b < kWmKey; b += 8) {
      const uint64_t x = rng();
      std::memcpy(v.data() + b, &x, 8);
    }
    const int t = static_cast<int>(r % tuples);
    for (size_t b = 0; b < kWmKey; b++) v[b] &= masks[t][b];
    values.push_back(v);
    tuple_of.push_back(t);
  }
  std::vector<bess::classifier::RuntimeMaskedRule<uint16_t>> rules;
  rules.reserve(entries);
  for (size_t r = 0; r < entries; r++) {
    rules.push_back({.value = bess::classifier::ConstBytes(values[r].data(), kWmKey),
                     .mask = bess::classifier::ConstBytes(masks[tuple_of[r]].data(), kWmKey),
                     .priority = static_cast<int64_t>(r),
                     .result = static_cast<uint16_t>(r)});
  }
  auto built_backend = Masked::Build(
      kWmKey, rules,
      body_arg == 0   ? LookupBody::kPlain
      : body_arg == 1 ? LookupBody::kStaged
                      : LookupBody::kAuto);
  if (!built_backend) std::abort();
  f->backend = std::move(*built_backend);
  f->stream.resize(kStream);
  for (auto &k : f->stream) {
    const size_t r = rng() % entries;
    k = values[r];
    k[tuple_of[r]] = static_cast<std::byte>(rng());  // wildcard byte
  }
  return *f;
}

void BM_Wildcard(benchmark::State &st) {
  const int tuples = static_cast<int>(st.range(1));
  WmFixture &f = WmFor(tuples, static_cast<size_t>(st.range(2)), st.range(0));
  size_t offset = 0;
  uint64_t found = 0;
  std::array<uint16_t, kBatch> results;
  for (auto _ : st) {
    const uint64_t hits = f.backend.lookup_batch(
        bess::classifier::ConstBytes(f.stream[offset].data(), kBatch * kWmKey),
        kWmKey, results);
    found += static_cast<uint64_t>(__builtin_popcountll(hits));
    benchmark::DoNotOptimize(results);
    offset = (offset + kBatch) % f.stream.size();
  }
  if (found != st.iterations() * kBatch) st.SkipWithError("wildcard missed");
  st.SetItemsProcessed(st.iterations() * kBatch);
  static const char *const kNames[] = {"plain", "staged", "auto"};
  st.SetLabel(std::string(kNames[st.range(0)]) + " t" + std::to_string(tuples));
}
BENCHMARK(BM_Wildcard)
    ->ArgsProduct({{0, 1, 2}, {1, 4, 8}, {4096, 65536, 1048576}});

}  // namespace
