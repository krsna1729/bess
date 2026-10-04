// SPDX-License-Identifier: BSD-3-Clause

// R3's graph adapter (roadmap M24): VswitchApp in a module. igate t is tenant
// t; an allowed packet leaves on its decision's gate (0 unless redirected), a
// denied one is dropped. `self_test` drives VswitchApp directly: compiled
// decisions from the layered policy, cache hits, and a group switch in one
// tenant that recompiles that tenant only.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "framework/plugin.h"
#include "module.h"
#include "offload/fake_flow_backend.h"
#include "offload/flow_rule_owner.h"
#include "vswitch/vswitch_app.h"

namespace {

using appliance::VswitchApp;

std::vector<uint8_t> Frame(uint32_t dst, uint16_t dport, uint8_t proto, uint16_t sport = 40000) {
  std::vector<uint8_t> f(proto == 6 ? 54 : 42, 0);
  f[12] = 0x08;
  f[14] = 0x45, f[22] = 64, f[23] = proto;
  const uint16_t len = static_cast<uint16_t>(f.size() - 14);
  f[16] = static_cast<uint8_t>(len >> 8), f[17] = static_cast<uint8_t>(len);
  const uint32_t src = 0xc0a80001;
  for (int i = 0; i < 4; i++) {
    f[26 + i] = static_cast<uint8_t>(src >> (24 - 8 * i));
    f[30 + i] = static_cast<uint8_t>(dst >> (24 - 8 * i));
  }
  f[34] = static_cast<uint8_t>(sport >> 8), f[35] = static_cast<uint8_t>(sport);
  f[36] = static_cast<uint8_t>(dport >> 8), f[37] = static_cast<uint8_t>(dport);
  if (proto == 6) {
    f[46] = 5 << 4, f[47] = 0x10;
  } else {
    f[39] = 8;
  }
  return f;
}

}  // namespace

class VswitchAppliance final : public Module {
 public:
  static const gate_idx_t kNumIGates = VswitchApp::kTenants;
  static const gate_idx_t kNumOGates = 2;
  static const Commands cmds;

  VswitchAppliance() : Module() { max_allowed_workers_ = 1; }  // one worker's caches

  CommandResponse Init(const bess::pb::EmptyArg &) {
    auto made = VswitchApp::Create(init_context().rcu());
    if (!made) {
      return CommandFailure(ENOMEM, "%s", made.error().c_str());
    }
    app_ = std::move(*made);
    return CommandSuccess();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    for (int i = 0; i < batch->cnt(); i++) {
      bess::PacketRef pkt = batch->packet(i);
      const auto d = app_->Process(ctx->current_igate,
                                   std::span<uint8_t>(pkt.head_data<uint8_t *>(), pkt.head_len()));
      if (d.allow) {
        EmitPacket(ctx, pkt, d.gate);
      } else {
        DropPacket(ctx, pkt);
      }
    }
  }

  CommandResponse CommandSelfTest(const bess::pb::EmptyArg &) {
    auto made = VswitchApp::Create(init_context().rcu());
    if (!made) {
      return CommandFailure(ENOMEM, "%s", made.error().c_str());
    }
    if (std::string failed = SelfTest(**made); !failed.empty()) {
      return CommandFailure(EINVAL, "R3 direct path: %s", failed.c_str());
    }
    auto hier = VswitchApp::Create(init_context().rcu());
    if (!hier) {
      return CommandFailure(ENOMEM, "%s", hier.error().c_str());
    }
    if (std::string failed = SelfTestHierarchy(**hier); !failed.empty()) {
      return CommandFailure(EINVAL, "R5 hierarchical mode: %s", failed.c_str());
    }
    return CommandSuccess();
  }

 private:
  static std::string SelfTest(VswitchApp &app) {
    const uint32_t server = 0x0a010101, special = 0x0a090101;
    // Compiled from the layers: web allowed and marked DSCP 10, SSH denied,
    // 10.9/16 redirected to gate 1.
    for (size_t t = 0; t < VswitchApp::kTenants; t++) {
      auto web = Frame(server, 80, 6);
      const auto d = app.Process(t, web);
      if (!d.allow || d.dscp != 10 || (web[15] >> 2) != 10) {
        return "web not allowed and marked";
      }
      auto ssh = Frame(server, 22, 6);
      if (app.Process(t, ssh).allow) {
        return "ssh not denied";
      }
      auto dns = Frame(special, 53, 17);
      const auto r = app.Process(t, dns);
      if (!r.allow || r.gate != 1) {
        return "10.9/16 not redirected";
      }
    }
    // Cached: the same flows again are hits, nothing compiles.
    const uint64_t compiles0 = app.compiles(0), compiles1 = app.compiles(1);
    for (size_t t = 0; t < VswitchApp::kTenants; t++) {
      auto web = Frame(server, 80, 6);
      (void)app.Process(t, web);
    }
    if (app.compiles(0) != compiles0 || app.compiles(1) != compiles1 || app.hits(0) == 0) {
      return "a cached flow compiled again";
    }
    // The compiler switches tenant 0's ACL group 0 to deny web: one O(1)
    // invalidation of tenant 0's scope (nothing is walked: the cache keeps its
    // entries), its web flow recompiles to deny; tenant 1 keeps its hit.
    const size_t cached0 = app.cached(0);
    if (!app.SwitchGroup(0, 0, 0, appliance::Group{{appliance::Rule{0, 0, 80, 80, 6, appliance::Action::kDeny}}})) {
      return "switch";
    }
    if (app.cached(0) != cached0) {
      return "the switch walked the cache";
    }
    auto web0 = Frame(server, 80, 6);
    auto web1 = Frame(server, 80, 6);
    if (app.Process(0, web0).allow) {
      return "tenant 0 still allows web after its switch";
    }
    if (app.compiles(0) != compiles0 + 1) {
      return "tenant 0 did not recompile";
    }
    if (!app.Process(1, web1).allow || app.compiles(1) != compiles1) {
      return "tenant 1 was disturbed by tenant 0's switch";
    }
    return "";
  }

  // R5: cold flows evaluated per packet and counted; the third packet makes a
  // flow hot (the application's rule): its decision is cached and, through the
  // application's promotion hook, installed as a hardware flow rule (the
  // offload owner over a fake device here).
  static std::string SelfTestHierarchy(VswitchApp &app) {
    bess::offload::FakeFlowBackend device;
    device.set_auto_complete(true);
    bess::offload::FlowCapabilities caps;
    caps.supported = true;
    device.SetCapabilities(0, caps);
    bess::offload::FlowRuleOwner<bess::offload::FakeFlowBackend> owner(
        device, bess::offload::FlowRuleOwner<bess::offload::FakeFlowBackend>::Config{});
    size_t promoted = 0;
    if (!app.EnableHierarchy(3, [&](const appliance::VswitchKey &k, appliance::CompiledDecision d) {
          promoted++;
          if (d.allow) {
            (void)owner.Install(0, uint64_t{k.dst} << 16 | k.dport, k.src);
          }
        })) {
      return "enable";
    }
    for (int i = 1; i <= 2; i++) {
      auto web = Frame(0x0a010101, 80, 6);
      if (!app.Process(0, web).allow) {
        return "a cold flow was not served by the default path";
      }
    }
    if (app.cached(0) != 0 || app.cold_flows() != 1 || promoted != 0) {
      return "a cold flow was cached or promoted";
    }
    auto third = Frame(0x0a010101, 80, 6);
    (void)app.Process(0, third);
    owner.Poll([](auto &&...) {});
    if (app.cached(0) != 1 || app.cold_flows() != 0 || promoted != 1 || device.installed(0) != 1) {
      return "the hot flow was not installed in the cache and the device (cached " +
             std::to_string(app.cached(0)) + ", cold " + std::to_string(app.cold_flows()) +
             ", promoted " + std::to_string(promoted) + ", device " +
             std::to_string(device.installed(0)) + ")";
    }
    const uint64_t compiles = app.compiles(0);
    auto fourth = Frame(0x0a010101, 80, 6);
    if (!app.Process(0, fourth).allow || app.compiles(0) != compiles || app.hits(0) != 1) {
      return "the hot flow did not take the fast path";
    }
    return "";
  }

  std::unique_ptr<VswitchApp> app_;
};

const Commands VswitchAppliance::cmds = {
    {"self_test", "EmptyArg", MODULE_CMD_FUNC(&VswitchAppliance::CommandSelfTest),
     Command::THREAD_UNSAFE},
};

BESS_PLUGIN_REQUIRES("vswitch_appliance", "1.0.0", BESS_CAP_INIT_CONTEXT);

ADD_MODULE(VswitchAppliance, "vswitch_appliance",
           "Reference appliance R3: a policy vSwitch (layers, groups, compiled decisions)")
