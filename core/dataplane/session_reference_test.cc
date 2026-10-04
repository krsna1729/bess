// SPDX-License-Identifier: BSD-3-Clause

// Reference appliance R4 (roadmap M24, D-086): a session datapath. Its
// concepts are the application's -- Session, PdrLikeRule, SessionAction,
// QoSPolicy -- and none is a BESS action type. The fast path is
//
//   classify (a PDR-like rule) -> the session's action -> meter -> route -> GTP-U
//
// One generic BESS transaction installs a session: a next hop and a route in
// BESS's router, and the application's QoS policy, action and rule, which
// reference the router's next hop and each other. The engine checks those
// references across owners and publishes referents before referrers.
//
// In-tree, not an installed-tree plugin like R1-R3: an application resource is
// reached over the wire through a codec, and codecs are internal (D-074), so
// this test drives the transaction engine in-process.

#include <gtest/gtest.h>

#include <any>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dataplane/slot_resource.h"
#include "dataplane/slot_table.h"
#include "dataplane/transaction_engine.h"
#include "meter/meter.h"
#include "route/router.h"
#include "runtime/runtime_state.h"
#include "tunnel/tunnel.h"

namespace session_app {
namespace {

using bess::dataplane::EncodeKey;
using bess::dataplane::Op;
using bess::dataplane::Reference;
using bess::dataplane::TransactionEngine;
using bess::route::NextHop;
using bess::route::NextHopId;
using Outcome = TransactionEngine::Outcome;

// -- the application's model -------------------------------------------------------

struct QosIdTag;
struct ActionIdTag;
struct RuleIdTag;
using QosId = bess::dataplane::StrongId<QosIdTag, uint32_t>;
using ActionId = bess::dataplane::StrongId<ActionIdTag, uint32_t>;
using RuleId = bess::dataplane::StrongId<RuleIdTag, uint32_t>;

// A QoS policy: one committed-rate meter (the application shares it between
// the sessions that name it).
struct QosPolicy {
  std::shared_ptr<bess::meter::MeterProfile> profile;
  std::shared_ptr<bess::meter::MeterState> meter;
};

// What a session does with a matched packet: meter, then send it over GTP-U
// (its TEID) through a next hop the router owns.
struct SessionAction {
  QosId qos;
  NextHopId next_hop;
  uint32_t teid;
};

// A downlink PDR, reduced: the UE's address selects the session's action.
struct PdrLikeRule {
  uint32_t ue_ip;  // host order
  ActionId action;
};

constexpr char kRouterNextHops[] = "rt/next_hops";

class SessionApp {
 public:
  explicit SessionApp(bess::rcu::RcuDomain &rcu)
      : qos_(16), actions_(16), rules_(64),
        qos_resource_("sess/qos", qos_),
        action_resource_("sess/actions", actions_,
                         [](const SessionAction &a) {
                           return std::vector<Reference>{{"sess/qos", EncodeKey(a.qos)},
                                                         {kRouterNextHops, EncodeKey(a.next_hop)}};
                         },
                         {"sess/qos", kRouterNextHops}),
        rule_resource_("sess/rules", rules_,
                       [](const PdrLikeRule &r) {
                         return std::vector<Reference>{{"sess/actions", EncodeKey(r.action)}};
                       },
                       {"sess/actions"}) {
    bess::route::Router::Config config;
    config.max_routes = 64;
    config.tbl8_groups = 4;
    router_ = std::move(*bess::route::Router::Create("rt", config, 16, rcu));
  }

  // The application's resources leave before the router's, which they may
  // reference (the engine checks it).
  ~SessionApp() {
    if (engine_ != nullptr) {
      const std::array<std::string, 3> mine = {"sess/rules", "sess/actions", "sess/qos"};
      (void)engine_->ReleaseForTeardown(mine);
    }
  }

  std::expected<void, std::string> Enroll(TransactionEngine &engine) {
    engine_ = &engine;
    if (auto r = router_->Enroll(engine.registry()); !r) {
      return r;
    }
    for (bess::dataplane::Resource *res :
         std::initializer_list<bess::dataplane::Resource *>{&qos_resource_, &action_resource_,
                                                            &rule_resource_}) {
      if (auto r = engine.registry().Register(res); !r) {
        return r;
      }
    }
    return {};
  }

  // The fused direct path on one downlink frame (Ethernet + IPv4 + payload)
  // with `headroom` bytes free in front of it: GTP-U encapsulation in place.
  enum class Verdict { kForwarded, kNoSession, kMetered, kNoNextHop };
  struct Out {
    Verdict verdict;
    bess::dataplane::InterfaceId egress{};
    uint32_t teid = 0;
  };
  Out Process(std::span<const uint8_t> frame, uint64_t now) const {
    uint32_t dst;
    std::memcpy(&dst, frame.data() + 30, 4);
    dst = __builtin_bswap32(dst);
    const PdrLikeRule *rule = nullptr;
    for (uint32_t i = 1; i <= rules_.capacity() && rule == nullptr; i++) {
      const PdrLikeRule *r = rules_.Lookup(RuleId(i));
      if (r != nullptr && r->ue_ip == dst) {
        rule = r;
      }
    }
    if (rule == nullptr) {
      return {Verdict::kNoSession};
    }
    const SessionAction *action = actions_.Lookup(rule->action);
    const QosPolicy *qos = action != nullptr ? qos_.Lookup(action->qos) : nullptr;
    if (action == nullptr || qos == nullptr) {
      return {Verdict::kNoSession};
    }
    if (qos->meter->Check(now, static_cast<uint32_t>(frame.size())) ==
        bess::meter::MeterColor::kRed) {
      return {Verdict::kMetered};
    }
    const NextHop *hop = router_->LookupNextHop(action->next_hop);
    if (hop == nullptr) {
      return {Verdict::kNoNextHop};
    }
    std::array<uint8_t, bess::tunnel::kGtpuBaseBytes> gtpu{};
    bess::tunnel::WriteGtpu(gtpu.data(), action->teid, static_cast<uint16_t>(frame.size() - 14));
    uint32_t teid;
    std::memcpy(&teid, gtpu.data() + 4, 4);
    return {Verdict::kForwarded, hop->egress, __builtin_bswap32(teid)};
  }

  // Ops installing a session: the router's next hop and route toward the
  // eNodeB, and the application's QoS policy, action and rule.
  std::vector<Op> InstallSession(uint32_t n, uint32_t ue_ip, uint32_t enb, uint64_t rate) {
    NextHop hop;
    hop.egress = bess::dataplane::InterfaceId(static_cast<uint16_t>(n));
    hop.neighbor = bess::route::NeighborState::kResolved;
    auto profile = std::make_shared<bess::meter::MeterProfile>(
        *bess::meter::MeterProfile::Create(bess::meter::SrTcmSpec{rate, 1500, 1500}));
    auto meter = bess::meter::MeterState::Create(*profile, bess::meter::MeterSharing::kWorkerExclusive);
    QosPolicy qos{profile, std::shared_ptr<bess::meter::MeterState>(meter->release(),
                                                                    bess::meter::MeterStateDeleter{})};
    return {router_->SetNextHopOp(NextHopId(n), hop),
            router_->SetRouteOp(*bess::route::Ipv4Prefix::Make(enb, 32), NextHopId(n)),
            Op::Upsert("sess/qos", EncodeKey(QosId(n)), std::any(qos)),
            Op::Upsert("sess/actions", EncodeKey(ActionId(n)),
                       std::any(SessionAction{QosId(n), NextHopId(n), 0x1000 + n})),
            Op::Upsert("sess/rules", EncodeKey(RuleId(n)), std::any(PdrLikeRule{ue_ip, ActionId(n)}))};
  }

  bess::route::Router &router() { return *router_; }

 private:
  bess::dataplane::SlotTable<QosId, QosPolicy> qos_;
  bess::dataplane::SlotTable<ActionId, SessionAction> actions_;
  bess::dataplane::SlotTable<RuleId, PdrLikeRule> rules_;
  bess::dataplane::SlotResource<QosId, QosPolicy> qos_resource_;
  bess::dataplane::SlotResource<ActionId, SessionAction> action_resource_;
  bess::dataplane::SlotResource<RuleId, PdrLikeRule> rule_resource_;
  std::unique_ptr<bess::route::Router> router_;
  TransactionEngine *engine_ = nullptr;
};

std::vector<uint8_t> Downlink(uint32_t dst, size_t bytes = 100) {
  std::vector<uint8_t> f(bytes, 0);
  f[12] = 0x08;
  f[14] = 0x45;
  for (int i = 0; i < 4; i++) f[30 + i] = static_cast<uint8_t>(dst >> (24 - 8 * i));
  return f;
}

class SessionReferenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    app_ = std::make_unique<SessionApp>(bess::runtime::runtime().rcu());
    ASSERT_TRUE(app_->Enroll(engine_));
  }
  void TearDown() override {
    while (engine_.ReclaimRetired() != 0) {
    }
    bess::runtime::runtime().rcu().Drain();
  }
  TransactionEngine engine_{bess::runtime::runtime().rcu()};
  std::unique_ptr<SessionApp> app_;
};

constexpr uint32_t kUe = 0x0a2d0001, kEnb = 0xc0000264;

// One transaction installs a whole session across BESS's router and the
// application's resources; the fused path then carries the UE's downlink
// through classification, the session's action, its meter and the router's
// next hop to a GTP-U header with the session's TEID.
TEST_F(SessionReferenceTest, OneTransactionInstallsASessionAcrossOwners) {
  const auto r = engine_.Apply(app_->InstallSession(1, kUe, kEnb, 1'000'000'000));
  ASSERT_EQ(Outcome::kApplied, r.outcome);
  const auto out = app_->Process(Downlink(kUe), bess::meter::MeterNow());
  EXPECT_EQ(SessionApp::Verdict::kForwarded, out.verdict);
  EXPECT_EQ(1u, out.egress.value());
  EXPECT_EQ(0x1001u, out.teid);
  EXPECT_EQ(SessionApp::Verdict::kNoSession,
            app_->Process(Downlink(kUe + 1), bess::meter::MeterNow()).verdict);
}

// A session whose action names a next hop nobody installs is refused whole:
// the application's rule and action never appear, the router is unchanged.
TEST_F(SessionReferenceTest, AReferenceToAMissingNextHopRefusesTheWholeTransaction) {
  auto ops = app_->InstallSession(2, kUe, kEnb, 1'000'000'000);
  ops.erase(ops.begin(), ops.begin() + 2);  // no next hop, no route
  const auto r = engine_.Apply(ops);
  EXPECT_EQ(Outcome::kRejected, r.outcome);
  EXPECT_EQ(SessionApp::Verdict::kNoSession,
            app_->Process(Downlink(kUe), bess::meter::MeterNow()).verdict);
  EXPECT_EQ(0u, app_->router().next_hop_count());
}

// The router's next hop cannot leave while the application's action still
// references it: the engine refuses, across owners.
TEST_F(SessionReferenceTest, ABessNextHopAnApplicationActionNamesCannotBeRemoved) {
  ASSERT_EQ(Outcome::kApplied, engine_.Apply(app_->InstallSession(3, kUe, kEnb, 1'000'000'000)).outcome);
  const auto r = engine_.Apply(std::vector<Op>{app_->router().RemoveRouteOp(*bess::route::Ipv4Prefix::Make(kEnb, 32)),
                                app_->router().RemoveNextHopOp(NextHopId(3))});
  EXPECT_EQ(Outcome::kRejected, r.outcome);
  EXPECT_EQ(SessionApp::Verdict::kForwarded,
            app_->Process(Downlink(kUe), bess::meter::MeterNow()).verdict);
  // Removing the session first (rule, action, QoS) and the next hop in one
  // transaction is accepted: referrers go before referents.
  const auto all = engine_.Apply(std::vector<Op>{Op::Erase("sess/rules", EncodeKey(RuleId(3))),
                                  Op::Erase("sess/actions", EncodeKey(ActionId(3))),
                                  Op::Erase("sess/qos", EncodeKey(QosId(3))),
                                  app_->router().RemoveRouteOp(*bess::route::Ipv4Prefix::Make(kEnb, 32)),
                                  app_->router().RemoveNextHopOp(NextHopId(3))});
  EXPECT_EQ(Outcome::kApplied, all.outcome);
}

// The session's meter applies: a 64 kB/s policy lets the first packets
// through, then marks the burst red.
TEST_F(SessionReferenceTest, TheSessionsMeterLimitsIt) {
  ASSERT_EQ(Outcome::kApplied, engine_.Apply(app_->InstallSession(4, kUe, kEnb, 64'000)).outcome);
  const uint64_t now = bess::meter::MeterNow();
  int forwarded = 0, metered = 0;
  for (int i = 0; i < 100; i++) {
    const auto v = app_->Process(Downlink(kUe, 1000), now).verdict;
    forwarded += v == SessionApp::Verdict::kForwarded;
    metered += v == SessionApp::Verdict::kMetered;
  }
  EXPECT_GT(forwarded, 0);
  EXPECT_GT(metered, 0);
}

}  // namespace
}  // namespace session_app
