// SPDX-License-Identifier: BSD-3-Clause

#include "module.h"
#include "framework/plugin.h"

#include <atomic>
#include <cstdint>

class StandalonePass final : public Module {
 public:
  static const Commands cmds;

  StandalonePass() : Module(), pkts_passed_(0) {}

  CommandResponse Init(const bess::pb::EmptyArg &);
  void DeInit() override;
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandGetStats(const bess::pb::EmptyArg &);

 private:
  std::atomic<uint64_t> pkts_passed_;
};

const Commands StandalonePass::cmds = {
    {"get_stats", "EmptyArg",
     MODULE_CMD_FUNC(&StandalonePass::CommandGetStats), Command::THREAD_SAFE},
};

CommandResponse StandalonePass::Init(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

void StandalonePass::DeInit() {}

void StandalonePass::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  pkts_passed_.fetch_add(batch->cnt(), std::memory_order_relaxed);
  RunNextModule(ctx, batch);
}

CommandResponse StandalonePass::CommandGetStats(const bess::pb::EmptyArg &) {
  bess::pb::EmptyArg resp;
  return CommandSuccess(resp);
}

BESS_PLUGIN("standalone_pass", "1.0.0");

ADD_MODULE(StandalonePass, "standalone_pass",
           "Standalone out-of-tree plugin passing packets through gate 0")
