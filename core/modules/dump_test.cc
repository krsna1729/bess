// SPDX-License-Identifier: BSD-3-Clause

// Dump's "<name>/settings" singleton (D-129) in process: the sampling
// interval, applied while workers run.

#include <gtest/gtest.h>

#include <cerrno>
#include <cmath>
#include <random>
#include <string>

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

Bytes Packet() {
  std::mt19937 rng(1);
  return bess::testing::TcpPacket(FlowSpec{}, rng);
}

bess::pb::DumpArg Interval(double sec) {
  bess::pb::DumpArg arg;
  arg.set_interval(sec);
  return arg;
}

TEST(DumpSettingsTest, ResourceSetsTheIntervalAndForwards) {
  ModuleHarness h;
  Module *m = h.Create("Dump", bess::pb::DumpArg(), "d");
  ASSERT_NE(nullptr, m);
  const bess::pb::DumpArg initial = Interval(1.0);
  EXPECT_EQ("", h.ReadBackDiffers("d/settings", {{nullptr, &initial}}));
  const bess::pb::DumpArg s = Interval(0.25);
  ASSERT_EQ("", h.SetResource("d/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("d/settings", {{nullptr, &s}}));
  const Bytes in = Packet();
  auto out = h.Run(m, 0, {in}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(in, out[0][0]);
}

TEST(DumpSettingsTest, BadIntervalsRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Dump", bess::pb::DumpArg(), "d"));
  EXPECT_NE("", h.SetResource("d/settings", Interval(0)));
  EXPECT_NE("", h.SetResource("d/settings", Interval(-1)));
  EXPECT_NE("", h.SetResource("d/settings", Interval(std::nan(""))));
}

// The interval is one relaxed atomic: it applies while workers run.
TEST(DumpSettingsTest, DoesNotRequirePausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Dump", bess::pb::DumpArg(), "d"));
  EXPECT_FALSE(RequiresPausedWorkers("d/settings"));
}

}  // namespace
