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

// Exact-match (cuckoo) lookup beyond cache: does batch pipelining pay, and how
// do table, key and value sizes move it?
//
// K4.5 measured batch bodies on tables of at most 65,536 small entries --
// L2-resident -- and prefetched only packet heads, so it could not see the
// regime where lookups are dependent cache misses. This drives the production
// ExactMatch backend state (RuntimeCuckooState and the fixed-width probe
// hash/equality kernels RuntimeCuckooLookupBatchFixed uses) with a 4M-lookup
// stream of existing keys in random order.
//
// Bodies, 32 keys per batch:
//   0 current   hash + probe one key at a time (production before K4.6)
//   1 pf-bucket hash all 32 and prefetch each primary bucket, then probe
//   2 pf-2stage as 1, then prefetch each candidate entry (bucket now cached),
//               then probe
//   3 production RuntimeCuckooLookupBatchFixed as shipped (body 1 via
//               dataplane::RunStages since K4.6)
//
// Shapes: (key bytes, value bytes) in {(8,2), (32,2), (64,2), (8,16),
// (8,64)}; entries 16K / 1M / 4M. Heap tables; THP is enabled on this host.

#include <benchmark/benchmark.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "classifier/cuckoo_exact.h"

namespace {

namespace detail = bess::classifier::detail;

constexpr size_t kBatch = 32;
constexpr size_t kStream = size_t{1} << 22;

template <size_t ValueBytes>
struct Value {
  std::array<uint8_t, ValueBytes> bytes;
};

template <size_t KeyBytes, size_t ValueBytes>
struct Fixture {
  using State = detail::RuntimeCuckooState<KeyBytes, Value<ValueBytes>>;
  State state;
  std::vector<std::array<std::byte, KeyBytes>> stream;
};

// One live fixture at a time: 4M-entry tables with 64-byte keys are large.
template <size_t KeyBytes, size_t ValueBytes>
Fixture<KeyBytes, ValueBytes> &FixtureFor(size_t n) {
  static std::unique_ptr<Fixture<KeyBytes, ValueBytes>> f;
  static size_t built = 0;
  if (f != nullptr && built == n) {
    return *f;
  }
  f.reset();
  f = std::make_unique<Fixture<KeyBytes, ValueBytes>>();
  built = n;
  f->state.logical_key_size = KeyBytes;
  // Stored-key functors are stateful (logical size); the defaults hash zero
  // bytes, so pass the table's, as the production builder does.
  const detail::RuntimeCuckooHash<KeyBytes> hash{KeyBytes};
  const detail::RuntimeCuckooEqual<KeyBytes> equal{KeyBytes};
  std::mt19937_64 rng(0x6b3c + n + KeyBytes * 7 + ValueBytes);
  std::vector<std::array<std::byte, KeyBytes>> keys;
  keys.reserve(n);
  detail::RuntimeCuckooKey<KeyBytes> key{};
  while (keys.size() < n) {
    for (size_t b = 0; b < KeyBytes; b += 8) {
      const uint64_t r = rng();
      std::memcpy(key.bytes.data() + b, &r, std::min<size_t>(8, KeyBytes - b));
    }
    if (f->state.map.Find(key, hash, equal) != nullptr) {
      continue;
    }
    Value<ValueBytes> v{};
    v.bytes[0] = static_cast<uint8_t>(keys.size());
    if (f->state.map.Insert(key, v, hash, equal) == nullptr) {
      std::abort();
    }
    keys.push_back(key.bytes);
  }
  f->stream.resize(kStream);
  for (auto &k : f->stream) {
    k = keys[rng() % keys.size()];
  }
  return *f;
}

template <size_t KeyBytes, size_t ValueBytes>
void BM_CuckooBatch(benchmark::State &st) {
  const size_t n = static_cast<size_t>(st.range(0));
  const int body = static_cast<int>(st.range(1));
  auto &f = FixtureFor<KeyBytes, ValueBytes>(n);
  const auto &map = f.state.map;
  const detail::RuntimeCuckooFixedProbeHash<KeyBytes> hash;
  const detail::RuntimeCuckooFixedProbeEqual<KeyBytes, KeyBytes> equal;

  size_t offset = 0;
  std::array<Value<ValueBytes>, kBatch> results{};
  std::array<bess::utils::HashResult, kBatch> hashes{};
  uint64_t hits_total = 0;
  for (auto _ : st) {
    const auto *keys = &f.stream[offset];
    uint64_t hits = 0;
    auto probe_of = [&](size_t i) {
      return detail::RuntimeCuckooProbe{.data = keys[i].data(),
                                        .size = KeyBytes};
    };
    if (body == 3) {
      std::array<Value<ValueBytes>, kBatch> out;
      hits = detail::RuntimeCuckooLookupBatchFixed<KeyBytes, KeyBytes,
                                                   Value<ValueBytes>, true>(
          &f.state,
          bess::classifier::ConstBytes(keys[0].data(), kBatch * KeyBytes),
          KeyBytes, std::span(out));
      results = out;
    } else if (body == 0) {
      for (size_t i = 0; i < kBatch; i++) {
        const auto probe = probe_of(i);
        if (const auto *e = map.FindPrehashedAs(
                static_cast<bess::utils::HashResult>(hash(probe)), probe,
                equal)) {
          results[i] = e->second;
          hits |= uint64_t{1} << i;
        }
      }
    } else {
      for (size_t i = 0; i < kBatch; i++) {
        hashes[i] = static_cast<bess::utils::HashResult>(hash(probe_of(i)));
        map.PrefetchBucketPrehashed(hashes[i]);
      }
      if (body == 2) {
        for (size_t i = 0; i < kBatch; i++) {
          map.PrefetchEntryPrehashed(hashes[i]);
        }
      }
      for (size_t i = 0; i < kBatch; i++) {
        if (const auto *e =
                map.FindPrehashedAs(hashes[i], probe_of(i), equal)) {
          results[i] = e->second;
          hits |= uint64_t{1} << i;
        }
      }
    }
    hits_total += static_cast<uint64_t>(__builtin_popcountll(hits));
    benchmark::DoNotOptimize(results);
    offset = (offset + kBatch) % f.stream.size();
  }
  if (hits_total != st.iterations() * kBatch) {
    st.SkipWithError("a lookup of an existing key missed");
  }
  st.SetItemsProcessed(st.iterations() * kBatch);
  static const char *const kNames[] = {"current", "pf-bucket", "pf-2stage",
                                       "production"};
  st.SetLabel(std::string(kNames[body]) + " k" + std::to_string(KeyBytes) +
              " v" + std::to_string(ValueBytes));
}

// Size outermost, body innermost, so each fixture is built once per size.
#define CUCKOO_ROWS(K, V)                                      \
  BENCHMARK(BM_CuckooBatch<K, V>)                              \
      ->Name("BM_CuckooBatch/k" #K "v" #V)                     \
      ->ArgsProduct({{16384, 1048576, 4194304}, {0, 1, 2, 3}})

CUCKOO_ROWS(8, 2);
// Crossover: where does the staged body start to pay?
BENCHMARK(BM_CuckooBatch<8, 2>)
    ->Name("BM_CuckooBatch/k8v2small")
    ->ArgsProduct({{256, 1024, 4096}, {0, 3}});
CUCKOO_ROWS(32, 2);
CUCKOO_ROWS(64, 2);
CUCKOO_ROWS(8, 16);
CUCKOO_ROWS(8, 64);

}  // namespace
