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

#include "pb/module_msg.pb.h"
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

}  // namespace
