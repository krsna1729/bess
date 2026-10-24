// SPDX-License-Identifier: BSD-3-Clause

// The firewall table's packet path: ns per key over the shapes a deployment
// has, next to the scalar reference scan (the definition the index must
// reproduce, at the cost a reader of the list pays) and next to the masking
// kernel's portable alternative (the delta classifier/firewall_kernels.h records).
// WildcardMatch's own cost is measured where it belongs: tests/perf/live_perf
// compares the modules end to end.
//
// Args: {rules, batch, dist, family} where
//   dist: 0 = every key hits the first rule (one group is scanned, then the
//              lookup stops),
//         1 = hits spread over the rules,
//         2 = every key misses (every group is scanned).
//   family: 4 or 6.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "classifier/firewall_kernels.h"
#include "classifier/firewall_table.h"
#include "testing/rng.h"

namespace {

using bess::classifier::FirewallAddr;
using bess::classifier::FirewallFamily;
using bess::classifier::FirewallKey;
using bess::classifier::FirewallRule;
using bess::classifier::FirewallTable;

constexpr size_t kMaxBatch = 32;
constexpr uint8_t kTcp = 6;
constexpr uint8_t kUdp = 17;

// The four shapes a rule list is spread over, so that the table has four
// groups (few groups, many entries: what a firewall has) instead of one:
//   tcp /24 any, udp /24 any, tcp /24 /24, tcp /32 any.
struct Shape {
  uint8_t proto;
  uint8_t src_len;
  uint8_t dst_len;
};
constexpr Shape kShapes[4] = {{kTcp, 24, 0}, {kUdp, 24, 0}, {kTcp, 24, 24}, {kTcp, 32, 0}};

// A v4 network whose low byte is zero, from an index; the same index in the
// second byte keeps every rule's network distinct up to 65536 rules.
FirewallAddr Net4(uint32_t index, uint8_t second_octet) {
  FirewallAddr out{};
  out[0] = 10;
  out[1] = second_octet;
  out[2] = static_cast<uint8_t>((index >> 8) & 0xff);
  out[3] = static_cast<uint8_t>(index & 0xff);
  return out;
}

FirewallAddr Net6(uint32_t index) {
  FirewallAddr out{};
  out[0] = 0x20;
  out[1] = 0x01;
  out[2] = 0x0d;
  out[3] = 0xb8;
  out[4] = static_cast<uint8_t>((index >> 8) & 0xff);
  out[5] = static_cast<uint8_t>(index & 0xff);
  return out;
}

struct Workload {
  std::vector<FirewallRule> rules;
  std::vector<FirewallKey> keys;
  std::unique_ptr<const FirewallTable> table;
};

FirewallRule RuleFor(uint32_t index, FirewallFamily family) {
  const Shape &shape = kShapes[index % 4];
  FirewallRule rule;
  rule.family = family;
  rule.src_net = family == FirewallFamily::kIpv4 ? Net4(index, 1) : Net6(index);
  rule.src_len = family == FirewallFamily::kIpv4 ? shape.src_len
                                                : (shape.src_len == 32 ? 128 : 64);
  if (shape.dst_len != 0) {
    rule.dst_net = family == FirewallFamily::kIpv4 ? Net4(index, 2) : Net6(index + 0x1000);
    rule.dst_len = family == FirewallFamily::kIpv4 ? shape.dst_len : 64;
  }
  rule.proto = shape.proto;
  // Half the rules constrain a port, as a policy's rules usually do.
  if (index % 2 == 0) {
    rule.dst_port_low = 80;
    rule.dst_port_high = static_cast<uint16_t>(80 + index % 16);
  }
  rule.action = 1;
  return rule;
}

FirewallKey KeyFor(const Workload &workload, uint32_t index, int dist,
                   FirewallFamily family, size_t batch) {
  FirewallKey key;
  key.family = family;
  key.proto = kTcp;
  key.dst_port = 80;
  key.src_port = 1024;
  if (dist == 2) {
    // Nothing matches: a network no rule names (the shapes all name 10/8 or
    // 2001:db8::/32 networks, and the /32 and /128 shapes are exact hosts).
    key.src = family == FirewallFamily::kIpv4 ? Net4(0xffff, 200) : Net6(0xffff);
    if (family == FirewallFamily::kIpv6) {
      key.src[4] = 0xff;
      key.src[5] = 0xff;
    }
    key.dst = key.src;
    return key;
  }
  const uint32_t rule =
      dist == 0 ? 0 : static_cast<uint32_t>(index * workload.rules.size() / batch);
  const FirewallRule &r = workload.rules[rule];
  key.proto = r.proto == 0 ? kTcp : r.proto;
  // The rule's own network: at a /32 or /128 that is the whole address, so the
  // key is inside the rule by construction.
  key.src = r.src_net;
  if (r.dst_len != 0) {
    key.dst = r.dst_net;
  }
  key.dst_port = r.dst_port_low == 0 ? 80 : r.dst_port_low;
  return key;
}

Workload Make(size_t rules, size_t batch, int dist, FirewallFamily family) {
  Workload workload;
  workload.rules.reserve(rules);
  for (uint32_t i = 0; i < rules; i++) {
    workload.rules.push_back(RuleFor(i, family));
  }
  auto built = FirewallTable::Build(workload.rules);
  if (!built) {
    return workload;
  }
  workload.table = std::move(*built);
  workload.keys.reserve(batch);
  for (uint32_t i = 0; i < batch; i++) {
    workload.keys.push_back(
        KeyFor(workload, i, dist, family, batch));
  }
  return workload;
}

// The definition, at the cost a reader of the list pays: the first rule that
// matches, masked per rule. The rule side is masked here too: these rules carry
// host bits inside their prefixes (the generator's addresses are not canonical
// networks), and a sloppy prefix means its network, so an unmasked compare
// would skip the rule a key targets and turn the baseline into a near-full
// sweep.
uint32_t ScalarScan(const std::vector<FirewallRule> &rules,
                    const FirewallKey &key) {
  for (size_t i = 0; i < rules.size(); i++) {
    const FirewallRule &r = rules[i];
    if (r.family != FirewallFamily::kAny && r.family != key.family) {
      continue;
    }
    if (r.proto != 0 && r.proto != key.proto) {
      continue;
    }
    if (r.src_len != 0 &&
        FirewallTable::MaskedAddr(key.src, r.src_len, r.family) !=
            FirewallTable::MaskedAddr(r.src_net, r.src_len, r.family)) {
      continue;
    }
    if (r.dst_len != 0 &&
        FirewallTable::MaskedAddr(key.dst, r.dst_len, r.family) !=
            FirewallTable::MaskedAddr(r.dst_net, r.dst_len, r.family)) {
      continue;
    }
    if (key.src_port < r.src_port_low || key.src_port > r.src_port_high) {
      continue;
    }
    if (key.dst_port < r.dst_port_low || key.dst_port > r.dst_port_high) {
      continue;
    }
    return static_cast<uint32_t>(i) + 1;
  }
  return 0;
}

FirewallFamily Family(int64_t family) {
  return family == 6 ? FirewallFamily::kIpv6 : FirewallFamily::kIpv4;
}

void Report(benchmark::State &state, const Workload &workload, size_t batch) {
  state.counters["rules"] = benchmark::Counter(
      static_cast<double>(workload.rules.size()), benchmark::Counter::kAvgThreads);
  state.counters["groups"] = benchmark::Counter(
      workload.table ? static_cast<double>(workload.table->stats().groups) : 0.0,
      benchmark::Counter::kAvgThreads);
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch));
}

// The indexed lookup: one batch of keys per iteration.
void BM_FirewallLookup(benchmark::State &state) {
  const size_t rules = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int dist = static_cast<int>(state.range(2));
  const FirewallFamily family = Family(state.range(3));
  const Workload workload = Make(rules, batch, dist, family);
  if (!workload.table) {
    state.SkipWithError("table build failed");
    return;
  }
  std::vector<FirewallTable::Match> matches(batch);
  for (auto _ : state) {
    const uint64_t matched = workload.table->LookupBatch(
        workload.keys.data(), batch, matches.data(), false);
    benchmark::DoNotOptimize(matched);
    benchmark::DoNotOptimize(matches);
  }
  Report(state, workload, batch);
}

// The same keys against the scalar scan.
void BM_FirewallScalarScan(benchmark::State &state) {
  const size_t rules = static_cast<size_t>(state.range(0));
  const size_t batch = static_cast<size_t>(state.range(1));
  const int dist = static_cast<int>(state.range(2));
  const Workload workload = Make(rules, batch, dist, Family(state.range(3)));
  for (auto _ : state) {
    uint32_t matched = 0;
    for (size_t i = 0; i < batch; i++) {
      matched += ScalarScan(workload.rules, workload.keys[i]);
    }
    benchmark::DoNotOptimize(matched);
  }
  Report(state, workload, batch);
}

// The masking kernel against the portable loop it replaced: two 16-byte
// addresses, `iterations` pairs per iteration.
void BM_FirewallMaskKernel(benchmark::State &state) {
  alignas(32) uint8_t src[16];
  alignas(32) uint8_t dst[16];
  alignas(32) uint8_t mask_src[16];
  alignas(32) uint8_t mask_dst[16];
  alignas(32) uint8_t out[32];
  std::mt19937 rng = bess::testing::SeededRng(11);
  for (size_t i = 0; i < 16; i++) {
    src[i] = static_cast<uint8_t>(rng());
    dst[i] = static_cast<uint8_t>(rng());
    mask_src[i] = i < 8 ? 0xff : 0;
    mask_dst[i] = i < 12 ? 0xff : 0;
  }
  const bool kernel = state.range(0) != 0;
  for (auto _ : state) {
    if (kernel) {
      bess::arch::MaskedAddrPair16x2(out, src, dst, mask_src, mask_dst);
    } else {
      for (size_t i = 0; i < 16; i++) {
        out[i] = static_cast<uint8_t>(src[i] & mask_src[i]);
        out[16 + i] = static_cast<uint8_t>(dst[i] & mask_dst[i]);
      }
    }
    benchmark::DoNotOptimize(out);
  }
  state.counters["kernel"] = benchmark::Counter(kernel ? 1 : 0);
}

// The control plane: building the table (what an apply costs).
void BM_FirewallBuild(benchmark::State &state) {
  const size_t rules = static_cast<size_t>(state.range(0));
  const FirewallFamily family = Family(state.range(1));
  std::vector<FirewallRule> list;
  list.reserve(rules);
  for (uint32_t i = 0; i < rules; i++) {
    list.push_back(RuleFor(i, family));
  }
  for (auto _ : state) {
    auto built = FirewallTable::Build(list);
    benchmark::DoNotOptimize(built);
  }
  state.counters["rules"] = benchmark::Counter(
      static_cast<double>(rules), benchmark::Counter::kAvgThreads);
}

constexpr size_t kBatch = kMaxBatch;

BENCHMARK(BM_FirewallLookup)
    ->Args({10, kBatch, 0, 4})
    ->Args({10, kBatch, 1, 4})
    ->Args({10, kBatch, 2, 4})
    ->Args({1000, kBatch, 0, 4})
    ->Args({1000, kBatch, 1, 4})
    ->Args({1000, kBatch, 2, 4})
    ->Args({10000, kBatch, 1, 4})
    ->Args({10000, kBatch, 2, 4})
    ->Args({1000, kBatch, 1, 6})
    ->Args({1000, kBatch, 2, 6});
BENCHMARK(BM_FirewallScalarScan)
    ->Args({10, kBatch, 0, 4})
    ->Args({10, kBatch, 2, 4})
    ->Args({1000, kBatch, 1, 4});
BENCHMARK(BM_FirewallMaskKernel)->Arg(1)->Arg(0);
BENCHMARK(BM_FirewallBuild)->Args({1000, 4})->Args({10000, 4});

}  // namespace
