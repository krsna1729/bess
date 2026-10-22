// SPDX-License-Identifier: BSD-3-Clause

// What a Firewall policy change costs on the control path (D-179): the
// "<name>/rules" resource rebuilds the whole table off the packet path, once
// per transaction, and publishes it in one RCU swap.
//
//   BM_RuleTransaction/rules/ops  one transaction of `ops` upserts, each
//        replacing an existing rule (its drop flag flips), against a module
//        holding `rules` rules over 17 source prefix lengths. Time is per
//        transaction; items are operations. The cost should follow `rules`
//        (one build) and barely `ops` (one map entry each): a transaction of
//        many operations must not rebuild once per operation.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <google/protobuf/any.pb.h>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "module.h"
#include "module_graph.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"

namespace {

using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;

// Rule i: a source prefix of length 16 + i % 17 inside 10.0.0.0/8, TCP, and a
// destination port range of 16 starting at i % 1000.
bess::pb::FirewallArg::Rule RuleFor(uint32_t i, bool drop) {
  bess::pb::FirewallArg::Rule rule;
  const uint32_t addr = 0x0a000000u | ((i * 2654435761u) & 0x00ffffffu);
  rule.set_src_ip(std::to_string(addr >> 24u) + "." +
                  std::to_string((addr >> 16u) & 0xffu) + "." +
                  std::to_string((addr >> 8u) & 0xffu) + "." +
                  std::to_string(addr & 0xffu) + "/" + std::to_string(16 + i % 17));
  rule.set_proto(6);
  rule.set_dst_port_low(i % 1000);
  rule.set_dst_port_high(i % 1000 + 15);
  rule.set_drop(drop);
  return rule;
}

const bess::framework::ResourceCodec &CodecOf(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  const bess::framework::ResourceCodec *codec =
      r == nullptr ? nullptr
                   : bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  if (codec == nullptr) {
    std::abort();
  }
  return *codec;
}

void BM_RuleTransaction(benchmark::State &state) {
  const auto rules = static_cast<uint32_t>(state.range(0));
  const auto ops_per_tx = static_cast<uint32_t>(state.range(1));
  ModuleGraph::DestroyAllModules();

  bess::pb::FirewallArg arg;
  for (uint32_t i = 0; i < rules; i++) {
    *arg.add_rules() = RuleFor(i, false);
  }
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) {
    std::abort();
  }
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(
      ModuleBuilder::all_module_builders().at("Firewall"), "fw", packed, &perr);
  if (m == nullptr || perr.code() != 0) {
    std::abort();
  }
  const std::string resource = "fw/rules";
  const bess::framework::ResourceCodec &codec = CodecOf(resource);

  // Two decoded transactions over the same ids (1..ops), one per drop flag,
  // so every iteration changes `ops` rules and decoding stays out of the
  // timing.
  std::vector<Op> txs[2];
  for (int flip = 0; flip < 2; flip++) {
    for (uint32_t k = 0; k < ops_per_tx; k++) {
      const uint32_t id = 1 + k * (rules / ops_per_tx);
      bess::pb::FirewallRuleKey key;
      key.set_id(id);
      bess::pb::FirewallRuleValue value;
      *value.mutable_rule() = RuleFor(id - 1, flip == 0);
      google::protobuf::Any pk;
      google::protobuf::Any pv;
      if (!pk.PackFrom(key) || !pv.PackFrom(value)) {
        std::abort();
      }
      auto k_enc = codec.Key(pk.type_url(), pk.value());
      auto v_enc = codec.Value(pv.type_url(), pv.value());
      if (!k_enc || !v_enc) {
        std::abort();
      }
      txs[flip].push_back(Op::Upsert(resource, *k_enc, std::move(*v_enc)));
    }
  }

  TransactionEngine &engine = bess::runtime::runtime().transactions();
  uint64_t busy = 0;
  size_t flip = 0;
  for (auto _ : state) {
    TransactionEngine::Outcome outcome;
    while ((outcome = engine.Apply(txs[flip]).outcome) ==
           TransactionEngine::Outcome::kBusy) {
      busy++;
    }
    if (outcome != TransactionEngine::Outcome::kApplied) {
      state.SkipWithError("transaction refused");
      break;
    }
    flip ^= 1u;
  }
  state.SetItemsProcessed(state.iterations() * ops_per_tx);
  state.counters["busy"] = static_cast<double>(busy);
  ModuleGraph::DestroyAllModules();
}
BENCHMARK(BM_RuleTransaction)
    ->ArgsProduct({{1000, 10000, 50000}, {1, 64, 1000}})
    ->Unit(benchmark::kMicrosecond);

}  // namespace
