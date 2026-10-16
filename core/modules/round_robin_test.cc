// SPDX-License-Identifier: BSD-3-Clause

// RoundRobin's "<name>/settings" singleton (D-129) in process.

#include <gtest/gtest.h>

#include <cerrno>
#include <random>
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
using bess::testing::FlowSpec;
using bess::testing::ModuleHarness;

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

bess::pb::RoundRobinArg Settings(const std::vector<int64_t> &gates, const std::string &mode) {
  bess::pb::RoundRobinArg arg;
  for (int64_t g : gates) {
    arg.add_gates(g);
  }
  arg.set_mode(mode);
  return arg;
}

std::vector<Bytes> Packets(int n) {
  std::mt19937 rng(1);
  std::vector<Bytes> out;
  for (int i = 0; i < n; i++) {
    out.push_back(bess::testing::TcpPacket(FlowSpec{}, rng));
  }
  return out;
}

TEST(RoundRobinSettingsTest, ResourceChangesGatesAndMode) {
  ModuleHarness h;
  Module *rr = h.Create("RoundRobin", Settings({0}, "batch"), "rr");
  ASSERT_NE(nullptr, rr);
  ASSERT_EQ("", h.SetResource("rr/settings", Settings({2, 3}, "packet")));
  auto out = h.Run(rr, 0, Packets(4), {0, 2, 3});
  EXPECT_EQ(0u, out[0].size());
  EXPECT_EQ(2u, out[2].size());
  EXPECT_EQ(2u, out[3].size());
}

TEST(RoundRobinSettingsTest, ReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("RoundRobin", Settings({0}, ""), "rr"));
  const bess::pb::RoundRobinArg initial = Settings({0}, "batch");
  EXPECT_EQ("", h.ReadBackDiffers("rr/settings", {{nullptr, &initial}}));
  const bess::pb::RoundRobinArg s = Settings({1, 4, 2}, "packet");
  ASSERT_EQ("", h.SetResource("rr/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("rr/settings", {{nullptr, &s}}));
}

TEST(RoundRobinSettingsTest, BadValuesRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("RoundRobin", Settings({0}, "batch"), "rr"));
  EXPECT_NE("", h.SetResource("rr/settings", Settings({0}, "")));
  EXPECT_NE("", h.SetResource("rr/settings", Settings({0}, "flow")));
  EXPECT_NE("", h.SetResource("rr/settings", Settings({-1}, "packet")));
  EXPECT_NE("", h.SetResource("rr/settings", Settings({int64_t{1} << 16}, "packet")));
  EXPECT_NE("", h.SetResource("rr/settings", Settings({MAX_GATES + 1}, "packet")));
}

TEST(RoundRobinSettingsTest, RequiresPausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("RoundRobin", Settings({0}, "batch"), "rr"));
  EXPECT_TRUE(RequiresPausedWorkers("rr/settings"));
}

}  // namespace
