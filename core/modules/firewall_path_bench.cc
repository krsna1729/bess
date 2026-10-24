// SPDX-License-Identifier: BSD-3-Clause

// Firewall's packet path through ProcessBatch, as a worker runs it: a feed
// task emits one 32-packet batch into the module per iteration; the module
// parses each packet, looks the batch up in its published table, writes the
// matched rule's action id when something reads it, and emits the batch to a
// sink that keeps the packets for a later iteration. The time per batch is
// the module's plus the dataplane's own hop to the sink and back to the task.
//
//   BM_Process/rules/mix/action
//     rules   rules of one shape (destination 10.x.y.z/32, UDP), each passing
//             with its own action id, then a catch-all that passes with none
//     mix     0 = every packet matches one of the /32 rules; 1 = every packet
//             misses them and falls to the catch-all
//     action  0 = nothing reads "action_id" (no write); 1 = the sink reads it
//     256 batches of distinct IPv4/UDP frames, cycled. Items = packets.

#include <benchmark/benchmark.h>

#include <google/protobuf/any.pb.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "metadata.h"
#include "module.h"
#include "module_graph.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "task.h"
#include "worker.h"

namespace {

constexpr size_t kBatch = 32;
constexpr size_t kBatches = 256;
constexpr size_t kFrameLen = 60;

std::vector<bess::PacketBatch> *feed_batches = nullptr;
size_t feed_next = 0;
bool sink_reads_action = false;

}  // namespace

// Emits the next prepared batch per task run.
class FirewallBenchFeed final : public Module {
 public:
  static const gate_idx_t kNumIGates = 0;
  static const gate_idx_t kNumOGates = 1;
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kSingleWorker,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  FirewallBenchFeed() { is_task_ = true; }

  CommandResponse Init(const bess::pb::EmptyArg &) { return CommandSuccess(); }

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch, void *) override {
    batch->clear();
    batch->add(&(*feed_batches)[feed_next]);
    feed_next = (feed_next + 1) % feed_batches->size();
    RunNextModule(ctx, batch);
    return {.block = false, .packets = kBatch, .bits = 0};
  }
};

// Reads "action_id" when asked (so the firewall writes it, D-141) and keeps
// the packets: they belong to the benchmark and are sent again.
class FirewallBenchSink final : public Module {
 public:
  static const gate_idx_t kNumIGates = 1;
  static const gate_idx_t kNumOGates = 0;
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kSingleWorker,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  CommandResponse Init(const bess::pb::EmptyArg &) {
    if (sink_reads_action &&
        AddMetadataAttr("action_id", 4, bess::metadata::Attribute::AccessMode::kRead) < 0) {
      return CommandFailure(EINVAL, "attribute action_id");
    }
    return CommandSuccess();
  }

  void ProcessBatch(Context *, bess::PacketBatch *batch) override {
    received += static_cast<uint64_t>(batch->cnt());
    batch->clear();
  }

  uint64_t received = 0;
};

ADD_MODULE(FirewallBenchFeed, "firewall_bench_feed", "benchmark: emits prepared batches")
ADD_MODULE(FirewallBenchSink, "firewall_bench_sink", "benchmark: keeps what it receives")

namespace {

Module *Create(const char *cls, const std::string &name, const google::protobuf::Message &arg) {
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) std::abort();
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(ModuleBuilder::all_module_builders().at(cls), name,
                                        packed, &perr);
  if (m == nullptr || perr.code() != 0) std::abort();
  return m;
}

// Rule r's destination: 10.0.0.0 + r.
std::array<uint8_t, 4> AddressOf(uint32_t r, bool miss) {
  return {static_cast<uint8_t>(miss ? 11 : 10), static_cast<uint8_t>(r >> 16U),
          static_cast<uint8_t>(r >> 8U), static_cast<uint8_t>(r)};
}

std::string Dotted(const std::array<uint8_t, 4> &a) {
  return std::to_string(a[0]) + "." + std::to_string(a[1]) + "." + std::to_string(a[2]) + "." +
         std::to_string(a[3]);
}

struct Packets {
  explicit Packets(size_t n) : pool(n + 64) {
    for (size_t i = 0; i < n; i++) {
      bess::PacketHandle pkt = pool.Alloc(kFrameLen);
      if (pkt == nullptr) std::abort();
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

// Ethernet, IPv4 (192.168.0.1 -> `dst`), UDP 1000 -> 53: 20 + 8 bytes of IP.
void WriteFrame(uint8_t *p, const std::array<uint8_t, 4> &dst) {
  std::memset(p, 0, kFrameLen);
  p[12] = 0x08;
  p[14] = 0x45;
  p[17] = 28;
  p[22] = 64;
  p[23] = 17;
  p[26] = 192;
  p[27] = 168;
  p[29] = 1;
  std::memcpy(p + 30, dst.data(), 4);
  p[34] = 0x03;
  p[35] = 0xe8;
  p[37] = 53;
  p[39] = 8;
}

void BM_Process(benchmark::State &state) {
  const auto rules = static_cast<uint32_t>(state.range(0));
  const bool miss = state.range(1) == 1;
  sink_reads_action = state.range(2) == 1;
  ModuleGraph::DestroyAllModules();
  current_worker.SetNonWorker();

  bess::pb::FirewallArg arg;
  for (uint32_t r = 0; r < rules; r++) {
    auto *rule = arg.add_rules();
    rule->set_dst_ip(Dotted(AddressOf(r, false)) + "/32");
    rule->set_proto(17);
    rule->set_action_id(r + 1);
  }
  arg.add_rules();  // the catch-all: both families, pass, no action
  Module *fw = Create("Firewall", "fw", arg);
  Module *feed = Create("FirewallBenchFeed", "feed", bess::pb::EmptyArg());
  auto *sink =
      static_cast<FirewallBenchSink *>(Create("FirewallBenchSink", "sink", bess::pb::EmptyArg()));
  if (ModuleGraph::ConnectModules(feed, 0, fw, 0, true) != 0 ||
      ModuleGraph::ConnectModules(fw, 0, sink, 0, true) != 0) {
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
    const auto r = static_cast<uint32_t>((i * 7919) % rules);
    WriteFrame(bess::PacketRef(packets.handles[i]).head_data<uint8_t *>(), AddressOf(r, miss));
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
  if (sink->received != state.iterations() * kBatch) {
    state.SkipWithError("packets were dropped");
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
  feed_batches = nullptr;
  ModuleGraph::DestroyAllModules();
}

BENCHMARK(BM_Process)->ArgsProduct({{16, 1024}, {0, 1}, {0, 1}});

}  // namespace
