// SPDX-License-Identifier: BSD-3-Clause

// R3's graph adapter: igate t is tenant t; allowed packets leave on their
// decision's gate, denied packets drop. The policies resource replaces one
// complete tenant scope while its worker runs. self_test also drives the
// private application without a graph, including R5's direct promotion and
// revocation path.

#include <any>
#include <bit>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "framework/plugin.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "module.h"
#include "offload/fake_flow_backend.h"
#include "offload/flow_rule_owner.h"
#include "packet_reshape.h"
#include "utils/ip.h"
#include "vswitch/vswitch_app.h"
#include "vswitch_appliance.pb.h"

namespace {

using appliance::VswitchApp;
constexpr gate_idx_t kOGates = 2;

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

// One codec value is one whole immutable scope. The revision is minted here,
// never supplied by the client or updated separately from its policy.
std::shared_ptr<const bess::framework::ResourceCodec>
MakePolicyCodec(VswitchApp &app) {
  namespace pb = vswitch_appliance;
  return std::make_shared<
      bess::framework::TypedCodec<pb::TenantKey, pb::PolicyValue>>(
      [](const pb::TenantKey &key)
          -> std::expected<bess::dataplane::ResourceKey, std::string> {
        if (key.id() == 0 || key.id() > VswitchApp::kTenants) {
          return std::unexpected("tenant id must be 1 or 2");
        }
        return bess::dataplane::EncodeKey(bess::dataplane::ScopeId(key.id()));
      },
      [&app](const pb::PolicyValue &value)
          -> std::expected<std::any, std::string> {
        appliance::Policy policy;
        policy.layers.reserve(value.layers_size());
        for (const auto &wire_layer : value.layers()) {
          appliance::Layer layer;
          layer.groups.reserve(wire_layer.groups_size());
          for (const auto &wire_group : wire_layer.groups()) {
            appliance::Group group;
            group.rules.reserve(wire_group.rules_size());
            for (const auto &wire : wire_group.rules()) {
              const uint32_t hi = wire.has_dport_hi() ? wire.dport_hi() : 65535;
              if (wire.prefix_length() > 32 || wire.dport_lo() > hi ||
                  hi > 65535 || wire.protocol() > 255 || wire.dscp() > 63 ||
                  wire.gate() >= kOGates) {
                return std::unexpected(
                    "invalid prefix, port range, protocol, DSCP or gate");
              }
              uint32_t dst = 0;
              if (!wire.destination().empty()) {
                bess::utils::be32_t parsed;
                if (!bess::utils::ParseIpv4Address(wire.destination(),
                                                   &parsed)) {
                  return std::unexpected("invalid IPv4 destination");
                }
                dst = parsed.value();
              } else if (wire.prefix_length() != 0) {
                return std::unexpected(
                    "a nonzero prefix requires a destination");
              }
              const uint32_t mask = wire.prefix_length() == 0
                                        ? 0
                                        : 0xffffffffu
                                              << (32 - wire.prefix_length());
              if ((dst & mask) != dst) {
                return std::unexpected(
                    "destination has bits outside its prefix");
              }
              appliance::Action action;
              switch (wire.action()) {
              case pb::ALLOW:
                action = appliance::Action::kAllow;
                break;
              case pb::DENY:
                action = appliance::Action::kDeny;
                break;
              case pb::MARK:
                action = appliance::Action::kMark;
                break;
              case pb::REDIRECT:
                action = appliance::Action::kRedirect;
                break;
              default:
                return std::unexpected("a known rule action is required");
              }
              group.rules.push_back(appliance::Rule{
                  dst, mask, static_cast<uint16_t>(wire.dport_lo()),
                  static_cast<uint16_t>(hi),
                  static_cast<uint8_t>(wire.protocol()), action,
                  static_cast<uint8_t>(wire.dscp()),
                  static_cast<uint16_t>(wire.gate())});
            }
            layer.groups.push_back(std::move(group));
          }
          policy.layers.push_back(std::move(layer));
        }
        return std::any(app.MakePolicyVersion(std::move(policy)));
      },
      [&app](const auto &visit) {
        for (uint32_t id = 1; id <= VswitchApp::kTenants; ++id) {
          const auto *version =
              app.policy_table().Current(bess::dataplane::ScopeId(id));
          if (version == nullptr) {
            continue;
          }
          pb::TenantKey key;
          key.set_id(id);
          pb::PolicyValue value;
          for (const auto &layer : version->policy().layers) {
            auto *wire_layer = value.add_layers();
            for (const auto &group : layer.groups) {
              auto *wire_group = wire_layer->add_groups();
              for (const auto &rule : group.rules) {
                auto *wire = wire_group->add_rules();
                wire->set_destination(
                    bess::utils::ToIpv4Address(bess::utils::be32_t(rule.dst)));
                wire->set_prefix_length(std::countl_one(rule.dst_mask));
                wire->set_dport_lo(rule.dport_lo);
                wire->set_dport_hi(rule.dport_hi);
                wire->set_protocol(rule.proto);
                wire->set_dscp(rule.dscp);
                wire->set_gate(rule.gate);
                switch (rule.action) {
                case appliance::Action::kAllow:
                  wire->set_action(pb::ALLOW);
                  break;
                case appliance::Action::kDeny:
                  wire->set_action(pb::DENY);
                  break;
                case appliance::Action::kMark:
                  wire->set_action(pb::MARK);
                  break;
                case appliance::Action::kRedirect:
                  wire->set_action(pb::REDIRECT);
                  break;
                }
              }
            }
          }
          visit(key, value);
        }
      });
}

}  // namespace

class VswitchAppliance final : public Module {
 public:
  static const gate_idx_t kNumIGates = VswitchApp::kTenants;
  static const gate_idx_t kNumOGates = kOGates;
  static const Commands cmds;
  static const ResourceTables resource_tables;
  // The packet path fills decision caches and cold-flow counts: one worker.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kSingleWorker,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  VswitchAppliance() : Module() { max_allowed_workers_ = 1; }  // one worker's caches

  CommandResponse Init(const bess::pb::EmptyArg &) {
    auto made = VswitchApp::Create(name(), init_context().rcu(), init_context().memory());
    if (!made) {
      return CommandFailure(ENOMEM, "%s", made.error().c_str());
    }
    if (auto enrolled = (*made)->Enroll(init_context().resources());
        !enrolled) {
      return CommandFailure(EINVAL, "%s", enrolled.error().c_str());
    }
    app_ = std::move(*made);
    policy_binding_ = init_context().codecs().Bind(app_->policy_resource(),
                                                   MakePolicyCodec(*app_));
    return CommandSuccess();
  }

  void DeInit() override {
    if (app_ == nullptr) {
      return;
    }
    policy_binding_.Reset();
    auto released = app_->Release();
    CHECK(released) << released.error();
    app_.reset();
  }

  // VswitchApp writes a DSCP mark in place (TOS and checksum). A shared
  // payload (a clone, an indirect mbuf) is copied first
  // (packet::WritableHead), so another holder never sees the mark; a frame
  // whose copy fails is dropped.
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    for (int i = 0; i < batch->cnt(); i++) {
      bess::PacketHandle handle = batch->handles()[i];
      const auto head = bess::packet::WritableHead(handle, 0);
      const bess::PacketRef pkt(handle);
      const auto d =
          head ? app_->Process(ctx->current_igate,
                               std::span<uint8_t>(reinterpret_cast<uint8_t *>(head->data()),
                                                  head->size()),
                               pkt.total_len())
               : appliance::CompiledDecision{};
      if (d.allow) {
        EmitPacket(ctx, pkt, d.gate);
      } else {
        DropPacket(ctx, pkt);
      }
    }
  }

  CommandResponse CommandSelfTest(const bess::pb::EmptyArg &) {
    // Private switches retain separate scoped resources and memory leases.
    auto r3 = VswitchApp::Create(name() + "_r3_self_test", init_context().rcu(),
                                 init_context().memory());
    auto r5 = VswitchApp::Create(name() + "_r5_self_test", init_context().rcu(),
                                 init_context().memory());
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
  // tenant's hardware rules with its policy; one-off flows age out of the
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
    // Tenant 0 switches web to deny: its hardware rule is revoked with the
    // policy; the flow is cold again and, hot, is promoted as a deny
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
  bess::framework::ResourceBinding policy_binding_;
};

const Commands VswitchAppliance::cmds = {
    {"self_test", "EmptyArg", MODULE_CMD_FUNC(&VswitchAppliance::CommandSelfTest),
     Command::THREAD_UNSAFE},
};

const ResourceTables VswitchAppliance::resource_tables = {
    ResourceTable::Map<vswitch_appliance::TenantKey,
                       vswitch_appliance::PolicyValue>("policies"),
};

BESS_PLUGIN_REQUIRES("vswitch_appliance", "1.0.0", BESS_CAP_INIT_CONTEXT | BESS_CAP_MEMORY);

ADD_MODULE(VswitchAppliance, "vswitch_appliance",
           "Reference appliance R3: a policy vSwitch (layers, groups, compiled decisions)")
