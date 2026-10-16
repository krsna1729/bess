// SPDX-License-Identifier: BSD-3-Clause

#include "testing/module_harness.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "metadata.h"
#include "module.h"
#include "module_graph.h"
#include "runtime/opts.h"
#include "runtime/runtime_state.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "port.h"
#include "task.h"
#include "utils/time.h"
#include "worker.h"

namespace bess::testing {
namespace {

// What the injector sends next and what the capture saw, for the one Run()
// in progress (tests are single-threaded).
std::vector<Bytes> pending;
std::map<gate_idx_t, std::vector<Bytes>> *captured = nullptr;

}  // namespace

// Feeds `pending` into its output gate once per task run.
class HarnessInject final : public Module {
 public:
  static const gate_idx_t kNumIGates = 0;
  static const gate_idx_t kNumOGates = 1;

  HarnessInject() { is_task_ = true; }

  CommandResponse Init(const bess::pb::EmptyArg &) {
    // A task module gets its task through the traffic-class tree; the
    // harness runs its own Task, so none is registered.
    return CommandSuccess();
  }

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch, void *) override {
    batch->clear();
    for (const Bytes &bytes : pending) {
      bess::PacketHandle h = current_worker.packet_pool()->Alloc(0);
      if (h == nullptr) {
        ADD_FAILURE() << "packet pool exhausted";
        break;
      }
      bess::PacketRef ref(h);
      void *data = ref.append(static_cast<uint16_t>(bytes.size()));
      if (data == nullptr) {
        ADD_FAILURE() << "a test packet of " << bytes.size() << " bytes does not fit an mbuf";
        bess::PacketFree(h);
        break;
      }
      std::memcpy(data, bytes.data(), bytes.size());
      batch->add(ref);
    }
    pending.clear();
    const uint32_t cnt = static_cast<uint32_t>(batch->cnt());
    RunNextModule(ctx, batch);
    return {.block = false, .packets = cnt, .bits = 0};
  }
};

// Records each packet under the input gate it arrived on (the output gate of
// the module under test it is connected to), then frees it.
class HarnessCapture final : public Module {
 public:
  static const gate_idx_t kNumIGates = MAX_GATES;
  static const gate_idx_t kNumOGates = 0;

  CommandResponse Init(const bess::pb::EmptyArg &) { return CommandSuccess(); }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    for (bess::PacketRef ref : batch->packets()) {
      // Linearize: a module may have built a chained packet.
      Bytes bytes;
      bytes.reserve(ref.total_len());
      for (bess::PacketHandle h = ref.handle(); h != nullptr; h = h->next) {
        const bess::PacketRef seg(h);
        const auto *p = seg.head_data<const uint8_t *>();
        bytes.insert(bytes.end(), p, p + seg.head_len());
      }
      if (captured != nullptr) {
        (*captured)[ctx->current_igate].push_back(std::move(bytes));
      }
    }
    bess::PacketFreeBatch(batch);
  }
};

ADD_MODULE(HarnessInject, "harness_inject", "test harness: injects queued packets")
ADD_MODULE(HarnessCapture, "harness_capture", "test harness: records packets per input gate")

void InitModuleTests() {
  static const bool done = [] {
    FLAGS_m = 0;  // malloc-backed pools: no hugepages, no root
    bess::PacketPool::CreateDefaultPools(32767);
    PortBuilder::InitDrivers();
    return true;
  }();
  (void)done;
  current_worker.SetNonWorker();
}

namespace {

google::protobuf::Any Pack(const google::protobuf::Message &arg) {
  google::protobuf::Any any;
  EXPECT_TRUE(any.PackFrom(arg));
  return any;
}

Module *CreateRaw(const std::string &mclass, const google::protobuf::Message &arg,
                  const std::string &name, pb_error_t *error) {
  const auto &builders = ModuleBuilder::all_module_builders();
  const auto it = builders.find(mclass);
  if (it == builders.end()) {
    error->set_code(ENOENT);
    error->set_errmsg("no module class " + mclass + " in this test binary");
    return nullptr;
  }
  return ModuleGraph::CreateModule(it->second, name, Pack(arg), error);
}

}  // namespace

ModuleHarness::ModuleHarness() {
  InitModuleTests();
  pb_error_t error;
  injector_ = CreateRaw("HarnessInject", bess::pb::EmptyArg(), "harness_inject", &error);
  capture_ = CreateRaw("HarnessCapture", bess::pb::EmptyArg(), "harness_capture", &error);
  EXPECT_NE(nullptr, injector_) << error.errmsg();
  EXPECT_NE(nullptr, capture_) << error.errmsg();
}

ModuleHarness::~ModuleHarness() {
  pending.clear();
  captured = nullptr;
  ModuleGraph::DestroyAllModules();
}

Module *ModuleHarness::Create(const std::string &mclass, const google::protobuf::Message &arg,
                              const std::string &name) {
  pb_error_t error;
  const std::string n = name.empty() ? "m" + std::to_string(next_name_++) : name;
  Module *m = CreateRaw(mclass, arg, n, &error);
  if (m == nullptr) {
    ADD_FAILURE() << mclass << " refused " << arg.ShortDebugString() << ": " << error.errmsg();
  }
  return m;
}

std::string ModuleHarness::CreateError(const std::string &mclass,
                                       const google::protobuf::Message &arg) {
  const pb_error_t error = CreateFailure(mclass, arg);
  if (error.code() == 0) {
    return "";
  }
  return error.errmsg().empty() ? "refused (code " + std::to_string(error.code()) + ")"
                                : error.errmsg();
}

pb_error_t ModuleHarness::CreateFailure(const std::string &mclass,
                                        const google::protobuf::Message &arg) {
  pb_error_t error;
  Module *m = CreateRaw(mclass, arg, "refused" + std::to_string(next_name_++), &error);
  if (m != nullptr) {
    ModuleGraph::DestroyModule(m);
    return pb_error_t();
  }
  return error;
}

void ModuleHarness::WireOutputs(Module *m, std::vector<gate_idx_t> ogates, Task &task) {
  if (ogates.empty()) {
    for (gate_idx_t g = 0; g < 16 && g < m->module_builder()->NumOGates(); g++) {
      ogates.push_back(g);
    }
  }
  // Only the gates asked for reach the capture, whatever an earlier Run
  // connected.
  for (size_t g = 0; g < m->ogates().size(); g++) {
    if (m->ogates()[g] != nullptr) {
      // ogates() never exceeds MAX_GATES, which fits gate_idx_t.
      ModuleGraph::DisconnectModule(m, static_cast<gate_idx_t>(g));
    }
  }
  for (gate_idx_t g : ogates) {
    EXPECT_EQ(0, ModuleGraph::ConnectModules(m, g, capture_, g, true)) << "output gate " << g;
  }
  // Global gate indexes, as a resume assigns them (every connected input
  // gate, then every output gate); the task's per-gate batches are indexed
  // by them.
  ModuleGraph::UpdateTaskGraph();
  // Metadata offsets, then each module's PreResume (modules that bake
  // attribute offsets into their tables refresh them there), as a resume
  // does.
  EXPECT_EQ(0, bess::metadata::default_pipeline.ComputeMetadataOffsets());
  uint32_t gates = 0;
  for (const auto &[_, module] : ModuleGraph::GetAllModules()) {
    gates += module->igates().size() + module->ogates().size();
    module->OnEvent(bess::Event::PreResume);
  }
  task.UpdatePerGateBatch(gates + 1);
}

void ModuleHarness::RunOnce(Task &task) {
  Context ctx = {};
  ctx.wid = bess::dataplane::WorkerId(0);
  ctx.task = &task;
  ctx.current_tsc = rdtsc();
  ctx.current_ns = tsc_to_ns(ctx.current_tsc);
  task(&ctx);
}

std::map<gate_idx_t, std::vector<Bytes>> ModuleHarness::Run(Module *m, gate_idx_t igate,
                                                            const std::vector<Bytes> &in,
                                                            std::vector<gate_idx_t> ogates) {
  return RunPipeline(m, igate, in, m, std::move(ogates));
}

std::map<gate_idx_t, std::vector<Bytes>> ModuleHarness::RunPipeline(
    Module *first, gate_idx_t igate, const std::vector<Bytes> &in, Module *last,
    std::vector<gate_idx_t> ogates) {
  std::map<gate_idx_t, std::vector<Bytes>> out;
  if (first == nullptr || last == nullptr || injector_ == nullptr || capture_ == nullptr) {
    ADD_FAILURE() << "no module to run";
    return out;
  }
  ModuleGraph::DisconnectModule(injector_, 0);
  EXPECT_EQ(0, ModuleGraph::ConnectModules(injector_, 0, first, igate, true));
  Task task(injector_, nullptr);
  WireOutputs(last, std::move(ogates), task);
  captured = &out;
  // One task run per burst: a batch holds kMaxBurst packets.
  for (size_t at = 0; at < in.size(); at += bess::PacketBatch::kMaxBurst) {
    const size_t n = std::min<size_t>(bess::PacketBatch::kMaxBurst, in.size() - at);
    pending.assign(in.begin() + static_cast<ptrdiff_t>(at),
                   in.begin() + static_cast<ptrdiff_t>(at + n));
    RunOnce(task);
  }
  captured = nullptr;
  return out;
}

std::map<gate_idx_t, std::vector<Bytes>> ModuleHarness::RunTask(Module *m,
                                                                std::vector<gate_idx_t> ogates) {
  std::map<gate_idx_t, std::vector<Bytes>> out;
  if (m == nullptr || capture_ == nullptr) {
    ADD_FAILURE() << "no module to run";
    return out;
  }
  if (m->tasks().empty()) {
    ADD_FAILURE() << m->name() << " registered no task";
    return out;
  }
  Task task(m, m->tasks()[0]->arg());
  WireOutputs(m, std::move(ogates), task);
  captured = &out;
  RunOnce(task);
  captured = nullptr;
  return out;
}

CommandResponse ModuleHarness::Action(Module *m, const std::string &action,
                                      const google::protobuf::Message &arg) {
  return m->module_builder()->RunCommand(m, action, Pack(arg));
}

namespace {

// One operation, decoded as ApplyTransaction decodes it, applied alone.
std::string ApplyWire(const std::string &resource, bool erase, const google::protobuf::Any *key,
                      const google::protobuf::Any *value) {
  dataplane::TransactionEngine &engine = runtime::runtime().transactions();
  const dataplane::Resource *r = engine.FindResource(resource);
  const framework::ResourceCodec *codec =
      r == nullptr ? nullptr : framework::ResourceBindings::ProcessDefault().Find(*r);
  if (codec == nullptr) {
    return "no resource '" + resource + "'";
  }
  if (codec->Busy()) {
    return "busy: a worker runs the module";
  }
  auto k = key == nullptr ? codec->Key("", "") : codec->Key(key->type_url(), key->value());
  if (!k) {
    return k.error();
  }
  dataplane::Op op;
  if (erase) {
    op = dataplane::Op::Erase(resource, std::move(*k));
  } else {
    auto v = codec->Value(value->type_url(), value->value());
    if (!v) {
      return v.error();
    }
    op = dataplane::Op::Upsert(resource, std::move(*k), std::move(*v));
  }
  const dataplane::TransactionEngine::Result result =
      engine.Apply(std::span<const dataplane::Op>(&op, 1));
  if (result.outcome == dataplane::TransactionEngine::Outcome::kApplied) {
    return "";
  }
  return result.ops.empty() || result.ops[0].error.empty() ? "refused" : result.ops[0].error;
}

}  // namespace

std::string ModuleHarness::SetResource(const std::string &resource,
                                       const google::protobuf::Message &value) {
  const google::protobuf::Any v = Pack(value);
  return ApplyWire(resource, false, nullptr, &v);
}

std::string ModuleHarness::UpsertResource(const std::string &resource,
                                          const google::protobuf::Message &key,
                                          const google::protobuf::Message &value) {
  const google::protobuf::Any k = Pack(key);
  const google::protobuf::Any v = Pack(value);
  return ApplyWire(resource, false, &k, &v);
}

std::string ModuleHarness::EraseResource(const std::string &resource,
                                         const google::protobuf::Message &key) {
  const google::protobuf::Any k = Pack(key);
  return ApplyWire(resource, true, &k, nullptr);
}

std::vector<std::pair<google::protobuf::Any, google::protobuf::Any>> ModuleHarness::ReadResource(
    const std::string &resource) {
  std::vector<std::pair<google::protobuf::Any, google::protobuf::Any>> out;
  const dataplane::Resource *r = runtime::runtime().transactions().FindResource(resource);
  const framework::ResourceCodec *codec =
      r == nullptr ? nullptr : framework::ResourceBindings::ProcessDefault().Find(*r);
  EXPECT_NE(nullptr, codec) << "no resource '" << resource << "'";
  if (codec == nullptr) {
    return out;
  }
  codec->Entries([&](const google::protobuf::Message &key, const google::protobuf::Message &value) {
    out.emplace_back(Pack(key), Pack(value));
  });
  return out;
}

namespace {

std::string Deterministic(const google::protobuf::Message &message) {
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream stream(&bytes);
    google::protobuf::io::CodedOutputStream out(&stream);
    out.SetSerializationDeterministic(true);
    EXPECT_TRUE(message.SerializeToCodedStream(&out));
  }
  return bytes;
}

}  // namespace

std::string ModuleHarness::ReadBackDiffers(
    const std::string &resource,
    const std::vector<std::pair<const google::protobuf::Message *,
                                const google::protobuf::Message *>> &entries) {
  const dataplane::Resource *r = runtime::runtime().transactions().FindResource(resource);
  const framework::ResourceCodec *codec =
      r == nullptr ? nullptr : framework::ResourceBindings::ProcessDefault().Find(*r);
  if (codec == nullptr) {
    return "no resource '" + resource + "'";
  }
  auto internal_key = [&](const google::protobuf::Message *key)
      -> std::expected<dataplane::ResourceKey, std::string> {
    if (key == nullptr) {
      return codec->Key("", "");
    }
    const google::protobuf::Any any = Pack(*key);
    return codec->Key(any.type_url(), any.value());
  };
  std::map<dataplane::ResourceKey, std::string> held;
  std::string error;
  codec->Entries([&](const google::protobuf::Message &key, const google::protobuf::Message &value) {
    auto k = internal_key(codec->kind() == framework::ResourceCodec::Kind::kSingleton ? nullptr
                                                                                       : &key);
    if (!k) {
      error = "read back key " + key.ShortDebugString() + " does not decode: " + k.error();
      return;
    }
    held.emplace(std::move(*k), Deterministic(value));
  });
  if (!error.empty()) {
    return error;
  }
  if (held.size() != entries.size()) {
    return "holds " + std::to_string(held.size()) + " entries, desired " +
           std::to_string(entries.size());
  }
  for (const auto &[key, value] : entries) {
    auto k = internal_key(key);
    if (!k) {
      return "desired key does not decode: " + k.error();
    }
    const auto it = held.find(*k);
    if (it == held.end()) {
      return "no entry for " + (key == nullptr ? std::string("the singleton") : key->ShortDebugString());
    }
    if (it->second != Deterministic(*value)) {
      return "entry " + (key == nullptr ? std::string("(singleton)") : key->ShortDebugString()) +
             ": desired " + value->ShortDebugString() + ", reads back differently";
    }
  }
  return "";
}

const framework::ResourceCodec *ModuleHarness::Codec(const std::string &resource) {
  const dataplane::Resource *r = runtime::runtime().transactions().FindResource(resource);
  return r == nullptr ? nullptr : framework::ResourceBindings::ProcessDefault().Find(*r);
}

std::string ModuleHarness::Apply(const std::vector<dataplane::Op> &ops) {
  const dataplane::TransactionEngine::Result result =
      runtime::runtime().transactions().Apply(std::span<const dataplane::Op>(ops));
  if (result.outcome == dataplane::TransactionEngine::Outcome::kApplied) {
    return "";
  }
  return result.ops.empty() || result.ops[0].error.empty() ? "refused" : result.ops[0].error;
}

}  // namespace bess::testing
