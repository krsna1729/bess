// SPDX-License-Identifier: BSD-3-Clause

#ifndef APPLIANCES_SESSION_SESSION_APP_H_
#define APPLIANCES_SESSION_SESSION_APP_H_

// Reference appliance R4 (roadmap M24, D-086, D-094): a session datapath. Its
// concepts are the application's -- Session, PdrLikeRule, SessionAction,
// QosPolicy -- and none is a BESS action type. The fast path is
//
//   classify (a PDR-like rule) -> the session's action -> meter -> route -> GTP-U
//
// The application owns three transactional resources (QoS policies, actions
// referencing a QoS policy and one of the router's next hops, rules
// referencing an action) and a BESS router; all five are registered with one
// engine (Enroll), so one transaction installs a session across both owners
// and the engine checks the references and publishes referents first.
//
// No protobuf here: the wire codecs are the module's (session_appliance.cc).

#include <any>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/interface_id.h"
#include "dataplane/resource.h"
#include "dataplane/resource_registry.h"
#include "dataplane/slot_resource.h"
#include "dataplane/slot_table.h"
#include "dataplane/strong_id.h"
#include "meter/meter.h"
#include "rcu/rcu_domain.h"
#include "route/router.h"
#include "tunnel/tunnel.h"

namespace appliance {

struct QosIdTag;
struct ActionIdTag;
struct RuleIdTag;
using QosId = bess::dataplane::StrongId<QosIdTag, uint32_t>;
using ActionId = bess::dataplane::StrongId<ActionIdTag, uint32_t>;
using RuleId = bess::dataplane::StrongId<RuleIdTag, uint32_t>;

// A QoS policy: one committed-rate meter, shared by the sessions that name
// it. The profile outlives the state (declared first, destroyed last).
struct QosPolicy {
  std::shared_ptr<const bess::meter::MeterProfile> profile;
  std::shared_ptr<bess::meter::MeterState> meter;

  static std::expected<QosPolicy, std::string> Make(const bess::meter::SrTcmSpec &spec) {
    auto profile = bess::meter::MeterProfile::Create(spec);
    if (!profile) {
      return std::unexpected(std::string("meter profile: ") + bess::meter::MeterErrorName(profile.error()));
    }
    auto shared = std::make_shared<const bess::meter::MeterProfile>(std::move(*profile));
    auto state = bess::meter::MeterState::Create(*shared, bess::meter::MeterSharing::kWorkerExclusive);
    if (!state) {
      return std::unexpected(std::string("meter state: ") + bess::meter::MeterErrorName(state.error()));
    }
    return QosPolicy{std::move(shared), std::shared_ptr<bess::meter::MeterState>(
                                            state->release(), bess::meter::MeterStateDeleter{})};
  }
};

// What a session does with a matched packet: meter, then send it over GTP-U
// (its TEID) to its eNodeB, routed by BESS's router. The next hop is named so
// the engine keeps it while the session needs it.
struct SessionAction {
  QosId qos;
  bess::route::NextHopId next_hop;
  uint32_t enb;  // host order
  uint32_t teid;
};

// A downlink PDR, reduced: the UE's address selects the session's action.
struct PdrLikeRule {
  uint32_t ue_ip;  // host order
  ActionId action;
};

class SessionApp {
 public:
  static constexpr size_t kMaxQos = 16, kMaxActions = 16, kMaxRules = 64, kMaxNextHops = 16;

  // Resources "<name>/qos", "<name>/actions", "<name>/rules", and the
  // router's "<name>/next_hops" and "<name>/routes".
  static std::expected<std::unique_ptr<SessionApp>, std::string> Create(const std::string &name,
                                                                        bess::rcu::RcuDomain &rcu) {
    auto router = bess::route::Router::Create(
        name, bess::route::Router::Config{.max_routes = 64, .tbl8_groups = 4}, kMaxNextHops, rcu);
    if (!router) {
      return std::unexpected(std::string("router: ") + bess::route::RouteErrorName(router.error()));
    }
    return std::unique_ptr<SessionApp>(new SessionApp(name, std::move(*router)));
  }

  // An enrolled application leaves its engine first: its resources before
  // the router's, which they may reference.
  ~SessionApp() {
    if (registry_ != nullptr) {
      (void)Release();
    }
  }

  SessionApp(const SessionApp &) = delete;
  SessionApp &operator=(const SessionApp &) = delete;

  // -- transactions ---------------------------------------------------------

  // Registers the router's resources and the application's three with one
  // engine (the module passes init_context().resources()). From then on the
  // engine is the only writer: the direct setters below refuse.
  std::expected<void, std::string> Enroll(bess::dataplane::ResourceRegistry &registry) {
    if (auto r = router_->Enroll(registry); !r) {
      return r;
    }
    registry_ = &registry;
    for (bess::dataplane::Resource *res :
         std::initializer_list<bess::dataplane::Resource *>{&qos_resource_, &action_resource_,
                                                            &rule_resource_}) {
      if (auto r = registry.Register(res); !r) {
        (void)Release();
        return r;
      }
    }
    return {};
  }

  // Releases all five for teardown, tolerating live keys and referrers still
  // registered (the order modules are destroyed in must not matter).
  std::expected<void, std::string> Release() {
    if (registry_ == nullptr) {
      return {};
    }
    const std::array<std::string, 3> mine = {rule_resource_.name(), action_resource_.name(),
                                             qos_resource_.name()};
    auto released = registry_->ReleaseForTeardown(mine);
    registry_ = nullptr;
    auto router = router_->Release();
    if (!released) {
      return released;
    }
    return router;
  }

  bess::dataplane::Resource &qos_resource() noexcept { return qos_resource_; }
  bess::dataplane::Resource &action_resource() noexcept { return action_resource_; }
  bess::dataplane::Resource &rule_resource() noexcept { return rule_resource_; }
  bess::route::Router &router() noexcept { return *router_; }

  // -- direct writes (an application that is not enrolled) -------------------

  // For an application with no engine and no concurrent reader (the module's
  // self test): the replaced object is freed at once.
  std::expected<void, std::string> SetQos(QosId id, QosPolicy qos) {
    return Put(qos_, id, std::move(qos));
  }
  std::expected<void, std::string> SetAction(ActionId id, SessionAction action) {
    return Put(actions_, id, std::move(action));
  }
  std::expected<void, std::string> SetRule(RuleId id, PdrLikeRule rule) {
    return Put(rules_, id, std::move(rule));
  }

  // -- the fast path ----------------------------------------------------------

  enum class Verdict : uint8_t { kForwarded, kNoSession, kMetered, kNoNextHop };
  struct Decision {
    Verdict verdict = Verdict::kNoSession;
    bess::dataplane::InterfaceId egress{};
    uint32_t teid = 0;
  };

  // The fused decision on one downlink frame (Ethernet + IPv4): classify, the
  // session's action, its meter, the route to its eNodeB. `bytes` is what the
  // meter charges (the packet's length).
  Decision Decide(std::span<const uint8_t> frame, uint32_t bytes, uint64_t now) const noexcept {
    if (frame.size() < kInner + 20 || frame[12] != 0x08 || frame[13] != 0x00) {
      return {};
    }
    uint32_t dst;
    std::memcpy(&dst, frame.data() + kInner + 16, 4);
    dst = __builtin_bswap32(dst);
    const PdrLikeRule *rule = nullptr;
    for (uint32_t i = 1; i <= rules_.capacity() && rule == nullptr; i++) {
      const PdrLikeRule *r = rules_.Lookup(RuleId(i));
      if (r != nullptr && r->ue_ip == dst) {
        rule = r;
      }
    }
    if (rule == nullptr) {
      return {};
    }
    const SessionAction *action = actions_.Lookup(rule->action);
    const QosPolicy *qos = action != nullptr ? qos_.Lookup(action->qos) : nullptr;
    if (action == nullptr || qos == nullptr) {
      return {};
    }
    if (qos->meter->Check(now, bytes) == bess::meter::MeterColor::kRed) {
      return {Verdict::kMetered};
    }
    const bess::route::NextHop *hop = router_->Resolve(action->enb);
    if (hop == nullptr || hop->neighbor != bess::route::NeighborState::kResolved) {
      return {Verdict::kNoNextHop};
    }
    return {Verdict::kForwarded, hop->egress, action->teid};
  }

  // The direct path: the decision, then the GTP-U header written in front of
  // the inner IPv4 packet in `gtpu` (the outer IPv4/UDP headers are the
  // egress's business, not shown).
  struct Out {
    Decision decision;
    std::vector<uint8_t> gtpu;  // GTP-U header + inner packet
  };
  Out Process(std::span<const uint8_t> frame, uint64_t now) const {
    Out out{Decide(frame, static_cast<uint32_t>(frame.size()), now), {}};
    if (out.decision.verdict == Verdict::kForwarded) {
      const auto inner = frame.subspan(kInner);
      out.gtpu.resize(bess::tunnel::kGtpuBaseBytes + inner.size());
      bess::tunnel::WriteGtpu(out.gtpu.data(), out.decision.teid, static_cast<uint16_t>(inner.size()));
      std::memcpy(out.gtpu.data() + bess::tunnel::kGtpuBaseBytes, inner.data(), inner.size());
    }
    return out;
  }

  static constexpr size_t kInner = 14;  // the inner IPv4 packet's offset in a frame

 private:
  SessionApp(const std::string &name, std::unique_ptr<bess::route::Router> router)
      : qos_(kMaxQos),
        actions_(kMaxActions),
        rules_(kMaxRules),
        qos_resource_(name + "/qos", qos_),
        action_resource_(
            name + "/actions", actions_,
            [qos = name + "/qos", hops = router->next_hops_resource()](const SessionAction &a) {
              return std::vector<bess::dataplane::Reference>{
                  {qos, bess::dataplane::EncodeKey(a.qos)},
                  {hops, bess::dataplane::EncodeKey(a.next_hop)}};
            },
            {name + "/qos", router->next_hops_resource()}),
        rule_resource_(
            name + "/rules", rules_,
            [actions = name + "/actions"](const PdrLikeRule &r) {
              return std::vector<bess::dataplane::Reference>{
                  {actions, bess::dataplane::EncodeKey(r.action)}};
            },
            {name + "/actions"}),
        router_(std::move(router)) {}

  template <typename Id, typename T>
  std::expected<void, std::string> Put(bess::dataplane::SlotTable<Id, T> &table, Id id, T value) {
    if (registry_ != nullptr) {
      return std::unexpected("enrolled: the engine is the writer");
    }
    if (!table.CanPublish(id)) {
      return std::unexpected("invalid id");
    }
    table.Publish(id, std::make_unique<const T>(std::move(value)));
    return {};
  }

  bess::dataplane::SlotTable<QosId, QosPolicy> qos_;
  bess::dataplane::SlotTable<ActionId, SessionAction> actions_;
  bess::dataplane::SlotTable<RuleId, PdrLikeRule> rules_;
  bess::dataplane::SlotResource<QosId, QosPolicy> qos_resource_;
  bess::dataplane::SlotResource<ActionId, SessionAction> action_resource_;
  bess::dataplane::SlotResource<RuleId, PdrLikeRule> rule_resource_;
  std::unique_ptr<bess::route::Router> router_;
  bess::dataplane::ResourceRegistry *registry_ = nullptr;
};

}  // namespace appliance

#endif  // APPLIANCES_SESSION_SESSION_APP_H_
