// SPDX-License-Identifier: BSD-3-Clause

// M18 (D-068): NAT before and after.
//
//   BM_Lookup/<impl>/<mappings>   ns per mapping lookup, a batch of 32 random
//     forward keys: 0 legacy (CuckooMap with a forward and a reverse entry per
//     mapping, prefetched batch, as the module did), 1 the binding table
//     (WorkerFlowTable, one binding with the external endpoint as an alias,
//     FindBatch)
//   BM_Allocate/<impl>/<fill_pct>  ns per new mapping's port when <fill_pct> of
//     a 64,512-port span is taken: 0 legacy (random start, linear probe of the
//     table, at most 128 trials), 1 PortPool (random start, bitmap scan);
//     counter fail_pct: allocations that found no port
//   BM_Translate/<path>/<dir>/<mappings>  ns per packet on established
//     mappings, packets in random mapping order: path 0 the legacy module's
//     per packet (unchecked parse, CuckooMap, rewrite), 1 this library's per
//     packet (ParseFrame, Nat::Translate), 2 legacy in batches of 32 with the
//     module's PrefetchBatch, 3 the NAT module's path: batches of 32,
//     Expire(256) per batch, ParseFrame, TranslateBatch (FindBatch); the
//     library rows use the module's table size (1M bindings with these 20
//     addresses), the legacy table holds exactly the mappings; the packet
//     stream draws mappings at random with replacement; dir 0 forward, 1
//     reverse; counter ns_per_packet
//   BM_Bind                        ns per new mapping (allocate, emplace,
//     schedule, rewrite) until the table is full; then the table is rebuilt

#include <benchmark/benchmark.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "arch/crc32c.h"
#include "nat/nat.h"
#include "rcu/rcu_domain.h"
#include "utils/cuckoo_map.h"

namespace {

using namespace bess::nat;
using bess::conntrack::ParsedFlowPacket;
using bess::conntrack::ParseFrame;

// The legacy module's table types (modules/nat.h before M18).
struct LegacyEntry {
  Endpoint endpoint;
  uint64_t last_refresh;
};
struct LegacyHash {
  size_t operator()(const Endpoint &e) const {
    uint64_t v;
    std::memcpy(&v, &e, 8);
    return bess::arch::Crc32c(v, 0);
  }
};
struct LegacyEq {
  bool operator()(const Endpoint &a, const Endpoint &b) const { return a == b; }
};
using LegacyMap = bess::utils::CuckooMap<Endpoint, LegacyEntry, LegacyHash, LegacyEq>;

constexpr size_t kStream = size_t{1} << 20;  // as many keys as the largest table
constexpr size_t kBatch = 32;

Endpoint Internal(uint64_t r) {
  return {be32_t(0x0a000000u | static_cast<uint32_t>(r & 0xffffff)),
          be16_t(static_cast<uint16_t>(1024 + (r >> 24) % 60000)), 6};
}

void BM_Lookup(benchmark::State &st) {
  const int impl = static_cast<int>(st.range(0));
  const auto n = static_cast<size_t>(st.range(1));
  std::mt19937_64 rng(0x18 + n);
  std::vector<Endpoint> keys;
  std::unique_ptr<LegacyMap> legacy;
  std::unique_ptr<Nat::Table> table;
  if (impl == 0) {
    legacy = std::make_unique<LegacyMap>();
  } else {
    table = Nat::Table::Create(n + n / 4).value();
  }
  uint16_t ext_port = 1;
  uint32_t ext_addr = 0xc6336400;
  while (keys.size() < n) {
    const Endpoint in = Internal(rng());
    if (++ext_port == 0) {
      ext_port = 1;
      ext_addr++;
    }
    const Endpoint ext{be32_t(ext_addr), be16_t(ext_port), 6};
    if (impl == 0) {
      if (legacy->Find(in) != nullptr) continue;
      legacy->Insert(in, LegacyEntry{ext, 0});
      legacy->Insert(ext, LegacyEntry{in, 0});
    } else {
      if (!table->EmplaceAliased(in, ext, Binding{in, ext, 0, 0}).created()) continue;
    }
    keys.push_back(in);
  }
  std::vector<Endpoint> stream(kStream);
  for (auto &k : stream) k = keys[rng() % keys.size()];
  size_t pos = 0;
  uint64_t found = 0;
  for (auto _ : st) {
    const Endpoint *batch = &stream[pos];
    if (impl == 0) {
      legacy->PrefetchBatch(std::span<const Endpoint>(batch, kBatch));
      for (size_t i = 0; i < kBatch; i++) found += legacy->Find(batch[i]) != nullptr;
    } else {
      const Binding *out[kBatch];
      found += std::popcount(table->FindBatch(std::span<const Endpoint>(batch, kBatch),
                                              std::span<const Binding *>(out, kBatch)));
    }
    pos = (pos + kBatch) & (kStream - 1);
  }
  if (found != st.iterations() * kBatch) st.SkipWithError("lookup missed");
  st.counters["ns_per_lookup"] = benchmark::Counter(
      static_cast<double>(st.iterations()) * kBatch,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  st.counters["bytes_per_mapping"] =
      static_cast<double>(impl == 0 ? legacy->MemoryBytes() : table->memory_bytes()) / n;
}

void BM_Allocate(benchmark::State &st) {
  const int impl = static_cast<int>(st.range(0));
  const double fill = st.range(1) / 100.0;
  constexpr uint32_t kLo = 1024, kHi = 65536;
  const uint32_t span = kHi - kLo;
  std::mt19937 rng(0x19);
  // Take `fill` of the span at random.
  std::vector<uint16_t> taken;
  for (uint32_t p = kLo; p < kHi; p++) {
    if (rng() % 1000 < fill * 1000) taken.push_back(static_cast<uint16_t>(p));
  }
  LegacyMap legacy;
  PortPool pool(1);
  const be32_t ext_addr(0xc6336401);
  for (const uint16_t p : taken) {
    if (impl == 0) {
      legacy.Insert(Endpoint{ext_addr, be16_t(p), 6}, LegacyEntry{});
    } else {
      pool.Allocate(0, 6, p, p + 1, 0);
    }
  }
  Random r(7);
  uint64_t fails = 0, ops = 0;
  for (auto _ : st) {
    uint16_t got = 0;
    bool ok = false;
    if (impl == 0) {
      uint16_t port = static_cast<uint16_t>(kLo + r.GetRange(span));
      for (int t = 0; t < 128; t++) {
        if (legacy.Find(Endpoint{ext_addr, be16_t(port), 6}) == nullptr) {
          ok = true;
          got = port;
          break;
        }
        if (++port == 0 || static_cast<uint32_t>(port) >= kHi) port = kLo;  // legacy wrap
      }
    } else {
      const auto p = pool.Allocate(0, 6, kLo, kHi, r.Get());
      ok = p.has_value();
      got = ok ? *p : 0;
      if (ok) pool.Release(0, 6, *p);  // keep the fill constant
    }
    benchmark::DoNotOptimize(got);
    fails += !ok;
    ops++;
  }
  st.counters["fail_pct"] = 100.0 * fails / ops;
}

std::vector<uint8_t> UdpFrame(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport) {
  std::vector<uint8_t> f(14 + 20 + 8 + 4, 0);
  f[12] = 0x08;
  f[14] = 0x45;
  f[17] = 32;
  f[22] = 64;
  f[23] = 17;
  auto put32 = [&](size_t o, uint32_t v) {
    f[o] = v >> 24, f[o + 1] = v >> 16, f[o + 2] = v >> 8, f[o + 3] = v;
  };
  put32(26, src);
  put32(30, dst);
  f[34] = sport >> 8, f[35] = sport & 0xff, f[36] = dport >> 8, f[37] = dport & 0xff;
  f[39] = 12;
  return f;
}


// The legacy module's per-packet path: Ethernet assumed untagged, IHL trusted,
// the endpoint read without checks, one CuckooMap lookup, the legacy rewrite
// (the same increments as Rewrite).
uint64_t LegacyPacket(LegacyMap &map, std::vector<uint8_t> &f, bool reverse) {
  uint8_t *ip = f.data() + 14;
  uint8_t *l4 = ip + ((ip[0] & 0x0f) << 2);
  Endpoint before;
  std::memcpy(&before.addr, ip + (reverse ? 16 : 12), 4);
  std::memcpy(&before.port, l4 + (reverse ? 2 : 0), 2);
  before.protocol = ip[9];
  auto *e = map.Find(before);
  if (e == nullptr) return 0;
  if (!reverse) e->second.last_refresh = 1;
  Rewrite(ip, l4, reverse ? Direction::kReverse : Direction::kForward, before, e->second.endpoint);
  return 1;
}

// N = Nat (the owned NAT) or SharedNat (TP6: one NAT for every worker; one
// RCU reader here, online throughout, as a worker is).
template <typename N>
void TranslateImpl(benchmark::State &st) {
  const bool legacy_path = st.range(0) == 0 || st.range(0) == 2;
  const bool reverse = st.range(1) == 1;
  const auto n = static_cast<uint32_t>(st.range(2));
  LegacyMap legacy;
  typename N::Config c;
  // 20 addresses: room for 1M UDP mappings (64,512 ports each).
  for (uint32_t a = 1; a <= 20; a++) {
    c.addresses.push_back({be32_t(0xc6336400 + a), {{0, 65535, false}}});
  }
  // Paths 1 and 3 size the table as the module does (CapacityFor: here 1M,
  // the cap), so a small set of mappings sits in a sparse table.
  c.capacity = N::CapacityFor(c.addresses);
  bess::rcu::RcuDomain domain(2);
  if constexpr (N::kShared) {
    if (!domain.Register(1).has_value()) std::abort();
    domain.Online(1);
    c.rcu = &domain;
  }
  auto nat = N::Create(c).value();
  std::vector<std::vector<uint8_t>> frames;
  std::vector<ParsedFlowPacket> parsed;
  for (uint32_t i = 0; i < n; i++) {
    auto out = UdpFrame(0x0a000000u + i / 50, static_cast<uint16_t>(2000 + i % 50), 0x08080808, 53);
    // (internal addresses vary, so the hash spreads them over the 20)
    ParsedFlowPacket p;
    (void)ParseFrame(out, p);
    const auto orig = out;
    if (nat->Translate(out, p, Direction::kForward, 0) != Verdict::kTranslated) std::abort();
    {
      // The same mapping in a legacy table: forward and reverse entries.
      Endpoint in, ext;
      std::memcpy(&in.addr, orig.data() + 26, 4);
      std::memcpy(&in.port, orig.data() + 34, 2);
      in.protocol = 17;
      std::memcpy(&ext.addr, out.data() + 26, 4);
      std::memcpy(&ext.port, out.data() + 34, 2);
      ext.protocol = 17;
      legacy.Insert(in, LegacyEntry{ext, 0});
      legacy.Insert(ext, LegacyEntry{in, 0});
    }
    if (reverse) {
      // The reply to what went out.
      const uint32_t ext = static_cast<uint32_t>(out[26]) << 24 | out[27] << 16 | out[28] << 8 | out[29];
      const uint16_t port = static_cast<uint16_t>(out[34] << 8 | out[35]);
      out = UdpFrame(0x08080808, 53, ext, port);
    } else {
      out = UdpFrame(0x0a000000u + i / 50, static_cast<uint16_t>(2000 + i % 50), 0x08080808, 53);
    }
    (void)ParseFrame(out, p);
    frames.push_back(out);
    parsed.push_back(p);
  }
  std::mt19937 rng(0x20);
  std::vector<uint32_t> order(n);
  for (auto &o : order) o = rng() % n;
  std::vector<std::vector<uint8_t>> sf;
  std::vector<ParsedFlowPacket> sp;
  for (const uint32_t i : order) {
    sf.push_back(frames[i]);
    sp.push_back(parsed[i]);
  }
  size_t pos = 0;
  uint64_t ok = 0;
  const auto dir = reverse ? Direction::kReverse : Direction::kForward;
  std::vector<uint8_t> scratch;
  const int path = static_cast<int>(st.range(0));
  constexpr size_t kB = 32;
  std::vector<std::vector<uint8_t>> work(kB);
  for (auto _ : st) {
    if (path <= 1) {
      // Translate a copy so every iteration sees the original header. The
      // checked path parses (ParseFrame) as the module does.
      scratch = sf[pos];
      if (legacy_path) {
        ok += LegacyPacket(legacy, scratch, reverse);
      } else {
        ParsedFlowPacket p;
        (void)ParseFrame(scratch, p);
        ok += nat->Translate(scratch, p, dir, 1) == Verdict::kTranslated;
      }
      pos = pos + 1 == n ? 0 : pos + 1;
      continue;
    }
    // A batch of 32, as the module processes them.
    for (size_t i = 0; i < kB; i++) {
      work[i] = sf[(pos + i) % n];
    }
    if (path == 2) {
      Endpoint keys[kB];
      for (size_t i = 0; i < kB; i++) {
        uint8_t *ip = work[i].data() + 14;
        uint8_t *l4 = ip + ((ip[0] & 0x0f) << 2);
        std::memcpy(&keys[i].addr, ip + (reverse ? 16 : 12), 4);
        std::memcpy(&keys[i].port, l4 + (reverse ? 2 : 0), 2);
        keys[i].protocol = ip[9];
      }
      legacy.PrefetchBatch(std::span<const Endpoint>(keys, kB));
      for (size_t i = 0; i < kB; i++) ok += LegacyPacket(legacy, work[i], reverse);
    } else {
      std::span<uint8_t> frames[kB];
      ParsedFlowPacket ps[kB];
      bool good[kB];
      Verdict v[kB];
      for (size_t i = 0; i < kB; i++) {
        frames[i] = work[i];
        good[i] = ParseFrame(frames[i], ps[i]) == bess::conntrack::ParseStatus::kOk;
      }
      nat->Expire(1, 256);  // as the module does every batch
      nat->TranslateBatch(frames, ps, good, dir, 1, v);
      for (size_t i = 0; i < kB; i++) ok += v[i] == Verdict::kTranslated;
    }
    pos = (pos + kB) % n;
  }
  const uint64_t per = st.range(0) >= 2 ? 32 : 1;
  if (ok != static_cast<uint64_t>(st.iterations()) * per) st.SkipWithError("not translated");
  st.counters["ns_per_packet"] = benchmark::Counter(
      static_cast<double>(st.iterations()) * per,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  st.counters["bytes_per_mapping"] = static_cast<double>(nat->memory_bytes()) / c.capacity;
  if constexpr (N::kShared) {
    domain.Offline(1);  // first: the table's destructor waits for a grace period
    nat.reset();
    domain.Unregister(1);
  }
}

void BM_Translate(benchmark::State &st) { TranslateImpl<Nat>(st); }
void BM_TranslateShared(benchmark::State &st) { TranslateImpl<SharedNat>(st); }

void BM_Bind(benchmark::State &st) {
  constexpr uint32_t kCap = 65536;
  Nat::Config c;
  c.addresses = {{be32_t(0xc6336401), {{0, 65535, false}}}, {be32_t(0xc6336402), {{0, 65535, false}}}};
  c.capacity = kCap;
  std::unique_ptr<Nat> nat = Nat::Create(c).value();
  std::vector<std::vector<uint8_t>> frames;
  std::vector<ParsedFlowPacket> parsed;
  for (uint32_t i = 0; i < kCap; i++) {
    frames.push_back(UdpFrame(0x0a000000u + i, static_cast<uint16_t>(3000 + i % 7), 0x08080808, 53));
    ParsedFlowPacket p;
    (void)ParseFrame(frames.back(), p);
    parsed.push_back(p);
  }
  uint32_t pos = 0;
  std::vector<uint8_t> scratch;
  for (auto _ : st) {
    scratch = frames[pos];
    benchmark::DoNotOptimize(nat->Translate(scratch, parsed[pos], Direction::kForward, 0));
    if (++pos == kCap) {
      st.PauseTiming();
      pos = 0;
      nat.reset();
      nat = Nat::Create(c).value();
      st.ResumeTiming();
    }
  }
}

BENCHMARK(BM_Lookup)->ArgsProduct({{0, 1}, {4096, 65536, 1048576}})->MinTime(0.2);
BENCHMARK(BM_Allocate)->ArgsProduct({{0, 1}, {10, 50, 90, 99}})->MinTime(0.2);
BENCHMARK(BM_Translate)->ArgsProduct({{0, 1, 2, 3}, {0, 1}, {4096, 65536, 1048576}})->MinTime(0.2);
BENCHMARK(BM_TranslateShared)->ArgsProduct({{3}, {0, 1}, {4096, 65536, 1048576}})->MinTime(0.2);
BENCHMARK(BM_Bind)->MinTime(0.2);

}  // namespace
