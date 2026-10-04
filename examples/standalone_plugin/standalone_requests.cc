// SPDX-License-Identifier: BSD-3-Clause

// Conformance plugin (TP4, D-077): a worker-to-control request round trip.
// ProcessBatch posts a request; the daemon's maintenance loop calls the
// module's handler on the control side, which flips the module to gate 1.
// Packets leave on gate 0 until the handler has run, then on gate 1: packets
// on gate 1 prove the request went worker -> maintenance loop -> handler.

#include <atomic>
#include <cstdint>

#include "framework/module_requests.h"
#include "framework/plugin.h"
#include "module.h"

namespace {

struct Switch {
  uint32_t to_gate;
};

}  // namespace

class StandaloneRequests final : public Module {
 public:
  static const gate_idx_t kNumOGates = 2;
  static const Commands cmds;

  CommandResponse Init(const bess::pb::EmptyArg &) {
    switch_ = bess::framework::RequestEndpoint<Switch>(
        init_context().requests(), [this](const Switch &s) {
          gate_.store(static_cast<gate_idx_t>(s.to_gate), std::memory_order_release);
          switch_.Done();
        });
    return CommandSuccess();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    const gate_idx_t gate = gate_.load(std::memory_order_acquire);
    if (gate == 0) {
      (void)switch_.Post({1});  // one relaxed load once pending
    }
    RunChooseModule(ctx, gate, batch);
  }

 private:
  bess::framework::RequestEndpoint<Switch> switch_;
  std::atomic<gate_idx_t> gate_{0};
};

const Commands StandaloneRequests::cmds = {};

BESS_PLUGIN_REQUIRES("standalone_requests", "1.0.0",
                     BESS_CAP_INIT_CONTEXT | BESS_CAP_REQUESTS);

ADD_MODULE(StandaloneRequests, "standalone_requests",
           "Conformance plugin: a worker-to-control request through the maintenance loop")
