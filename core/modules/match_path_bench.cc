// SPDX-License-Identifier: BSD-3-Clause

// ExactMatch's and WildcardMatch's packet path through ProcessBatch, as a
// worker runs it, result placement included; and a rule change through each
// module's "<name>/rules" resource. A feed task emits one 32-packet batch into
// the module per iteration; the module classifies it, places the matched
// records and emits it to a sink that keeps the packets for a later
// iteration, so the time per batch is the module's plus the dataplane's own
// hop to the sink and back to the task.
//
//   BM_Process/impl/key/result/mix
//     impl    0 = ExactMatch, 1 = WildcardMatch (all-ones masks, one tuple)
//     key     0 = two packet fields (4 B at 26, 2 B at 34): extract, then
//                 look up; 1 = one 4-byte field at 26 (ExactMatch's fused
//                 kernel, D-121)
//     result  0 = gate only; 1 = two metadata fields (4 B, 2 B) the sink
//                 reads; 2 = one metadata field (4 B) and a 2-byte packet
//                 field at offset 40 (the packet is made writable)
//     mix     0 = every packet hits (gate 1); 1 = every packet misses (the
//                 default gate, 2, and the default result when result != 0)
//     1,024 rules; 256 batches of distinct packets, cycled. Items = packets.
//   BM_RuleUpdate/impl/result  one rule (with a 6-byte result when result
//     != 0) upserted and erased again, two transactions through the
//     module's resource, keys and values decoded beforehand. Items = changes.

#include <benchmark/benchmark.h>

#include <google/protobuf/any.pb.h>

#include <any>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "metadata.h"
#include "module.h"
#include "module_graph.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "task.h"
#include "worker.h"

namespace {

constexpr size_t kBatch = 32;
constexpr size_t kBatches = 256;
constexpr uint32_t kRules = 1024;
constexpr uint32_t kMissBit = 0x80000000U;
constexpr gate_idx_t kHitGate = 1;
constexpr gate_idx_t kDefaultGate = 2;

// What the feed sends and what the sink declares, for the one benchmark in
// progress.
std::vector<bess::PacketBatch> *feed_batches = nullptr;
size_t feed_next = 0;
std::vector<std::pair<std::string, size_t>> sink_reads;

}  // namespace

// Emits the next prepared batch per task run.
class MatchBenchFeed final : public Module {
 public:
  static const gate_idx_t kNumIGates = 0;
  static const gate_idx_t kNumOGates = 1;
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kSingleWorker,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  MatchBenchFeed() { is_task_ = true; }

  CommandResponse Init(const bess::pb::EmptyArg &) { return CommandSuccess(); }

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch, void *) override {
    batch->clear();
    batch->add(&(*feed_batches)[feed_next]);
    feed_next = (feed_next + 1) % feed_batches->size();
    RunNextModule(ctx, batch);
    return {.block = false, .packets = kBatch, .bits = 0};
  }
};

// Reads the result attributes (so they are placed, D-181) and keeps the
// packets: they belong to the benchmark and are sent again.
class MatchBenchSink final : public Module {
 public:
  static const gate_idx_t kNumIGates = MAX_GATES;
  static const gate_idx_t kNumOGates = 0;
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kSingleWorker,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  CommandResponse Init(const bess::pb::EmptyArg &) {
    for (const auto &[name, size] : sink_reads) {
      if (AddMetadataAttr(name, size, bess::metadata::Attribute::AccessMode::kRead) < 0) {
        return CommandFailure(EINVAL, "attribute %s", name.c_str());
      }
    }
    return CommandSuccess();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    received[ctx->current_igate] += static_cast<uint64_t>(batch->cnt());
    batch->clear();
  }

  std::array<uint64_t, 4> received{};
};

ADD_MODULE(MatchBenchFeed, "match_bench_feed", "benchmark: emits prepared batches")
ADD_MODULE(MatchBenchSink, "match_bench_sink", "benchmark: keeps what it receives")

namespace {

using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;

Module *Create(const char *cls, const std::string &name, const google::protobuf::Message &arg) {
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) std::abort();
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(ModuleBuilder::all_module_builders().at(cls), name,
                                        packed, &perr);
  if (m == nullptr || perr.code() != 0) std::abort();
  return m;
}

const bess::framework::ResourceCodec &CodecOf(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  const bess::framework::ResourceCodec *codec =
      r == nullptr ? nullptr : bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  if (codec == nullptr) std::abort();
  return *codec;
}

Op Upsert(const std::string &resource, const google::protobuf::Message &key,
          const google::protobuf::Message &value) {
  const auto &codec = CodecOf(resource);
  google::protobuf::Any pk, pv;
  if (!pk.PackFrom(key) || !pv.PackFrom(value)) std::abort();
  auto k = codec.Key(pk.type_url(), pk.value());
  auto v = codec.Value(pv.type_url(), pv.value());
  if (!k || !v) std::abort();
  return Op::Upsert(resource, std::move(*k), std::move(*v));
}

bool Apply(const std::vector<Op> &ops) {
  TransactionEngine &engine = bess::runtime::runtime().transactions();
  TransactionEngine::Outcome outcome;
  while ((outcome = engine.Apply(ops).outcome) == TransactionEngine::Outcome::kBusy) {
  }
  return outcome == TransactionEngine::Outcome::kApplied;
}

std::string Bytes(const void *p, size_t n) {
  return std::string(static_cast<const char *>(p), n);
}

// Rule r's 6-byte result record: r big-endian, then r's low 16 bits.
std::string ResultFor(uint32_t r) {
  const std::array<uint8_t, 6> bytes = {
      static_cast<uint8_t>(r >> 24U), static_cast<uint8_t>(r >> 16U),
      static_cast<uint8_t>(r >> 8U),  static_cast<uint8_t>(r),
      static_cast<uint8_t>(r >> 8U),  static_cast<uint8_t>(r)};
  return Bytes(bytes.data(), bytes.size());
}

struct Config {
  int impl = 0;    // 0 ExactMatch, 1 WildcardMatch
  int key = 0;     // 0 two fields, 1 one 4-byte field
  int result = 0;  // 0 none, 1 metadata, 2 metadata + packet
};

template <typename Arg>
void AddFields(const Config &c, Arg *arg) {
  auto *f = arg->add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  if (c.key == 0) {
    f = arg->add_fields();
    f->set_offset(34);
    f->set_num_bytes(2);
  }
  if (c.result == 0) {
    return;
  }
  auto *r = arg->add_result_fields();
  r->set_attr_name("match_bench_a");
  r->set_num_bytes(4);
  r->set_value_offset(0);
  r = arg->add_result_fields();
  if (c.result == 1) {
    r->set_attr_name("match_bench_b");
  } else {
    r->set_offset(40);
  }
  r->set_num_bytes(2);
  r->set_value_offset(4);
  arg->set_default_result(ResultFor(0xffffffffU));
}

// The module under test, its rules loaded, and the rule `extra` as an add
// and an erase.
Module *CreateModule(const Config &c, std::vector<Op> *add, std::vector<Op> *erase) {
  const std::string resource = "m/rules";
  std::vector<Op> load;
  const auto key_of = [&](uint32_t r, auto *values) {
    values->Add()->set_value_bin(Bytes(&r, 4));
    if (c.key == 0) {
      const auto low = static_cast<uint16_t>(r);
      values->Add()->set_value_bin(Bytes(&low, 2));
    }
  };
  if (c.impl == 0) {
    bess::pb::ExactMatchArg arg;
    AddFields(c, &arg);
    arg.set_default_gate(kDefaultGate);
    Module *m = Create("ExactMatch", "m", arg);
    const auto op = [&](uint32_t r) {
      bess::pb::ExactMatchRuleKey key;
      key_of(r, key.mutable_fields());
      bess::pb::ExactMatchRuleValue value;
      value.set_gate(kHitGate);
      if (c.result != 0) value.set_result(ResultFor(r));
      return Upsert(resource, key, value);
    };
    for (uint32_t r = 0; r < kRules; r++) load.push_back(op(r));
    add->push_back(op(kRules));
    erase->push_back(Op::Erase(resource, add->back().key));
    if (!Apply(load)) std::abort();
    return m;
  }
  bess::pb::WildcardMatchArg arg;
  AddFields(c, &arg);
  arg.set_default_gate(kDefaultGate);
  Module *m = Create("WildcardMatch", "m", arg);
  const auto op = [&](uint32_t r) {
    bess::pb::WildcardMatchRuleKey key;
    key_of(r, key.mutable_values());
    key_of(0xffffffffU, key.mutable_masks());
    bess::pb::WildcardMatchRuleValue value;
    value.set_priority(1);
    value.set_gate(kHitGate);
    if (c.result != 0) value.set_result(ResultFor(r));
    return Upsert(resource, key, value);
  };
  for (uint32_t r = 0; r < kRules; r++) load.push_back(op(r));
  add->push_back(op(kRules));
  erase->push_back(Op::Erase(resource, add->back().key));
  if (!Apply(load)) std::abort();
  return m;
}

struct Packets {
  explicit Packets(size_t n) : pool(n + 64) {
    for (size_t i = 0; i < n; i++) {
      bess::PacketHandle pkt = pool.Alloc(60);
      if (pkt == nullptr) std::abort();
      auto *p = bess::PacketRef(pkt).head_data<uint8_t *>();
      std::memset(p, 0, 60);
      p[12] = 0x08;
      p[14] = 0x45;
      p[23] = 17;
      handles.push_back(pkt);
    }
  }
  ~Packets() {
    for (bess::PacketHandle pkt : handles) bess::PacketFree(pkt);
  }
  Packets(const Packets &) = delete;
  Packets &operator=(const Packets &) = delete;
  bess::PlainPacketPool pool;
  std::vector<bess::PacketHandle> handles;
};

void BM_Process(benchmark::State &state) {
  const Config c{.impl = static_cast<int>(state.range(0)),
                 .key = static_cast<int>(state.range(1)),
                 .result = static_cast<int>(state.range(2))};
  const bool miss = state.range(3) == 1;
  ModuleGraph::DestroyAllModules();
  current_worker.SetNonWorker();
  sink_reads.clear();
  if (c.result != 0) {
    sink_reads.emplace_back("match_bench_a", 4);
  }
  if (c.result == 1) {
    sink_reads.emplace_back("match_bench_b", 2);
  }
  std::vector<Op> add, erase;
  Module *m = CreateModule(c, &add, &erase);
  Module *feed = Create("MatchBenchFeed", "feed", bess::pb::EmptyArg());
  auto *sink = static_cast<MatchBenchSink *>(Create("MatchBenchSink", "sink", bess::pb::EmptyArg()));
  if (ModuleGraph::ConnectModules(feed, 0, m, 0, true) != 0 ||
      ModuleGraph::ConnectModules(m, kHitGate, sink, kHitGate, true) != 0 ||
      ModuleGraph::ConnectModules(m, kDefaultGate, sink, kDefaultGate, true) != 0) {
    std::abort();
  }
  Task task(feed, nullptr);
  ModuleGraph::UpdateTaskGraph();
  if (bess::metadata::default_pipeline.ComputeMetadataOffsets() != 0) std::abort();
  uint32_t gates = 0;
  for (const auto &[_, module] : ModuleGraph::GetAllModules()) {
    gates += module->igates().size() + module->ogates().size();
    module->OnEvent(bess::Event::PreResume);
  }
  task.UpdatePerGateBatch(gates + 1);

  Packets packets(kBatch * kBatches);
  for (size_t i = 0; i < packets.handles.size(); i++) {
    const auto r = static_cast<uint32_t>((i * 7919) % kRules) | (miss ? kMissBit : 0);
    auto *p = bess::PacketRef(packets.handles[i]).head_data<uint8_t *>();
    std::memcpy(p + 26, &r, 4);
    const auto low = static_cast<uint16_t>(r);
    std::memcpy(p + 34, &low, 2);
  }
  std::vector<bess::PacketBatch> batches(kBatches);
  for (size_t b = 0; b < kBatches; b++) {
    batches[b].clear();
    for (size_t i = 0; i < kBatch; i++) {
      batches[b].add(bess::PacketRef(packets.handles[(b * kBatch) + i]));
    }
  }
  feed_batches = &batches;
  feed_next = 0;

  Context ctx = {};
  ctx.wid = bess::dataplane::WorkerId{0};
  ctx.task = &task;
  for (auto _ : state) {
    task(&ctx);
  }
  const gate_idx_t expect = miss ? kDefaultGate : kHitGate;
  if (sink->received[expect] != state.iterations() * kBatch) {
    state.SkipWithError("packets reached the wrong gate");
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
  feed_batches = nullptr;
  ModuleGraph::DestroyAllModules();
}

void BM_RuleUpdate(benchmark::State &state) {
  const Config c{.impl = static_cast<int>(state.range(0)),
                 .key = 0,
                 .result = static_cast<int>(state.range(1))};
  ModuleGraph::DestroyAllModules();
  sink_reads.clear();
  std::vector<Op> add, erase;
  CreateModule(c, &add, &erase);
  for (auto _ : state) {
    if (!Apply(add) || !Apply(erase)) {
      state.SkipWithError("transaction refused");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations());
  ModuleGraph::DestroyAllModules();
}

BENCHMARK(BM_Process)->ArgsProduct({{0, 1}, {0, 1}, {0, 1, 2}, {0, 1}});
BENCHMARK(BM_RuleUpdate)->ArgsProduct({{0, 1}, {0, 1}});

}  // namespace
