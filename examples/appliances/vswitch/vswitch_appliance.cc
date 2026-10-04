// SPDX-License-Identifier: BSD-3-Clause

// R3's graph adapter (roadmap M24): VswitchApp in a module. igate t is tenant
// t; an allowed packet leaves on its decision's gate (0 unless redirected), a
// denied one is dropped. `self_test` drives VswitchApp directly: compiled
// decisions from the layered policy, cache hits, and a group switch in one
// tenant that recompiles that tenant only; and R5, the hierarchical mode.

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
                                   std::span<uint8_t>(pkt.head_data<uint8_t *>(), pkt.head_len()),
                                   pkt.total_len());
      if (d.allow) {
        EmitPacket(ctx, pkt, d.gate);
      } else {
        DropPacket(ctx, pkt);
      }
    }
  }

  CommandResponse CommandSelfTest(const bess::pb::EmptyArg &) {
    auto r3 = VswitchApp::Create(init_context().rcu());
    auto r5 = VswitchApp::Create(init_context().rcu());
    if (!r3 || !r5) {
      return CommandFailure(ENOMEM, "decision cache");
    }
    const std::string failed3 = SelfTest(**r3);
    const std::string failed5 = failed3.empty() ? SelfTestHierarchy(**r5) : "";
    r3->reset();
    r5->reset();
    // The switches retired policies through the runtime's domain; this
    // plugin's code frees them, and the plugin may be unloaded after the
    // command: reclaim them now.
    init_context().rcu().Drain();
    if (!failed3.empty()) {
      return CommandFailure(EINVAL, "R3 direct path: %s", failed3.c_str());
    }
    if (!failed5.empty()) {
      return CommandFailure(EINVAL, "R5 hierarchical mode: %s", failed5.c_str());
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
  // offload owner over a fake device here). A group switch revokes the
  // tenant's hardware rules with its generation; one-off flows age out of the
  // cold counts rather than filling them.
  static std::string SelfTestHierarchy(VswitchApp &app) {
    using Owner = bess::offload::FlowRuleOwner<bess::offload::FakeFlowBackend>;
    bess::offload::FakeFlowBackend device;
    device.set_auto_complete(true);
    bess::offload::FlowCapabilities caps;
    caps.supported = true;
    device.SetCapabilities(0, caps);
    Owner owner(device, Owner::Config{});
    std::vector<bess::offload::FlowRuleHandle> rules[VswitchApp::kTenants];
    size_t promoted = 0;
    VswitchApp::Hierarchy h;
    h.hot_after = 3;
    h.idle_after = 64;
    h.promote = [&](const appliance::VswitchKey &k, appliance::CompiledDecision d) {
      promoted++;
      if (d.allow) {
        const auto r = owner.Install(0, uint64_t{k.dst} << 16 | k.dport, k.src);
        if (r.status == bess::offload::InstallError::kOk) {
          rules[k.tenant].push_back(r.handle);
        }
      }
    };
    h.revoke = [&](size_t tenant) {
      for (const auto handle : rules[tenant]) {
        (void)owner.Remove(handle);
      }
      rules[tenant].clear();
    };
    if (!app.EnableHierarchy(std::move(h))) {
      return "enable";
    }
    auto poll = [&] { owner.Poll([](auto &&...) {}); };
    const uint32_t server = 0x0a010101;
    for (int i = 1; i <= 2; i++) {
      auto web = Frame(server, 80, 6);
      if (!app.Process(0, web).allow) {
        return "a cold flow was not served by the default path";
      }
    }
    if (app.cached(0) != 0 || app.cold_flows() != 1 || promoted != 0) {
      return "a cold flow was cached or promoted";
    }
    auto third = Frame(server, 80, 6);
    (void)app.Process(0, third);
    poll();
    if (app.cached(0) != 1 || app.cold_flows() != 0 || promoted != 1 || device.installed(0) != 1) {
      return "the hot flow was not installed in the cache and the device";
    }
    const uint64_t compiles = app.compiles(0);
    auto fourth = Frame(server, 80, 6);
    if (!app.Process(0, fourth).allow || app.compiles(0) != compiles || app.hits(0) != 1) {
      return "the hot flow did not take the fast path";
    }
    // Tenant 0 switches web to deny: its hardware rule goes with its
    // generation; the flow is cold again and, hot, is promoted as a deny
    // (which this application keeps in software).
    if (!app.SwitchGroup(0, 0, 0, appliance::Group{{appliance::Rule{0, 0, 80, 80, 6, appliance::Action::kDeny}}})) {
      return "switch";
    }
    poll();
    if (device.installed(0) != 0) {
      return "a revoked decision stayed in hardware";
    }
    for (int i = 0; i < 3; i++) {
      auto web = Frame(server, 80, 6);
      if (app.Process(0, web).allow) {
        return "web allowed after the switch";
      }
    }
    poll();
    if (promoted != 2 || device.installed(0) != 0) {
      return "the denied flow was not promoted once, in software only";
    }
    // Ageing: more one-off flows than the cold table holds, then a new flow
    // still turns hot.
    for (uint32_t i = 0; i < 4200; i++) {
      auto once = Frame(server, 53, 17, static_cast<uint16_t>(1 + i));
      (void)app.Process(0, once);
    }
    for (int i = 0; i < 3; i++) {
      auto web = Frame(server, 53, 17, 50000);
      (void)app.Process(0, web);
    }
    if (promoted != 3) {
      return "one-off flows filled the cold counts: a new flow no longer turns hot";
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
