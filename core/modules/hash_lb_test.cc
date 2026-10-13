// SPDX-License-Identifier: BSD-3-Clause

// HashLB in process (was bessctl/module_tests/hash_lb.py): every packet
// leaves on one of the configured gates, a wire gate that does not fit
// gate_idx_t is refused (not narrowed), and a refused command changes
// nothing.

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::TcpPacket;

std::vector<Bytes> Flows(int n = 64) {
  std::mt19937 rng(7);
  std::vector<Bytes> pkts;
  for (int i = 0; i < n; i++) {
    pkts.push_back(TcpPacket({.sip = "10.0." + std::to_string(i / 250) + "." +
                                     std::to_string(i % 250 + 1),
                              .dip = "10.1.0.1",
                              .sport = 1000 + i,
                              .dport = 80},
                             rng));
  }
  return pkts;
}

bess::pb::HashLBCommandSetGatesArg Gates(std::initializer_list<int64_t> gates) {
  bess::pb::HashLBCommandSetGatesArg arg;
  for (int64_t g : gates) {
    arg.add_gates(g);
  }
  return arg;
}

bess::pb::HashLBCommandSetModeArg Mode(const std::string &mode) {
  bess::pb::HashLBCommandSetModeArg arg;
  arg.set_mode(mode);
  return arg;
}

const std::vector<gate_idx_t> kGates = {0, 1, 2, 3, 4, 5, 6, 7};

// The gates that got packets; every packet must come out unchanged on one.
std::set<gate_idx_t> GatesUsed(const std::map<gate_idx_t, std::vector<Bytes>> &out,
                               const std::vector<Bytes> &in) {
  std::set<gate_idx_t> used;
  std::multiset<Bytes> seen;
  for (const auto &[g, pkts] : out) {
    if (!pkts.empty()) {
      used.insert(g);
    }
    seen.insert(pkts.begin(), pkts.end());
  }
  EXPECT_EQ(std::multiset<Bytes>(in.begin(), in.end()), seen) << "every packet, once";
  return used;
}

TEST(HashLbTest, NoGatesIsRefusedAtCreation) {
  ModuleHarness h;
  EXPECT_EQ(EINVAL, h.CreateFailure("HashLB", bess::pb::HashLBArg()).code());
}

TEST(HashLbTest, GatesAndModes) {
  const std::vector<Bytes> pkts = Flows();
  ModuleHarness h;
  bess::pb::HashLBArg arg;
  arg.add_gates(1);
  arg.add_gates(2);
  Module *lb = h.Create("HashLB", arg);
  ASSERT_NE(nullptr, lb);
  EXPECT_EQ((std::set<gate_idx_t>{1, 2}), GatesUsed(h.Run(lb, 0, pkts, kGates), pkts))
      << "all flows on the configured gates";

  h.MustCommand(lb, "set_gates", Gates({3, 4, 5}));
  const auto spread = h.Run(lb, 0, pkts, kGates);
  EXPECT_EQ((std::set<gate_idx_t>{3, 4, 5}), GatesUsed(spread, pkts));

  // 65536 would narrow to gate 0; it must be refused, and a bad gate later
  // in the list must not leave a half-written list behind.
  EXPECT_EQ(EINVAL, h.Command(lb, "set_gates", Gates({65536})).error().code());
  EXPECT_EQ(EINVAL, h.Command(lb, "set_gates", Gates({6, 7, 70000})).error().code());
  // No destinations: every packet would index an empty gate list.
  EXPECT_EQ(EINVAL, h.Command(lb, "set_gates", Gates({})).error().code());
  EXPECT_EQ(spread, h.Run(lb, 0, pkts, kGates)) << "refused commands change nothing";

  h.MustCommand(lb, "set_mode", Mode("l3"));
  const auto l3 = h.Run(lb, 0, pkts, kGates);
  const auto used = GatesUsed(l3, pkts);
  const std::set<gate_idx_t> allowed = {3, 4, 5};
  EXPECT_TRUE(std::includes(allowed.begin(), allowed.end(), used.begin(), used.end()));
  EXPECT_EQ(EINVAL, h.Command(lb, "set_mode", Mode("bogus")).error().code());
  EXPECT_EQ(l3, h.Run(lb, 0, pkts, kGates)) << "a refused mode changes nothing";
}

// -- "<name>/settings" (D-129) ---------------------------------------------------

bess::pb::HashLBSettings LbSettings(const std::string &mode,
                                    std::initializer_list<int64_t> gates) {
  bess::pb::HashLBSettings s;
  s.set_mode(mode);
  for (int64_t g : gates) {
    s.add_gates(g);
  }
  return s;
}

bess::pb::HashLBArg LbArg(std::initializer_list<int64_t> gates) {
  bess::pb::HashLBArg arg;
  for (int64_t g : gates) {
    arg.add_gates(g);
  }
  return arg;
}

// True when "<resource>"'s codec says applying it requires paused workers.
bool RequiresPausedWorkers(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  EXPECT_NE(nullptr, r) << resource;
  if (r == nullptr) {
    return false;
  }
  const bess::framework::ResourceCodec *codec =
      bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  EXPECT_NE(nullptr, codec) << resource;
  return codec != nullptr && codec->requires_paused_workers();
}

TEST(HashLbSettingsTest, GatesAndModeDecideTheOutputGate) {
  const std::vector<Bytes> pkts = Flows();
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);

  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l4", {3, 4, 5})));
  EXPECT_EQ((std::set<gate_idx_t>{3, 4, 5}), GatesUsed(h.Run(lb, 0, pkts, kGates), pkts));

  // Every flow has the same destination: a custom tuple over the
  // destination address alone sends them all to one gate.
  bess::pb::HashLBSettings dst_only = LbSettings("", {3, 4, 5});
  bess::pb::Field *field = dst_only.add_fields();
  field->set_offset(30);  // IPv4 destination
  field->set_num_bytes(4);
  ASSERT_EQ("", h.SetResource("lb/settings", dst_only));
  EXPECT_EQ(1u, GatesUsed(h.Run(lb, 0, pkts, kGates), pkts).size());

  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l3", {6})));
  EXPECT_EQ((std::set<gate_idx_t>{6}), GatesUsed(h.Run(lb, 0, pkts, kGates), pkts));
}

TEST(HashLbSettingsTest, ReadBackRoundTrips) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("HashLB", LbArg({1, 2}), "lb"));
  const bess::pb::HashLBSettings initial = LbSettings("l4", {1, 2});
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &initial}}))
      << "no mode given: l4";
  const bess::pb::HashLBSettings s = LbSettings("l2", {0, 7, 9});
  ASSERT_EQ("", h.SetResource("lb/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &s}}));

  bess::pb::HashLBSettings fields = LbSettings("", {2});
  bess::pb::Field *f = fields.add_fields();
  f->set_offset(26);
  f->set_num_bytes(8);
  ASSERT_EQ("", h.SetResource("lb/settings", fields));
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &fields}}));
}

TEST(HashLbSettingsTest, BadValuesRefused) {
  const std::vector<Bytes> pkts = Flows();
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  const auto before = h.Run(lb, 0, pkts, kGates);
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("l4", {})));
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("l4", {65536})))
      << "not narrowed to gate 0";
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("l4", {3, 70000})));
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("bogus", {3})));
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("", {3})));
  const bess::pb::HashLBSettings unchanged = LbSettings("l4", {1, 2});
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &unchanged}}));
  EXPECT_EQ(before, h.Run(lb, 0, pkts, kGates)) << "refused values change nothing";
}

TEST(HashLbSettingsTest, AppliesLive) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("HashLB", LbArg({1, 2}), "lb"));
  EXPECT_FALSE(RequiresPausedWorkers("lb/settings"));
}

TEST(HashLbSettingsTest, SettersAreLegacyOnly) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  EXPECT_EQ(ENOTSUP, h.Action(lb, "set_gates", Gates({3})).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(lb, "set_mode", Mode("l2")).error().code());
  // The legacy commands still run, and the resource reads what they set.
  h.MustCommand(lb, "set_gates", Gates({3}));
  h.MustCommand(lb, "set_mode", Mode("l2"));
  const bess::pb::HashLBSettings s = LbSettings("l2", {3});
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &s}}));
}

}  // namespace
