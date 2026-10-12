// SPDX-License-Identifier: BSD-3-Clause

#include "testing/module_harness.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "runtime/opts.h"
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
  pb_error_t error;
  Module *m = CreateRaw(mclass, arg, "refused" + std::to_string(next_name_++), &error);
  if (m != nullptr) {
    ModuleGraph::DestroyModule(m);
    return "";
  }
  return error.errmsg().empty() ? "refused (code " + std::to_string(error.code()) + ")"
                                : error.errmsg();
}

std::map<gate_idx_t, std::vector<Bytes>> ModuleHarness::Run(Module *m, gate_idx_t igate,
                                                            const std::vector<Bytes> &in,
                                                            std::vector<gate_idx_t> ogates) {
  std::map<gate_idx_t, std::vector<Bytes>> out;
  if (m == nullptr || injector_ == nullptr || capture_ == nullptr) {
    ADD_FAILURE() << "no module to run";
    return out;
  }
  if (ogates.empty()) {
    for (gate_idx_t g = 0; g < 16 && g < m->module_builder()->NumOGates(); g++) {
      ogates.push_back(g);
    }
  }
  ModuleGraph::DisconnectModule(injector_, 0);
  EXPECT_EQ(0, ModuleGraph::ConnectModules(injector_, 0, m, igate, true));
  for (gate_idx_t g : ogates) {
    ModuleGraph::DisconnectModule(m, g);
    EXPECT_EQ(0, ModuleGraph::ConnectModules(m, g, capture_, g, true)) << "output gate " << g;
  }
  // Global gate indexes, as a resume assigns them; the task's per-gate
  // batches are indexed by them.
  ModuleGraph::UpdateTaskGraph();
  uint32_t gates = 0;
  for (const auto &[_, module] : ModuleGraph::GetAllModules()) {
    gates += module->ogates().size();
  }

  pending = in;
  captured = &out;
  Task task(injector_, nullptr);
  task.UpdatePerGateBatch(gates + 1);
  Context ctx = {};
  ctx.wid = 0;
  ctx.task = &task;
  ctx.current_tsc = rdtsc();
  ctx.current_ns = tsc_to_ns(ctx.current_tsc);
  task(&ctx);
  captured = nullptr;
  return out;
}

CommandResponse ModuleHarness::Command(Module *m, const std::string &command,
                                       const google::protobuf::Message &arg) {
  return m->RunCommand(command, Pack(arg));
}

google::protobuf::Any ModuleHarness::MustCommand(Module *m, const std::string &command,
                                                 const google::protobuf::Message &arg) {
  CommandResponse r = Command(m, command, arg);
  EXPECT_EQ(0, r.error().code()) << command << " " << arg.ShortDebugString() << ": "
                                 << r.error().errmsg();
  return r.data();
}

}  // namespace bess::testing
