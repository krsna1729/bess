// SPDX-License-Identifier: BSD-3-Clause

// Conformance plugin (M23, persona D: an appliance). The application owns an
// object graph built from public libraries -- a range classifier that yields
// an `ActionId`, and an `ObjectTable` from action to what to do -- and shares it
// through the init context's `InstanceRegistry`: the first module instance
// creates it, later ones look it up, and every instance runs the whole decision
// in one step (no chain of graph modules). Gate 1: UDP destination port in
// [1000, 2000]; gate 0: everything else (the default action).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "classifier/range_backend.h"
#include "dataplane/action_id.h"
#include "dataplane/object_table.h"
#include "framework/instance_registry.h"
#include "framework/plugin.h"
#include "module.h"

namespace {

using bess::dataplane::ActionId;

struct Action {
  gate_idx_t gate;
};

// The application's object graph. BESS knows nothing about its meaning.
struct Policy {
  bess::classifier::RangeClassifier<uint32_t> classifier;  // key -> action id
  std::unique_ptr<const bess::dataplane::ObjectTable<ActionId, Action>> actions;
  ActionId fallback;
};

constexpr char kPolicyName[] = "standalone_appliance.policy";
constexpr size_t kUdpDstPortOffset = 14 + 20 + 2;

Policy MakePolicy() {
  Policy policy;
  bess::classifier::RangeRule<uint32_t> rule;
  rule.value.assign(2, std::byte{0});
  rule.mask.assign(2, std::byte{0});
  rule.dst_range = {1000, 2000};
  rule.dst_port_offset = 0;
  rule.priority = 1;
  rule.result = 2;  // ActionId{2}
  policy.classifier = bess::classifier::RangeClassifier<uint32_t>(
      std::vector<bess::classifier::RangeRule<uint32_t>>{rule});
  bess::dataplane::ObjectTableBuilder<ActionId, Action> actions(4);
  actions.Set(ActionId{1}, Action{0});
  actions.Set(ActionId{2}, Action{1});
  policy.actions = std::move(actions).Build();
  policy.fallback = ActionId{1};
  return policy;
}

}  // namespace

class StandaloneAppliance final : public Module {
 public:
  static const gate_idx_t kNumOGates = 2;
  static const Commands cmds;

  CommandResponse Init(const bess::pb::EmptyArg &) {
    auto &instances = init_context().instances();
    auto lease = instances.Lookup<Policy>(kPolicyName);
    if (!lease) {
      lease = instances.Create<Policy>(kPolicyName, MakePolicy());
    }
    if (!lease) {
      return CommandFailure(EINVAL, "policy instance unavailable");
    }
    policy_lease_ = std::move(*lease);
    policy_ = policy_lease_.get();
    return CommandSuccess();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    const int cnt = batch->cnt();
    for (int i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      ActionId action = policy_->fallback;
      if (pkt.head_len() > kUdpDstPortOffset + 1) {
        const auto *bytes = pkt.head_data<const uint8_t *>();
        const std::array<std::byte, 2> key = {std::byte{bytes[kUdpDstPortOffset]},
                                              std::byte{bytes[kUdpDstPortOffset + 1]}};
        uint32_t result = 0;
        if (policy_->classifier.LookupBatch(bess::classifier::ConstBytes(key.data(), 2), 2, 2,
                                            std::span<uint32_t>(&result, 1)) != 0) {
          action = ActionId{result};
        }
      }
      const Action *what = policy_->actions->Lookup(action);
      EmitPacket(ctx, pkt, what != nullptr ? what->gate : 0);
    }
    ProcessOGates(ctx);
  }

 private:
  bess::framework::InstanceLease<Policy> policy_lease_;  // keeps the graph alive
  const Policy *policy_ = nullptr;                       // the cached pointer
};

const Commands StandaloneAppliance::cmds = {};

BESS_PLUGIN_REQUIRES("standalone_appliance", "1.0.0",
                     BESS_CAP_INIT_CONTEXT | BESS_CAP_INSTANCES);

ADD_MODULE(StandaloneAppliance, "standalone_appliance",
           "Conformance plugin: an application-owned object graph shared via the registry")
