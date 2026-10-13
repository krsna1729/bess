// SPDX-License-Identifier: BSD-3-Clause

// WorkerSplit's "<name>/settings" singleton (D-129) in process (the harness
// runs as worker 0).

#include <gtest/gtest.h>

#include <cerrno>
#include <initializer_list>
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

bess::pb::WorkerSplitArg Map(std::initializer_list<std::pair<uint32_t, uint32_t>> m) {
  bess::pb::WorkerSplitArg arg;
  for (const auto &[wid, gate] : m) {
    (*arg.mutable_worker_gates())[wid] = gate;
  }
  return arg;
}

TEST(WorkerSplitSettingsTest, ResourceMovesWorkerZero) {
  ModuleHarness h;
  Module *m = h.Create("WorkerSplit", bess::pb::WorkerSplitArg(), "ws");
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.SetResource("ws/settings", Map({{0, 3}})));
  auto out = h.Run(m, 0, {Packet()}, {0, 3});
  EXPECT_EQ(0u, out[0].size());
  EXPECT_EQ(1u, out[3].size());

  // Worker 0 unmapped: dropped.
  ASSERT_EQ("", h.SetResource("ws/settings", Map({{1, 0}})));
  out = h.Run(m, 0, {Packet()}, {0, 3});
  EXPECT_EQ(0u, out[0].size());
  EXPECT_EQ(0u, out[3].size());

  // Empty: back to worker x -> gate x.
  ASSERT_EQ("", h.SetResource("ws/settings", bess::pb::WorkerSplitArg()));
  out = h.Run(m, 0, {Packet()}, {0, 3});
  EXPECT_EQ(1u, out[0].size());
}

TEST(WorkerSplitSettingsTest, ReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("WorkerSplit", bess::pb::WorkerSplitArg(), "ws"));
  const bess::pb::WorkerSplitArg initial;
  EXPECT_EQ("", h.ReadBackDiffers("ws/settings", {{nullptr, &initial}}));
  const bess::pb::WorkerSplitArg s = Map({{0, 2}, {3, 1}});
  ASSERT_EQ("", h.SetResource("ws/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("ws/settings", {{nullptr, &s}}));
}

TEST(WorkerSplitSettingsTest, BadValuesRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("WorkerSplit", bess::pb::WorkerSplitArg(), "ws"));
  EXPECT_NE("", h.SetResource("ws/settings", Map({{0, MAX_GATES}})));
  EXPECT_NE("", h.SetResource("ws/settings", Map({{Worker::kMaxWorkers, 0}})));
  const bess::pb::WorkerSplitArg unchanged;
  EXPECT_EQ("", h.ReadBackDiffers("ws/settings", {{nullptr, &unchanged}}));
}

TEST(WorkerSplitSettingsTest, RequiresPausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("WorkerSplit", bess::pb::WorkerSplitArg(), "ws"));
  EXPECT_TRUE(RequiresPausedWorkers("ws/settings"));
}

TEST(WorkerSplitSettingsTest, ResetIsLegacyOnly) {
  ModuleHarness h;
  Module *m = h.Create("WorkerSplit", bess::pb::WorkerSplitArg(), "ws");
  ASSERT_NE(nullptr, m);
  const bess::pb::WorkerSplitArg s = Map({{0, 1}});
  EXPECT_EQ(ENOTSUP, h.Action(m, "reset", s).error().code());
  h.MustCommand(m, "reset", s);
  EXPECT_EQ("", h.ReadBackDiffers("ws/settings", {{nullptr, &s}}));
}

}  // namespace
