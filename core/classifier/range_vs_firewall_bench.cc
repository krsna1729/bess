// SPDX-License-Identifier: BSD-3-Clause

// One IPv4 5-tuple policy, two tables (D-186): WildcardMatch's table with
// per-field ranges (ConcurrentMaskedTable over WildcardMatch's 13-byte key:
// source, destination, protocol, source port, destination port) against the
// firewall table (FirewallTable, D-147) and, for small policies, its scan
// (FirewallScan). Rule i of the policy is the same rule in both: the firewall's
// list position i (the first match wins), WildcardMatch's priority
// `rules - i` (the highest wins). Every key of every stream is checked to get
// the same winner from every table before anything is timed.
//
//   BM_Lookup/impl/rules/shape/dist  32 keys per iteration, from a stream of
//        4,096 keys (no rule id requested, as a gate-only module asks).
//        impl: 0 = ConcurrentMaskedTable, 1 = FirewallTable, 2 = FirewallScan.
//        shape: 0 = prefixes only, 1 = a destination port range per rule,
//               2 = source and destination port ranges.
//        dist: 0 = each key inside one rule (the winner may be a better
//              overlapping rule), spread over the policy; 1 = every key
//              misses every rule; 2 = the first 32 keys of dist 0, repeated
//              (one batch looked up again and again, as
//              classifier_firewall_table_bench does: branch predictors and
//              caches learn it, so it reads as a best case).
//        Counters: tuples (WildcardMatch's distinct masks) or groups (the
//        firewall's), and bytes_per_rule: heap and DPDK heap growth while the
//        table was built, over its rules.
//   BM_TableUpdate/impl/rules/shape  one rule added to the policy and removed
//        again, at table level: impl 0 = ConcurrentMaskedTable::Upsert and
//        Erase in place; impl 1 = FirewallTable::Build of the policy with the
//        rule and without it (the table has no other update, D-147).
//   BM_ModuleUpdate/impl/rules/shape  the same change through the module's
//        "<name>/rules" resource, as two transactions (the add, then the
//        erase), operations decoded beforehand as modules_firewall_update_bench
//        does: impl 0 = WildcardMatch, 1 = Firewall (one build per
//        transaction, D-179).

#include <benchmark/benchmark.h>

#include <malloc.h>
#include <rte_malloc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <google/protobuf/any.pb.h>

#include "classifier/concurrent_masked.h"
#include "classifier/firewall_table.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "module.h"
#include "module_graph.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"

namespace {

using bess::classifier::ConcurrentMaskedTable;
using bess::classifier::ConstBytes;
using bess::classifier::FirewallFamily;
using bess::classifier::FirewallKey;
using bess::classifier::FirewallRule;
using bess::classifier::FirewallScan;
using bess::classifier::FirewallTable;
using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;

constexpr size_t kBatch = 32;
constexpr size_t kStream = 4096;
constexpr size_t kKeyLen = 13;  // src 4, dst 4, proto 1, sport 2, dport 2
constexpr uint8_t kTcp = 6;
constexpr uint8_t kUdp = 17;

// Eight (source, destination) prefix-length pairs: eight masks for
// WildcardMatch, sixteen groups for the firewall (its groups also split by
// protocol).
constexpr std::array<std::pair<uint8_t, uint8_t>, 8> kPrefixes = {
    {{24, 0}, {32, 0}, {16, 0}, {24, 24}, {32, 24}, {20, 0}, {0, 24}, {28, 16}}};

struct PolicyRule {
  uint32_t src = 0, dst = 0;
  uint8_t src_len = 0, dst_len = 0, proto = 0;
  uint16_t sport_low = 0, sport_high = 0xffff, dport_low = 0, dport_high = 0xffff;
  uint16_t action = 0;
};

uint32_t PrefixMask(uint8_t len) {
  return len == 0 ? 0 : ~uint32_t{0} << (32 - len);
}

// Rule i: the i-th network of its prefix length (distinct for every i under
// 2^len), so no two rules are the same rule; shorter prefixes overlap longer
// ones, and priority decides.
PolicyRule RuleFor(uint32_t i, int shape) {
  const auto [src_len, dst_len] = kPrefixes[i % kPrefixes.size()];
  PolicyRule r;
  r.src_len = src_len;
  r.dst_len = dst_len;
  r.src = src_len == 0 ? 0 : (i << (32 - src_len)) & PrefixMask(src_len);
  r.dst = dst_len == 0 ? 0
          : dst_len == 16 ? 0xac100000u
                          : (i << (32 - dst_len)) & PrefixMask(dst_len);
  r.proto = i % 3 == 0 ? kUdp : kTcp;
  if (shape >= 1) {
    static constexpr std::array<uint16_t, 4> kWidths = {0, 15, 999, 9999};
    r.dport_low = static_cast<uint16_t>(1024 + (i * 37) % 50000);
    r.dport_high = static_cast<uint16_t>(r.dport_low + kWidths[i % 4]);
  }
  if (shape == 2) {
    r.sport_low = static_cast<uint16_t>(1024 + (i * 53) % 40000);
    r.sport_high = static_cast<uint16_t>(r.sport_low + (i % 3 == 0 ? 4095 : 63));
  }
  r.action = static_cast<uint16_t>(i % 4096 + 1);
  return r;
}

FirewallRule ToFirewall(const PolicyRule &p) {
  FirewallRule r;
  r.family = FirewallFamily::kIpv4;
  for (int b = 0; b < 4; b++) {
    r.src_net[b] = static_cast<uint8_t>(p.src >> (24 - 8 * b));
    r.dst_net[b] = static_cast<uint8_t>(p.dst >> (24 - 8 * b));
  }
  r.src_len = p.src_len;
  r.dst_len = p.dst_len;
  r.proto = p.proto;
  r.src_port_low = p.sport_low;
  r.src_port_high = p.sport_high;
  r.dst_port_low = p.dport_low;
  r.dst_port_high = p.dport_high;
  r.action = p.action;
  return r;
}

void Put32(std::byte *at, uint32_t v) {
  for (int b = 0; b < 4; b++) at[b] = std::byte{static_cast<uint8_t>(v >> (24 - 8 * b))};
}
void Put16(std::byte *at, uint16_t v) {
  at[0] = std::byte{static_cast<uint8_t>(v >> 8)};
  at[1] = std::byte{static_cast<uint8_t>(v)};
}

// WildcardMatch's form of a rule: the mask and value of its prefixes and
// protocol, and a range per constrained port.
struct MaskedForm {
  std::array<std::byte, kKeyLen> mask{}, value{};
  std::vector<ConcurrentMaskedTable::KeyRange> ranges;
};
MaskedForm ToMasked(const PolicyRule &p) {
  MaskedForm f;
  Put32(f.mask.data(), PrefixMask(p.src_len));
  Put32(f.mask.data() + 4, PrefixMask(p.dst_len));
  f.mask[8] = std::byte{0xff};
  Put32(f.value.data(), p.src);
  Put32(f.value.data() + 4, p.dst);
  f.value[8] = std::byte{p.proto};
  if (p.sport_low != 0 || p.sport_high != 0xffff) {
    f.ranges.push_back({9, 2, p.sport_low, p.sport_high});
  }
  if (p.dport_low != 0 || p.dport_high != 0xffff) {
    f.ranges.push_back({11, 2, p.dport_low, p.dport_high});
  }
  return f;
}

// Bytes allocated on the heap (small and mmapped blocks) and the DPDK heap,
// once every retired object of the runtime's domain is freed.
size_t AllocatedBytes() {
  bess::runtime::runtime().rcu().Drain();
  size_t dpdk = 0;
  for (unsigned socket = 0; socket < RTE_MAX_NUMA_NODES; socket++) {
    rte_malloc_socket_stats stats{};
    if (rte_malloc_get_socket_stats(static_cast<int>(socket), &stats) == 0) {
      dpdk += stats.heap_allocsz_bytes;
    }
  }
  const struct mallinfo2 heap = mallinfo2();
  return heap.uordblks + heap.hblkhd + dpdk;
}

struct Workload {
  size_t rules = 0;
  int shape = 0;
  int dist = 0;
  std::vector<PolicyRule> policy;
  std::unique_ptr<ConcurrentMaskedTable> masked;
  std::unique_ptr<const FirewallTable> firewall;
  std::optional<FirewallScan> scan;
  double masked_bytes_per_rule = 0, firewall_bytes_per_rule = 0;
  std::vector<std::byte> masked_keys;  // kStream keys of kKeyLen bytes
  std::vector<FirewallKey> firewall_keys;
};

// One workload per (rules, shape, dist), built once for every impl.
Workload *Get(size_t rules, int shape, int dist) {
  static std::map<std::tuple<size_t, int, int>, std::unique_ptr<Workload>> cache;
  auto &slot = cache[{rules, shape, dist}];
  if (slot != nullptr) {
    return slot.get();
  }
  auto w = std::make_unique<Workload>();
  w->rules = rules;
  w->shape = shape;
  w->dist = dist;
  std::vector<FirewallRule> list;
  for (uint32_t i = 0; i < rules; i++) {
    w->policy.push_back(RuleFor(i, shape));
    list.push_back(ToFirewall(w->policy.back()));
  }

  // A first table pays the process's one-time costs (the runtime's domain,
  // DPDK heap metadata): pay them before measuring one.
  static const bool warmed = [] {
    auto first = ConcurrentMaskedTable::Create(kKeyLen, 1, bess::runtime::runtime().rcu());
    const std::array<std::byte, kKeyLen> zero{};
    return first && (*first)->Upsert(ConstBytes(zero), ConstBytes(zero), 0, 1) ==
                        ConcurrentMaskedTable::UpsertResult::kInserted;
  }();
  if (!warmed) std::abort();
  const auto per_rule = [rules](size_t before) {
    return static_cast<double>(static_cast<int64_t>(AllocatedBytes() - before)) /
           static_cast<double>(rules);
  };
  size_t before = AllocatedBytes();
  auto created = ConcurrentMaskedTable::Create(kKeyLen, 1024, bess::runtime::runtime().rcu());
  if (!created) std::abort();
  w->masked = std::move(*created);
  for (uint32_t i = 0; i < rules; i++) {
    const MaskedForm f = ToMasked(w->policy[i]);
    if (w->masked->Upsert(ConstBytes(f.mask), ConstBytes(f.value),
                          static_cast<int64_t>(rules - i), w->policy[i].action, 0,
                          f.ranges) != ConcurrentMaskedTable::UpsertResult::kInserted) {
      std::abort();
    }
  }
  w->masked_bytes_per_rule = per_rule(before);
  before = AllocatedBytes();
  auto built = FirewallTable::Build(list);
  if (!built) std::abort();
  w->firewall = std::move(*built);
  w->firewall_bytes_per_rule = per_rule(before);
  if (rules <= 1000) {
    auto scan = FirewallScan::Create(list);
    if (!scan) std::abort();
    w->scan = std::move(*scan);
  }

  std::mt19937_64 rng(0x5eed + rules * 3 + static_cast<uint64_t>(shape));
  w->masked_keys.resize(kStream * kKeyLen);
  for (size_t k = 0; k < kStream; k++) {
    FirewallKey key;
    key.family = FirewallFamily::kIpv4;
    uint32_t src, dst;
    if (dist == 1) {
      // No rule names 250/8 or 251/8 (sources and destinations stay below
      // 0x28000000, or are 172.16/16).
      src = 0xfa000000u | static_cast<uint32_t>(rng() & 0xffffff);
      dst = 0xfb000000u | static_cast<uint32_t>(rng() & 0xffffff);
      key.proto = kTcp;
      key.src_port = static_cast<uint16_t>(rng());
      key.dst_port = static_cast<uint16_t>(rng());
    } else {
      const PolicyRule &r = w->policy[rng() % rules];
      src = r.src | (static_cast<uint32_t>(rng()) & ~PrefixMask(r.src_len));
      dst = r.dst | (static_cast<uint32_t>(rng()) & ~PrefixMask(r.dst_len));
      key.proto = r.proto;
      key.src_port = static_cast<uint16_t>(r.sport_low + rng() % (r.sport_high - r.sport_low + 1u));
      key.dst_port = static_cast<uint16_t>(r.dport_low + rng() % (r.dport_high - r.dport_low + 1u));
    }
    for (int b = 0; b < 4; b++) {
      key.src[b] = static_cast<uint8_t>(src >> (24 - 8 * b));
      key.dst[b] = static_cast<uint8_t>(dst >> (24 - 8 * b));
    }
    std::byte *mk = w->masked_keys.data() + k * kKeyLen;
    Put32(mk, src);
    Put32(mk + 4, dst);
    mk[8] = std::byte{key.proto};
    Put16(mk + 9, key.src_port);
    Put16(mk + 11, key.dst_port);
    w->firewall_keys.push_back(key);
  }
  if (dist == 2) {
    // One batch over and over: the rest of the stream repeats its first.
    for (size_t k = kBatch; k < kStream; k++) {
      std::memcpy(w->masked_keys.data() + k * kKeyLen,
                  w->masked_keys.data() + (k % kBatch) * kKeyLen, kKeyLen);
      w->firewall_keys[k] = w->firewall_keys[k % kBatch];
    }
  }

  // The tables agree on every key's winner (the masked table's rule i has
  // priority rules - i), or nothing here is a comparison.
  for (size_t k = 0; k < kStream; k++) {
    ConcurrentMaskedTable::Match m{};
    const bool masked_hit =
        w->masked->LookupBatch(ConstBytes(w->masked_keys.data() + k * kKeyLen, kKeyLen),
                               kKeyLen, &m, 1) != 0;
    const size_t masked_rule =
        masked_hit ? rules - static_cast<size_t>(w->masked->RuleAt(m.rule_id).priority) + 1 : 0;
    FirewallTable::Match f{};
    const size_t firewall_rule = w->firewall->Lookup(w->firewall_keys[k], &f) ? f.rule_id : 0;
    if (masked_rule != firewall_rule || (dist != 1) != (firewall_rule != 0)) {
      std::abort();
    }
    if (w->scan) {
      FirewallTable::Match s{};
      if ((w->scan->Lookup(w->firewall_keys[k], &s) ? s.rule_id : 0) != firewall_rule) {
        std::abort();
      }
    }
  }
  slot = std::move(w);
  return slot.get();
}

void BM_Lookup(benchmark::State &state) {
  const int impl = static_cast<int>(state.range(0));
  const auto rules = static_cast<size_t>(state.range(1));
  const int shape = static_cast<int>(state.range(2));
  const int dist = static_cast<int>(state.range(3));
  const Workload &w = *Get(rules, shape, dist);
  if (impl == 2 && !w.scan) {
    state.SkipWithError("no scan at this size");
    return;
  }
  std::array<ConcurrentMaskedTable::Match, kBatch> masked_matches;
  std::array<FirewallTable::Match, kBatch> firewall_matches;
  size_t offset = 0;
  uint64_t hits = 0;
  for (auto _ : state) {
    uint64_t matched;
    if (impl == 0) {
      matched = w.masked->LookupBatch(
          ConstBytes(w.masked_keys.data() + offset * kKeyLen, kBatch * kKeyLen), kKeyLen,
          masked_matches.data(), kBatch, false);
      benchmark::DoNotOptimize(masked_matches);
    } else if (impl == 1) {
      matched = w.firewall->LookupBatch(w.firewall_keys.data() + offset, kBatch,
                                        firewall_matches.data(), false);
      benchmark::DoNotOptimize(firewall_matches);
    } else {
      matched = w.scan->LookupBatch(w.firewall_keys.data() + offset, kBatch,
                                    firewall_matches.data(), false);
      benchmark::DoNotOptimize(firewall_matches);
    }
    hits += static_cast<uint64_t>(__builtin_popcountll(matched));
    offset = (offset + kBatch) % kStream;
  }
  benchmark::DoNotOptimize(hits);
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBatch));
  state.counters["tuples"] = static_cast<double>(w.masked->tuple_count());
  state.counters["groups"] = static_cast<double>(w.firewall->stats().groups);
  state.counters["bytes_per_rule"] =
      impl == 0 ? w.masked_bytes_per_rule : w.firewall_bytes_per_rule;
  state.SetLabel(impl == 0 ? "wildcard_match" : impl == 1 ? "firewall" : "firewall_scan");
}

// Extra rules for the update benchmarks: beyond the policy's indexes, so
// none of them is already a rule.
PolicyRule ExtraRule(size_t rules, int shape, size_t k) {
  return RuleFor(static_cast<uint32_t>(rules + 1 + k), shape);
}

void BM_TableUpdate(benchmark::State &state) {
  const int impl = static_cast<int>(state.range(0));
  const auto rules = static_cast<size_t>(state.range(1));
  const int shape = static_cast<int>(state.range(2));
  Workload &w = *Get(rules, shape, 0);
  constexpr size_t kExtra = 64;
  size_t k = 0;
  if (impl == 0) {
    std::vector<MaskedForm> extra;
    for (size_t i = 0; i < kExtra; i++) extra.push_back(ToMasked(ExtraRule(rules, shape, i)));
    for (auto _ : state) {
      const MaskedForm &f = extra[k];
      auto r = w.masked->Upsert(ConstBytes(f.mask), ConstBytes(f.value), 0, 1, 0, f.ranges);
      benchmark::DoNotOptimize(r);
      if (!w.masked->Erase(ConstBytes(f.mask), ConstBytes(f.value), f.ranges)) {
        state.SkipWithError("erase failed");
        break;
      }
      k = (k + 1) % kExtra;
    }
  } else {
    std::vector<FirewallRule> list;
    for (const PolicyRule &p : w.policy) list.push_back(ToFirewall(p));
    for (auto _ : state) {
      list.push_back(ToFirewall(ExtraRule(rules, shape, k)));
      auto with = FirewallTable::Build(list);
      benchmark::DoNotOptimize(with);
      list.pop_back();
      auto without = FirewallTable::Build(list);
      benchmark::DoNotOptimize(without);
      k = (k + 1) % kExtra;
    }
  }
  state.SetItemsProcessed(state.iterations());
  state.SetLabel(impl == 0 ? "wildcard_match in place" : "firewall rebuild x2");
}

const bess::framework::ResourceCodec &CodecOf(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  const bess::framework::ResourceCodec *codec =
      r == nullptr ? nullptr : bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  if (codec == nullptr) std::abort();
  return *codec;
}

std::string Dotted(uint32_t a) {
  return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 0xff) + "." +
         std::to_string((a >> 8) & 0xff) + "." + std::to_string(a & 0xff);
}

bess::pb::FirewallArg::Rule FirewallPb(const PolicyRule &p) {
  bess::pb::FirewallArg::Rule rule;
  if (p.src_len != 0) rule.set_src_ip(Dotted(p.src) + "/" + std::to_string(p.src_len));
  if (p.dst_len != 0) rule.set_dst_ip(Dotted(p.dst) + "/" + std::to_string(p.dst_len));
  rule.set_proto(p.proto);
  rule.set_src_port_low(p.sport_low);
  rule.set_src_port_high(p.sport_high);
  rule.set_dst_port_low(p.dport_low);
  rule.set_dst_port_high(p.dport_high);
  return rule;
}

bess::pb::WildcardMatchRuleKey WildcardPb(const PolicyRule &p) {
  bess::pb::WildcardMatchRuleKey key;
  const MaskedForm f = ToMasked(p);
  size_t at = 0;
  for (const size_t width : {4, 4, 1, 2, 2}) {
    key.add_values()->set_value_bin(reinterpret_cast<const char *>(f.value.data() + at), width);
    key.add_masks()->set_value_bin(reinterpret_cast<const char *>(f.mask.data() + at), width);
    at += width;
  }
  if (p.sport_low != 0 || p.sport_high != 0xffff) {
    auto *r = key.add_ranges();
    r->set_field(3);
    r->set_low(p.sport_low);
    r->set_high(p.sport_high);
  }
  if (p.dport_low != 0 || p.dport_high != 0xffff) {
    auto *r = key.add_ranges();
    r->set_field(4);
    r->set_low(p.dport_low);
    r->set_high(p.dport_high);
  }
  return key;
}

template <typename Key, typename Value>
std::pair<std::string, std::any> Encode(const bess::framework::ResourceCodec &codec,
                                        const Key &key, const Value &value) {
  google::protobuf::Any pk, pv;
  if (!pk.PackFrom(key) || !pv.PackFrom(value)) std::abort();
  auto k = codec.Key(pk.type_url(), pk.value());
  auto v = codec.Value(pv.type_url(), pv.value());
  if (!k || !v) std::abort();
  return {std::move(*k), std::move(*v)};
}

Module *Create(const char *cls, const std::string &name, const google::protobuf::Message &arg) {
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) std::abort();
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(ModuleBuilder::all_module_builders().at(cls), name,
                                        packed, &perr);
  if (m == nullptr || perr.code() != 0) std::abort();
  return m;
}

bool ApplyAll(TransactionEngine &engine, const std::vector<Op> &ops, uint64_t *busy) {
  TransactionEngine::Outcome outcome;
  while ((outcome = engine.Apply(ops).outcome) == TransactionEngine::Outcome::kBusy) {
    (*busy)++;
  }
  return outcome == TransactionEngine::Outcome::kApplied;
}

void BM_ModuleUpdate(benchmark::State &state) {
  const int impl = static_cast<int>(state.range(0));
  const auto rules = static_cast<size_t>(state.range(1));
  const int shape = static_cast<int>(state.range(2));
  ModuleGraph::DestroyAllModules();
  TransactionEngine &engine = bess::runtime::runtime().transactions();
  uint64_t busy = 0;
  std::string resource;
  std::vector<Op> add, erase;
  if (impl == 0) {
    bess::pb::WildcardMatchArg arg;
    // The 5-tuple of an IPv4 TCP/UDP frame, in the key's order.
    constexpr std::array<std::pair<int, int>, 5> kFields = {
        {{26, 4}, {30, 4}, {23, 1}, {34, 2}, {36, 2}}};
    for (const auto &[offset, size] : kFields) {
      auto *f = arg.add_fields();
      f->set_offset(offset);
      f->set_num_bytes(size);
    }
    arg.set_max_tuples(1024);
    Create("WildcardMatch", "wm", arg);
    resource = "wm/rules";
    const auto &codec = CodecOf(resource);
    std::vector<Op> load;
    for (uint32_t i = 0; i < rules; i++) {
      bess::pb::WildcardMatchRuleValue value;
      value.set_priority(static_cast<int64_t>(rules - i));
      value.set_gate(1);
      auto [k, v] = Encode(codec, WildcardPb(RuleFor(i, shape)), value);
      load.push_back(Op::Upsert(resource, std::move(k), std::move(v)));
    }
    if (!ApplyAll(engine, load, &busy)) {
      state.SkipWithError("policy refused");
      return;
    }
    bess::pb::WildcardMatchRuleValue value;
    value.set_gate(1);
    auto [k, v] = Encode(codec, WildcardPb(ExtraRule(rules, shape, 0)), value);
    add.push_back(Op::Upsert(resource, k, std::move(v)));
    erase.push_back(Op::Erase(resource, k));
  } else {
    bess::pb::FirewallArg arg;
    for (uint32_t i = 0; i < rules; i++) *arg.add_rules() = FirewallPb(RuleFor(i, shape));
    Create("Firewall", "fw", arg);
    resource = "fw/rules";
    const auto &codec = CodecOf(resource);
    bess::pb::FirewallRuleKey key;
    key.set_id(static_cast<uint32_t>(rules + 1));
    bess::pb::FirewallRuleValue value;
    *value.mutable_rule() = FirewallPb(ExtraRule(rules, shape, 0));
    auto [k, v] = Encode(codec, key, value);
    add.push_back(Op::Upsert(resource, k, std::move(v)));
    erase.push_back(Op::Erase(resource, k));
  }
  for (auto _ : state) {
    if (!ApplyAll(engine, add, &busy) || !ApplyAll(engine, erase, &busy)) {
      state.SkipWithError("transaction refused");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["busy"] = static_cast<double>(busy);
  state.SetLabel(impl == 0 ? "wildcard_match" : "firewall");
  ModuleGraph::DestroyAllModules();
}

constexpr std::array<int64_t, 4> kSizes = {10, 100, 1000, 10000};

[[maybe_unused]] const bool kRegistered = [] {
  for (const int64_t rules : kSizes) {
    for (int64_t shape = 0; shape < 3; shape++) {
      for (int64_t dist = 0; dist < 3; dist++) {
        for (int64_t impl = 0; impl < 3; impl++) {
          if (impl == 2 && rules > 100) continue;
          benchmark::RegisterBenchmark("BM_Lookup", BM_Lookup)
              ->Args({impl, rules, shape, dist});
        }
      }
      for (int64_t impl = 0; impl < 2; impl++) {
        benchmark::RegisterBenchmark("BM_TableUpdate", BM_TableUpdate)
            ->Args({impl, rules, shape})
            ->Unit(benchmark::kMicrosecond);
        benchmark::RegisterBenchmark("BM_ModuleUpdate", BM_ModuleUpdate)
            ->Args({impl, rules, shape})
            ->Unit(benchmark::kMicrosecond);
      }
    }
  }
  return true;
}();

}  // namespace
