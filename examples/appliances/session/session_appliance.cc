// SPDX-License-Identifier: BSD-3-Clause

// R4's graph adapter and wire (roadmap M24, D-094): SessionApp in a module
// whose five resources -- the application's QoS policies, actions and rules,
// and its router's next hops and routes -- are registered with bessd's engine
// through init_context().resources() and reachable over the control API
// (bess.pb.v2.ApplyTransaction) through the codecs bound here with
// init_context().codecs(). The application's wire types are session.proto's;
// the router's are BESS's (bess.pb.RouterNextHopIdKey, ...).
//
// Graph path: a downlink frame on igate 0 that a session claims leaves on the
// gate of its next hop's interface (interface i is output gate i-1) as the
// GTP-U header followed by the inner IPv4 packet; everything else is dropped.
//
// `self_test` runs the direct path on a private SessionApp (no engine, no
// graph): one session's five operations decoded by the same codecs the
// module binds, written directly, and the fused path checked on frames in
// memory -- forwarding with the session's TEID, a miss, the meter, and no way
// out without the route. The engine's side (references across owners,
// all-or-nothing) is checked over the wire by
// tools/check_standalone_plugins.py --set appliances.

#include <any>
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
#include "pb/module_msg.pb.h"
#include "session.pb.h"
#include "session/session_app.h"
#include "utils/ip.h"

namespace {

using appliance::ActionId;
using appliance::PdrLikeRule;
using appliance::QosId;
using appliance::QosPolicy;
using appliance::RuleId;
using appliance::SessionAction;
using appliance::SessionApp;
using bess::dataplane::EncodeKey;
using bess::dataplane::ResourceKey;
using bess::framework::ResourceCodec;
using bess::framework::TypedCodec;
using bess::route::NextHop;
using bess::route::NextHopId;

constexpr gate_idx_t kOGates = 4;

std::expected<uint32_t, std::string> Ipv4(const std::string &dotted, const char *what) {
  bess::utils::be32_t addr;
  if (!bess::utils::ParseIpv4Address(dotted, &addr)) {
    return std::unexpected(std::string(what) + ": invalid IPv4 address '" + dotted + "'");
  }
  return addr.value();
}

template <typename Id>
std::expected<ResourceKey, std::string> IdKey(uint32_t id, const char *what) {
  if (id == 0) {
    return std::unexpected(std::string(what) + " id 0 is invalid");
  }
  return EncodeKey(Id(id));
}

// The codecs of the five resources; the module binds them, the self test
// decodes through them.
struct Codecs {
  std::shared_ptr<const ResourceCodec> qos, actions, rules, next_hops, routes;
};

Codecs MakeCodecs() {
  namespace pb = session_appliance;
  Codecs c;
  c.qos = std::make_shared<TypedCodec<pb::QosKey, pb::QosValue>>(
      [](const pb::QosKey &k) { return IdKey<QosId>(k.id(), "qos"); },
      [](const pb::QosValue &v) -> std::expected<std::any, std::string> {
        auto qos = QosPolicy::Make(
            bess::meter::SrTcmSpec{v.committed_rate(), v.committed_burst(), v.excess_burst()});
        if (!qos) {
          return std::unexpected(qos.error());
        }
        return std::any(std::move(*qos));
      });
  c.actions = std::make_shared<TypedCodec<pb::ActionKey, pb::ActionValue>>(
      [](const pb::ActionKey &k) { return IdKey<ActionId>(k.id(), "action"); },
      [](const pb::ActionValue &v) -> std::expected<std::any, std::string> {
        if (v.qos_id() == 0 || v.next_hop_id() == 0) {
          return std::unexpected("qos_id and next_hop_id must be non-zero");
        }
        auto enb = Ipv4(v.enb(), "enb");
        if (!enb) {
          return std::unexpected(enb.error());
        }
        return std::any(SessionAction{QosId(v.qos_id()), NextHopId(v.next_hop_id()), *enb, v.teid()});
      });
  c.rules = std::make_shared<TypedCodec<pb::RuleKey, pb::RuleValue>>(
      [](const pb::RuleKey &k) { return IdKey<RuleId>(k.id(), "rule"); },
      [](const pb::RuleValue &v) -> std::expected<std::any, std::string> {
        if (v.action_id() == 0) {
          return std::unexpected("action_id must be non-zero");
        }
        auto ue = Ipv4(v.ue(), "ue");
        if (!ue) {
          return std::unexpected(ue.error());
        }
        return std::any(PdrLikeRule{*ue, ActionId(v.action_id())});
      });
  // The router's resources, in BESS's own wire types. The egress gate is
  // this module's output gate; the application's interface is gate + 1.
  c.next_hops = std::make_shared<TypedCodec<bess::pb::RouterNextHopIdKey, bess::pb::RouterNextHopValue>>(
      [](const bess::pb::RouterNextHopIdKey &k) { return IdKey<NextHopId>(k.id(), "next hop"); },
      [](const bess::pb::RouterNextHopValue &v) -> std::expected<std::any, std::string> {
        if (v.egress_gate() >= kOGates) {
          return std::unexpected("egress_gate must be below " + std::to_string(kOGates));
        }
        NextHop hop;
        hop.egress = bess::dataplane::InterfaceId(static_cast<uint16_t>(v.egress_gate() + 1));
        switch (v.neighbor()) {
          case bess::pb::ROUTER_NEIGHBOR_STATE_UNSPECIFIED:
          case bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED:
            hop.neighbor = bess::route::NeighborState::kResolved;
            break;
          case bess::pb::ROUTER_NEIGHBOR_STATE_INCOMPLETE:
            hop.neighbor = bess::route::NeighborState::kIncomplete;
            break;
          case bess::pb::ROUTER_NEIGHBOR_STATE_UNREACHABLE:
            hop.neighbor = bess::route::NeighborState::kUnreachable;
            break;
          default:
            return std::unexpected("invalid neighbor state");
        }
        return std::any(hop);
      });
  c.routes = std::make_shared<TypedCodec<bess::pb::RouterRouteKey, bess::pb::RouterRouteValue>>(
      [](const bess::pb::RouterRouteKey &k) -> std::expected<ResourceKey, std::string> {
        auto addr = Ipv4(k.ipv4(), "route");
        if (!addr) {
          return std::unexpected(addr.error());
        }
        if (k.domain() != 0 || k.prefix_length() > 32) {
          return std::unexpected("the default domain only, prefix length 0..32");
        }
        auto prefix = bess::route::Ipv4Prefix::Make(*addr, static_cast<uint8_t>(k.prefix_length()));
        if (!prefix) {
          return std::unexpected(bess::route::RouteErrorName(prefix.error()));
        }
        return bess::route::Router::RouteKey(*prefix);
      },
      [](const bess::pb::RouterRouteValue &v) -> std::expected<std::any, std::string> {
        if (v.next_hop_id() == 0) {
          return std::unexpected("next hop id 0 is invalid");
        }
        return std::any(NextHopId(v.next_hop_id()));
      });
  return c;
}

// The wire form of a message as ApplyTransaction carries it (a packed Any).
template <typename M>
std::expected<ResourceKey, std::string> DecodeKey(const ResourceCodec &codec, const M &msg) {
  return codec.Key("type.googleapis.com/" + std::string(M::descriptor()->full_name()),
                   msg.SerializeAsString());
}
template <typename T, typename M>
std::expected<T, std::string> DecodeValue(const ResourceCodec &codec, const M &msg) {
  auto any = codec.Value("type.googleapis.com/" + std::string(M::descriptor()->full_name()),
                         msg.SerializeAsString());
  if (!any) {
    return std::unexpected(any.error());
  }
  const T *value = std::any_cast<T>(&*any);
  if (value == nullptr) {
    return std::unexpected("the codec returned another value type");
  }
  return *value;
}

// Ethernet + IPv4 to `dst` (host order) + payload, `bytes` long.
std::vector<uint8_t> Downlink(uint32_t dst, size_t bytes = 100) {
  std::vector<uint8_t> f(bytes, 0);
  f[12] = 0x08;
  f[14] = 0x45;
  for (int i = 0; i < 4; i++) f[30 + i] = static_cast<uint8_t>(dst >> (24 - 8 * i));
  return f;
}

}  // namespace

class SessionAppliance final : public Module {
 public:
  static const gate_idx_t kNumIGates = 1;
  static const gate_idx_t kNumOGates = kOGates;
  static const Commands cmds;

  CommandResponse Init(const bess::pb::EmptyArg &) {
    auto made = SessionApp::Create(name(), init_context().rcu());
    if (!made) {
      return CommandFailure(EINVAL, "%s", made.error().c_str());
    }
    if (auto enrolled = (*made)->Enroll(init_context().resources()); !enrolled) {
      return CommandFailure(EINVAL, "%s", enrolled.error().c_str());
    }
    app_ = std::move(*made);
    const Codecs c = MakeCodecs();
    bess::framework::ResourceBindings &codecs = init_context().codecs();
    qos_binding_ = codecs.Bind(app_->qos_resource(), c.qos);
    actions_binding_ = codecs.Bind(app_->action_resource(), c.actions);
    rules_binding_ = codecs.Bind(app_->rule_resource(), c.rules);
    next_hops_binding_ = codecs.Bind(*app_->router().next_hops_resource_object(), c.next_hops);
    routes_binding_ = codecs.Bind(*app_->router().routes_resource_object(), c.routes);
    return CommandSuccess();
  }

  void DeInit() override {
    if (app_ == nullptr) {
      return;
    }
    qos_binding_.Reset();
    actions_binding_.Reset();
    rules_binding_.Reset();
    next_hops_binding_.Reset();
    routes_binding_.Reset();
    auto released = app_->Release();
    CHECK(released) << released.error();
    app_.reset();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    const uint64_t now = bess::meter::MeterNow();
    for (int i = 0; i < batch->cnt(); i++) {
      bess::PacketRef pkt = batch->packet(i);
      const SessionApp::Decision d = app_->Decide(
          std::span<const uint8_t>(pkt.head_data<const uint8_t *>(), pkt.head_len()), pkt.total_len(),
          now);
      // GTP-U in place: the Ethernet header's last 8 bytes become the GTP-U
      // header in front of the inner packet.
      constexpr uint16_t kTrim = SessionApp::kInner - bess::tunnel::kGtpuBaseBytes;
      if (d.verdict == SessionApp::Verdict::kForwarded && pkt.adj(kTrim) != nullptr) {
        bess::tunnel::WriteGtpu(pkt.head_data<uint8_t *>(), d.teid,
                                static_cast<uint16_t>(pkt.total_len() - bess::tunnel::kGtpuBaseBytes));
        EmitPacket(ctx, pkt, static_cast<gate_idx_t>(d.egress.value() - 1));
      } else {
        DropPacket(ctx, pkt);
      }
    }
  }

  CommandResponse CommandSelfTest(const bess::pb::EmptyArg &) {
    std::string failed;
    {
      auto made = SessionApp::Create(name() + "_self_test", init_context().rcu());
      if (!made) {
        return CommandFailure(EINVAL, "%s", made.error().c_str());
      }
      failed = SelfTest(**made);
    }
    // The private application's retired objects are freed by this plugin's
    // code: free them before the command returns.
    init_context().rcu().Drain();
    if (!failed.empty()) {
      return CommandFailure(EINVAL, "R4 direct path: %s", failed.c_str());
    }
    return CommandSuccess();
  }

 private:
  // Returns "" or what failed.
  static std::string SelfTest(SessionApp &app) {
    using V = SessionApp::Verdict;
    namespace pb = session_appliance;
    constexpr uint32_t kUe = 0x0a2d0001, kEnb = 0xc0000264;  // 10.45.0.1, 192.0.2.100
    const Codecs c = MakeCodecs();

    // One session's five operations, decoded as ApplyTransaction decodes them.
    bess::pb::RouterNextHopIdKey hop_key;
    hop_key.set_id(1);
    bess::pb::RouterNextHopValue hop_value;
    hop_value.set_egress_gate(0);
    hop_value.set_neighbor(bess::pb::ROUTER_NEIGHBOR_STATE_RESOLVED);
    bess::pb::RouterRouteKey route_key;
    route_key.set_ipv4("192.0.2.100");
    route_key.set_prefix_length(32);
    bess::pb::RouterRouteValue route_value;
    route_value.set_next_hop_id(1);
    pb::QosKey qos_key;
    qos_key.set_id(1);
    pb::QosValue qos_value;
    qos_value.set_committed_rate(1'000'000'000);
    qos_value.set_committed_burst(1500);
    qos_value.set_excess_burst(1500);
    pb::ActionKey action_key;
    action_key.set_id(1);
    pb::ActionValue action_value;
    action_value.set_qos_id(1);
    action_value.set_next_hop_id(1);
    action_value.set_enb("192.0.2.100");
    action_value.set_teid(0x1001);
    pb::RuleKey rule_key;
    rule_key.set_id(1);
    pb::RuleValue rule_value;
    rule_value.set_ue("10.45.0.1");
    rule_value.set_action_id(1);

    const auto enb32 = *bess::route::Ipv4Prefix::Make(kEnb, 32);
    if (DecodeKey(*c.next_hops, hop_key) != EncodeKey(NextHopId(1)) ||
        DecodeKey(*c.routes, route_key) != bess::route::Router::RouteKey(enb32) ||
        DecodeKey(*c.qos, qos_key) != EncodeKey(QosId(1)) ||
        DecodeKey(*c.actions, action_key) != EncodeKey(ActionId(1)) ||
        DecodeKey(*c.rules, rule_key) != EncodeKey(RuleId(1))) {
      return "a key decoded to the wrong resource key";
    }
    auto hop = DecodeValue<NextHop>(*c.next_hops, hop_value);
    auto via = DecodeValue<NextHopId>(*c.routes, route_value);
    auto qos = DecodeValue<QosPolicy>(*c.qos, qos_value);
    auto action = DecodeValue<SessionAction>(*c.actions, action_value);
    auto rule = DecodeValue<PdrLikeRule>(*c.rules, rule_value);
    if (!hop || !via || !qos || !action || !rule) {
      return "a value did not decode";
    }
    if (hop->egress.value() != 1 || *via != NextHopId(1) || action->qos != QosId(1) ||
        action->next_hop != NextHopId(1) || action->enb != kEnb || action->teid != 0x1001 ||
        rule->ue_ip != kUe || rule->action != ActionId(1)) {
      return "a value decoded to the wrong fields";
    }
    // Refusals: id 0, a message under another type's URL, a bad address, a
    // gate the module does not have.
    pb::QosKey zero;
    pb::RuleValue bad_ue = rule_value;
    bad_ue.set_ue("10.45.0");
    bess::pb::RouterNextHopValue bad_gate = hop_value;
    bad_gate.set_egress_gate(kOGates);
    if (DecodeKey(*c.qos, zero) || DecodeKey(*c.actions, qos_key) ||
        DecodeValue<PdrLikeRule>(*c.rules, bad_ue) || DecodeValue<NextHop>(*c.next_hops, bad_gate)) {
      return "an invalid key or value was accepted";
    }

    // Written directly (no engine), then the fused path on frames.
    if (!app.router().SetNextHop(NextHopId(1), *hop) || !app.router().SetRoute(enb32, *via) ||
        !app.SetQos(QosId(1), *qos) || !app.SetAction(ActionId(1), *action) ||
        !app.SetRule(RuleId(1), *rule)) {
      return "the decoded session could not be written";
    }
    const auto out = app.Process(Downlink(kUe), bess::meter::MeterNow());
    if (out.decision.verdict != V::kForwarded || out.decision.egress.value() != 1) {
      return "the session's downlink was not forwarded to its next hop";
    }
    const auto &g = out.gtpu;
    if (g.size() != bess::tunnel::kGtpuBaseBytes + 86 || g[0] != 0x30 || g[1] != 0xff ||
        (g[2] << 8 | g[3]) != 86 ||
        (uint32_t{g[4]} << 24 | uint32_t{g[5]} << 16 | uint32_t{g[6]} << 8 | g[7]) != 0x1001 ||
        g[8] != 0x45) {
      return "wrong GTP-U header or inner packet";
    }
    if (app.Process(Downlink(kUe + 1), bess::meter::MeterNow()).decision.verdict != V::kNoSession) {
      return "a UE with no session was forwarded";
    }
    // The session's meter applies: a 64 kB/s policy lets the first packets
    // through, then marks the burst red.
    auto slow = QosPolicy::Make(bess::meter::SrTcmSpec{64'000, 1500, 1500});
    if (!slow || !app.SetQos(QosId(2), *slow) ||
        !app.SetAction(ActionId(2), SessionAction{QosId(2), NextHopId(1), kEnb, 0x1002}) ||
        !app.SetRule(RuleId(2), PdrLikeRule{kUe + 2, ActionId(2)})) {
      return "the metered session could not be written";
    }
    const uint64_t now = bess::meter::MeterNow();
    int forwarded = 0, metered = 0;
    for (int i = 0; i < 100; i++) {
      const V v = app.Process(Downlink(kUe + 2, 1000), now).decision.verdict;
      forwarded += v == V::kForwarded;
      metered += v == V::kMetered;
    }
    if (forwarded == 0 || metered == 0) {
      return "the session's meter did not limit it";
    }
    // The route is what carries it: without the route there is no way out.
    if (!app.router().RemoveRoute(enb32)) {
      return "route removal";
    }
    if (app.Process(Downlink(kUe), bess::meter::MeterNow()).decision.verdict != V::kNoNextHop) {
      return "a session with no route was forwarded";
    }
    return "";
  }

  // Declared before the bindings, which are reset first.
  std::unique_ptr<SessionApp> app_;
  bess::framework::ResourceBinding qos_binding_;
  bess::framework::ResourceBinding actions_binding_;
  bess::framework::ResourceBinding rules_binding_;
  bess::framework::ResourceBinding next_hops_binding_;
  bess::framework::ResourceBinding routes_binding_;
};

const Commands SessionAppliance::cmds = {
    {"self_test", "EmptyArg", MODULE_CMD_FUNC(&SessionAppliance::CommandSelfTest),
     Command::THREAD_UNSAFE},  // the private application is destroyed with workers paused
};

BESS_PLUGIN_REQUIRES("session_appliance", "1.0.0", BESS_CAP_INIT_CONTEXT | BESS_CAP_RESOURCES);

ADD_MODULE(SessionAppliance, "session_appliance",
           "Reference appliance R4: a session datapath whose resources are driven over the control API")
