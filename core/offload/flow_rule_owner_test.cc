// SPDX-License-Identifier: BSD-3-Clause

// Hardware flow-rule lifecycle (M20, D-070), in software: the owner's state
// machine over a fake backend with injected completions, failures and resets.

#include "offload/flow_rule_owner.h"

#include <gtest/gtest.h>

#include <vector>

#include "offload/fake_flow_backend.h"
#include "offload/rte_flow_backend.h"

namespace bess::offload {
namespace {

using Owner = FlowRuleOwner<FakeFlowBackend>;

FlowCapabilities Nic() {
  FlowCapabilities c;
  c.supported = true;
  c.async = true;
  c.count_action = true;
  c.mark_action = true;
  c.mark_bits = 32;
  return c;
}

struct Fixture {
  FakeFlowBackend backend;
  Owner owner;
  std::vector<Completion> done;
  explicit Fixture(Owner::Config c = {}) : owner(backend, c) {
    backend.SetCapabilities(0, Nic());
    backend.SetCapabilities(1, Nic());
  }
  size_t Poll() {
    return owner.Poll([&](const Completion &c) { done.push_back(c); });
  }
};

TEST(FlowRuleOwnerTest, AsyncInstallIsNotInstalledUntilTheDeviceSaysSo) {
  Fixture f;
  const auto r = f.owner.Install(0, /*rule=*/7, /*cookie=*/100);
  ASSERT_EQ(InstallError::kOk, r.status);
  EXPECT_EQ(RuleState::kSubmitted, f.owner.StateOf(r.handle));
  EXPECT_FALSE(f.owner.MarkCookie(r.mark).has_value()) << "no packet may map before install";
  EXPECT_EQ(1u, f.owner.outstanding());
  EXPECT_EQ(0u, f.Poll()) << "nothing completed yet";
  f.backend.Complete(1);
  EXPECT_EQ(1u, f.Poll());
  ASSERT_EQ(1u, f.done.size());
  EXPECT_EQ(RuleState::kInstalled, f.done[0].state);
  EXPECT_EQ(100u, f.done[0].cookie);
  EXPECT_EQ(RuleState::kInstalled, f.owner.StateOf(r.handle));
  EXPECT_EQ(100u, f.owner.MarkCookie(r.mark).value());
  EXPECT_EQ(0u, f.owner.outstanding());
  // Statistics come from the device.
  f.backend.Hit(1, 1500);
  FlowRuleStats st;
  ASSERT_TRUE(f.owner.Stats(r.handle, st));
  EXPECT_EQ(1u, st.hits);
  EXPECT_EQ(1500u, st.bytes);
  // Removal: requested, then confirmed; the handle goes stale.
  ASSERT_TRUE(f.owner.Remove(r.handle));
  EXPECT_EQ(RuleState::kRemoving, f.owner.StateOf(r.handle));
  EXPECT_FALSE(f.owner.MarkCookie(r.mark).has_value()) << "new packets stop mapping at once";
  f.backend.Complete(1);
  f.Poll();
  EXPECT_EQ(RuleState::kRemoved, f.done.back().state);
  EXPECT_FALSE(f.owner.StateOf(r.handle).has_value());
  EXPECT_EQ(0u, f.backend.installed(0));
}

TEST(FlowRuleOwnerTest, FailuresAndRefusalsAreReportedAndReleaseTheirResources) {
  Fixture f;
  f.backend.FailNextInstall(EOPNOTSUPP);
  const auto r = f.owner.Install(0, 1, 5);
  f.backend.Complete(1);
  f.Poll();
  ASSERT_EQ(RuleState::kFailed, f.done.back().state);
  EXPECT_EQ(EOPNOTSUPP, f.done.back().error);
  EXPECT_EQ(RuleState::kFailed, f.owner.StateOf(r.handle)) << "kept until the application removes it";
  const size_t free_before = f.owner.free_marks();
  ASSERT_TRUE(f.owner.Remove(r.handle));
  EXPECT_FALSE(f.owner.StateOf(r.handle).has_value());
  EXPECT_EQ(free_before + 1, f.owner.free_marks()) << "the device never used that MARK";
  EXPECT_EQ(0u, f.owner.draining_marks());

  f.backend.RefuseNextSubmit(ENOSPC);
  const auto refused = f.owner.Install(0, 2, 6);
  EXPECT_EQ(InstallError::kRefused, refused.status);
  EXPECT_EQ(ENOSPC, refused.error);
  EXPECT_EQ(0u, f.owner.outstanding());
  EXPECT_EQ(0u, f.owner.live_rules());

  FlowCapabilities none;
  f.backend.SetCapabilities(2, none);
  EXPECT_EQ(InstallError::kUnsupported, f.owner.Install(2, 3, 7).status);
}

TEST(FlowRuleOwnerTest, OutstandingRequestsAreBoundedAndRemovalsQueue) {
  Owner::Config c;
  c.max_outstanding = 2;
  Fixture f(c);
  const auto a = f.owner.Install(0, 1, 1);
  const auto b = f.owner.Install(0, 2, 2);
  EXPECT_EQ(InstallError::kBackpressure, f.owner.Install(0, 3, 3).status);
  f.backend.Complete(1);
  f.Poll();
  const auto d = f.owner.Install(0, 4, 4);
  ASSERT_EQ(InstallError::kOk, d.status) << "room after a completion";
  // Removing an installed rule while the window is full waits, then goes.
  ASSERT_TRUE(f.owner.Remove(a.handle));
  EXPECT_EQ(RuleState::kInstalled, f.owner.StateOf(a.handle));
  EXPECT_EQ(0u, f.owner.RetryPendingRemovals());
  f.backend.Complete(2);
  f.Poll();  // b and d installed
  EXPECT_EQ(1u, f.owner.RetryPendingRemovals());
  EXPECT_EQ(RuleState::kRemoving, f.owner.StateOf(a.handle));
  // Removing a rule still being installed removes it once installed.
  const auto e = f.owner.Install(1, 5, 5);
  ASSERT_TRUE(f.owner.Remove(e.handle));
  f.backend.Complete(1);
  f.Poll();  // a removed
  f.backend.Complete(1);
  f.Poll();  // e installed, then its removal starts
  EXPECT_EQ(RuleState::kRemoving, f.owner.StateOf(e.handle));
  f.backend.Complete(1);
  f.Poll();
  EXPECT_FALSE(f.owner.StateOf(e.handle).has_value());
  EXPECT_EQ(2u, f.backend.installed(0)) << "b and d";
  (void)b;
}

// A MARK value is never handed to a new rule while packets marked by the old
// one can still arrive: only after the removal completed and the port's queues
// were drained once more.
TEST(FlowRuleOwnerTest, MarksAreReusedOnlyAfterRemovalAndADrain) {
  Owner::Config c;
  c.marks = 2;
  Fixture f(c);
  f.backend.set_auto_complete(true);
  const auto a = f.owner.Install(0, 1, 10);
  const auto b = f.owner.Install(0, 2, 20);
  f.Poll();
  EXPECT_NE(a.mark, b.mark);
  EXPECT_EQ(InstallError::kNoMark, f.owner.Install(0, 3, 30).status);
  ASSERT_TRUE(f.owner.Remove(a.handle));
  f.Poll();
  EXPECT_FALSE(f.owner.MarkCookie(a.mark).has_value());
  EXPECT_EQ(1u, f.owner.draining_marks());
  EXPECT_EQ(InstallError::kNoMark, f.owner.Install(0, 3, 30).status) << "removed, not drained";
  f.owner.NoteDrained(1);
  EXPECT_EQ(InstallError::kNoMark, f.owner.Install(0, 3, 30).status) << "another port's drain";
  f.owner.NoteDrained(0);
  const auto again = f.owner.Install(0, 3, 30);
  ASSERT_EQ(InstallError::kOk, again.status);
  EXPECT_EQ(a.mark, again.mark);
  f.Poll();
  EXPECT_EQ(30u, f.owner.MarkCookie(a.mark).value());
  EXPECT_EQ(20u, f.owner.MarkCookie(b.mark).value());
  EXPECT_EQ((std::set<uint32_t>{a.mark, b.mark}), f.backend.marks(0)) << "the device saw the owner's marks";
}

TEST(FlowRuleOwnerTest, DeviceResetMakesRulesUnknownUntilReconciled) {
  Fixture f;
  const auto installed = f.owner.Install(0, 1, 1);
  const auto other_port = f.owner.Install(1, 2, 2);
  f.backend.Complete(2);
  f.Poll();
  const auto in_flight = f.owner.Install(0, 3, 3);
  EXPECT_EQ(1u, f.owner.outstanding());
  f.backend.Reset(0);
  f.owner.OnDeviceReset(0);
  EXPECT_EQ(RuleState::kUnknown, f.owner.StateOf(installed.handle));
  EXPECT_EQ(RuleState::kUnknown, f.owner.StateOf(in_flight.handle));
  EXPECT_EQ(RuleState::kInstalled, f.owner.StateOf(other_port.handle));
  EXPECT_EQ(0u, f.owner.outstanding()) << "requests lost with the device";
  EXPECT_FALSE(f.owner.MarkCookie(installed.mark).has_value());
  std::vector<uint64_t> kept;
  EXPECT_EQ(2u, f.owner.Reconcile(0, [&](FlowRuleHandle, uint64_t cookie) {
    kept.push_back(cookie);
    return cookie == 1;
  }));
  EXPECT_EQ((std::vector<uint64_t>{1, 3}), kept);
  EXPECT_FALSE(f.owner.StateOf(installed.handle).has_value());
  EXPECT_EQ(2u, f.owner.draining_marks()) << "packets marked before the reset may be in flight";
  // A handle from before the reset never names the rule in its slot later:
  // both retired slots are reused, under new generations.
  const auto fresh = f.owner.Install(0, 4, 4);
  const auto fresh2 = f.owner.Install(0, 5, 5);
  EXPECT_TRUE((fresh.handle.index == installed.handle.index) ||
              (fresh2.handle.index == installed.handle.index))
      << "the slot is reused";
  EXPECT_FALSE(f.owner.StateOf(installed.handle).has_value());
  EXPECT_FALSE(f.owner.StateOf(in_flight.handle).has_value());
  EXPECT_TRUE(f.owner.StateOf(fresh.handle).has_value());
  EXPECT_TRUE(f.owner.StateOf(fresh2.handle).has_value());
  EXPECT_FALSE(f.owner.Remove(installed.handle)) << "a stale handle removes nothing";
  EXPECT_EQ(1u, f.owner.RemoveAll(1));
}

// A completion that the device delivers after the owner was told of a reset
// (an asynchronous queue drained late) changes nothing.
TEST(FlowRuleOwnerTest, LateCompletionsAfterAResetAreIgnored) {
  Fixture f;
  const auto r = f.owner.Install(0, 1, 1);
  f.owner.OnDeviceReset(0);  // the backend still holds the request
  EXPECT_EQ(0u, f.owner.outstanding());
  f.backend.Complete(1);
  f.Poll();
  EXPECT_TRUE(f.done.empty());
  EXPECT_EQ(0u, f.owner.outstanding());
  EXPECT_EQ(RuleState::kUnknown, f.owner.StateOf(r.handle));
  EXPECT_FALSE(f.owner.MarkCookie(r.mark).has_value());
}

TEST(FlowRuleOwnerTest, RteFlowBackendReportsNoPortWithoutADevice) {
  RteFlowBackend backend;
  EXPECT_FALSE(backend.Capabilities(0).supported) << "no ethdev is probed in unit tests";
  FlowRuleOwner<RteFlowBackend> owner(backend, {});
  RteFlowBackend::Rule rule;
  rule.pattern = {{RTE_FLOW_ITEM_TYPE_END, nullptr, nullptr, nullptr}};
  rule.actions = {{RTE_FLOW_ACTION_TYPE_MARK, nullptr}, {RTE_FLOW_ACTION_TYPE_END, nullptr}};
  EXPECT_EQ(InstallError::kUnsupported, owner.Install(0, rule, 1).status);
  int error = 0;
  EXPECT_FALSE(backend.Validate(0, rule, error));
  EXPECT_NE(0, error);
}

// Only `supported` gates an install: the other capabilities are hints, and
// the device's answer for the rule decides. MARK values must fit 32 bits.
TEST(FlowRuleOwnerTest, OnlySupportGatesInstallAndMarksMustFit32Bits) {
  FakeFlowBackend backend;
  FlowCapabilities hints_off = Nic();
  hints_off.mark_action = false;
  hints_off.mark_bits = 8;
  hints_off.count_action = false;
  backend.SetCapabilities(0, hints_off);
  backend.SetCapabilities(1, FlowCapabilities{});  // no rules at all
  Owner owner(backend, Owner::Config{.mark_base = 1, .marks = 4096});
  EXPECT_EQ(InstallError::kOk, owner.Install(0, 1, 1).status) << "hints do not refuse";
  EXPECT_EQ(InstallError::kUnsupported, owner.Install(1, 1, 1).status);
  backend.RefuseNextSubmit(-95);
  const auto refused = owner.Install(0, 2, 2);
  EXPECT_EQ(InstallError::kRefused, refused.status) << "the device decides";
  EXPECT_EQ(-95, refused.error);

  backend.SetCapabilities(2, Nic());
  Owner top(backend, Owner::Config{.mark_base = 0xffffff00u, .marks = 0x100});  // ..0xffffffff
  const auto last = top.Install(2, 1, 1);
  EXPECT_EQ(InstallError::kOk, last.status);
  Owner past(backend, Owner::Config{.mark_base = 0xffffff00u, .marks = 0x101});  // would wrap
  EXPECT_EQ(InstallError::kUnsupported, past.Install(2, 1, 1).status);
  Owner unmarked(backend, Owner::Config{.mark_base = 0, .marks = 0});  // no MARK values at all
  EXPECT_EQ(InstallError::kOk, unmarked.Install(2, 1, 1).status);
}

// A batch installs in order and stops at the first rule not submitted.
TEST(FlowRuleOwnerTest, InstallBatchStopsAtTheFirstRuleNotSubmitted) {
  Fixture f(Owner::Config{.max_outstanding = 3, .marks = 8});
  const std::vector<uint64_t> rules = {1, 2, 3, 4, 5};
  const std::vector<uint64_t> cookies = {10, 20, 30, 40, 50};
  std::vector<InstallResult> out(5);
  EXPECT_EQ(3u, f.owner.InstallBatch(0, rules, cookies, out));
  EXPECT_EQ(InstallError::kBackpressure, out[3].status);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(InstallError::kOk, out[i].status);
    EXPECT_EQ(RuleState::kSubmitted, f.owner.StateOf(out[i].handle));
  }
  f.backend.Complete(3);
  f.Poll();
  ASSERT_EQ(3u, f.done.size());
  EXPECT_EQ(10u, f.done[0].cookie);
  EXPECT_EQ(30u, f.done[2].cookie);
  EXPECT_EQ(2u, f.owner.InstallBatch(0, std::span(rules).subspan(3), std::span(cookies).subspan(3),
                                     std::span(out).subspan(3)));
}

// A removal the device refuses or fails leaves the rule installed, with its
// MARK still mapped and never reused, so a later removal can still succeed.
TEST(FlowRuleOwnerTest, RefusedOrFailedRemovalsKeepTheRuleAndItsMark) {
  Fixture f(Owner::Config{.max_outstanding = 8, .marks = 2});
  const auto r = f.owner.Install(0, 1, 11);
  f.backend.Complete(1);
  f.Poll();
  f.done.clear();

  f.backend.RefuseNextRemove(-16);
  EXPECT_FALSE(f.owner.Remove(r.handle));
  EXPECT_EQ(RuleState::kInstalled, f.owner.StateOf(r.handle));
  EXPECT_EQ(11u, f.owner.MarkCookie(r.mark)) << "the rule still marks packets";
  EXPECT_EQ(0u, f.owner.outstanding());

  ASSERT_TRUE(f.owner.Remove(r.handle));
  f.backend.FailNextRemove(-5);
  f.backend.Complete(1);
  ASSERT_EQ(1u, f.Poll());
  EXPECT_EQ(RuleState::kInstalled, f.done[0].state);
  EXPECT_EQ(-5, f.done[0].error);
  EXPECT_EQ(RuleState::kInstalled, f.owner.StateOf(r.handle));
  EXPECT_EQ(11u, f.owner.MarkCookie(r.mark));
  EXPECT_EQ(0u, f.owner.outstanding());
  f.owner.NoteDrained(0);
  // The other MARK is the only one free: the kept rule's is not reused.
  const auto other = f.owner.Install(0, 2, 22);
  EXPECT_NE(r.mark, other.mark);
  EXPECT_EQ(InstallError::kNoMark, f.owner.Install(0, 3, 33).status);

  // A deferred removal the device refuses is reported by the next Poll.
  f.backend.Complete(1);
  f.Poll();
  f.done.clear();
  Fixture g(Owner::Config{.max_outstanding = 1, .marks = 4});
  const auto a = g.owner.Install(0, 1, 1);
  g.backend.Complete(1);
  g.Poll();
  const auto b = g.owner.Install(0, 2, 2);  // fills the window
  ASSERT_TRUE(g.owner.Remove(a.handle)) << "queued";
  g.backend.Complete(1);
  g.Poll();
  g.done.clear();
  g.backend.RefuseNextRemove(-16);
  EXPECT_EQ(0u, g.owner.RetryPendingRemovals());
  ASSERT_EQ(1u, g.Poll());
  EXPECT_EQ(a.handle, g.done[0].handle);
  EXPECT_EQ(RuleState::kInstalled, g.done[0].state);
  EXPECT_EQ(-16, g.done[0].error);
  EXPECT_EQ(RuleState::kInstalled, g.owner.StateOf(a.handle));
  (void)b;

  ASSERT_TRUE(f.owner.Remove(r.handle));
  f.backend.Complete(1);
  f.Poll();
  EXPECT_EQ(RuleState::kRemoved, f.done.back().state);
}

// The application may act on a rule from its completion callback; the owner
// does not then act on it a second time.
TEST(FlowRuleOwnerTest, CallbacksMayRemoveTheRuleTheyAreToldAbout) {
  FakeFlowBackend backend;
  backend.SetCapabilities(0, Nic());
  Owner owner(backend, Owner::Config{.max_outstanding = 8, .marks = 4});
  const auto r = owner.Install(0, 1, 1);
  ASSERT_TRUE(owner.Remove(r.handle)) << "removed when installed";
  backend.Complete(1);
  size_t removes = 0;
  owner.Poll([&](const Completion &c) {
    if (c.state == RuleState::kInstalled) {
      removes += owner.Remove(c.handle) ? 1 : 0;
    }
  });
  EXPECT_EQ(1u, removes);
  EXPECT_EQ(RuleState::kRemoving, owner.StateOf(r.handle));
  EXPECT_EQ(1u, owner.outstanding()) << "one removal in flight, not two";
  EXPECT_EQ(1u, backend.pending()) << "one request at the device";
  backend.Complete(8);
  std::vector<Completion> done;
  owner.Poll([&](const Completion &c) { done.push_back(c); });
  ASSERT_EQ(1u, done.size());
  EXPECT_EQ(RuleState::kRemoved, done[0].state);
  EXPECT_EQ(0u, owner.outstanding());
}

}  // namespace
}  // namespace bess::offload
