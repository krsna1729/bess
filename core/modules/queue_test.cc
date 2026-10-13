// SPDX-License-Identifier: BSD-3-Clause

// Queue's "<name>/settings" singleton (D-129) in process: size, burst,
// prefetch and backpressure set through the resource's codec, read back, and
// the setters it supersedes refused as actions.

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

std::vector<Bytes> Packets(int n) {
  std::mt19937 rng(1);
  std::vector<Bytes> out;
  for (int i = 0; i < n; i++) {
    out.push_back(bess::testing::TcpPacket(FlowSpec{}, rng));
  }
  return out;
}

bess::pb::QueueSettings Settings(uint64_t size, uint64_t burst) {
  bess::pb::QueueSettings s;
  s.set_size(size);
  s.set_burst(burst);
  return s;
}

uint64_t Status(ModuleHarness &h, Module *q, uint64_t *size) {
  CommandResponse r = h.Action(q, "get_status", bess::pb::QueueCommandGetStatusArg());
  EXPECT_EQ(0, r.error().code()) << r.error().errmsg();
  bess::pb::QueueCommandGetStatusResponse status;
  EXPECT_TRUE(r.data().UnpackTo(&status));
  *size = status.size();
  return status.count();
}

// The burst bounds a release; a resize keeps the queued packets.
TEST(QueueSettingsTest, ResourceSetsBurstAndSize) {
  ModuleHarness h;
  Module *q = h.Create("Queue", bess::pb::QueueArg(), "q");
  ASSERT_NE(nullptr, q);
  h.Run(q, 0, Packets(5), {});

  ASSERT_EQ("", h.SetResource("q/settings", Settings(64, 2)));
  uint64_t size = 0;
  EXPECT_EQ(5u, Status(h, q, &size));
  EXPECT_EQ(64u, size);
  EXPECT_EQ(2u, h.RunTask(q, {0})[0].size());
  EXPECT_EQ(3u, Status(h, q, &size));
}

TEST(QueueSettingsTest, ReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Queue", bess::pb::QueueArg(), "q"));
  bess::pb::QueueSettings s = Settings(256, 7);
  s.set_prefetch(true);
  s.set_backpressure(true);
  ASSERT_EQ("", h.SetResource("q/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("q/settings", {{nullptr, &s}}));
}

// Defaults read back canonically: size 1024, burst 32.
TEST(QueueSettingsTest, ReadsDefaults) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Queue", bess::pb::QueueArg(), "q"));
  const bess::pb::QueueSettings s = Settings(1024, bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("q/settings", {{nullptr, &s}}));
}

TEST(QueueSettingsTest, BadValuesRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Queue", bess::pb::QueueArg(), "q"));
  EXPECT_NE("", h.SetResource("q/settings", Settings(0, 32)));
  EXPECT_NE("", h.SetResource("q/settings", Settings(100, 32)));
  EXPECT_NE("", h.SetResource("q/settings", Settings(32768, 32)));
  EXPECT_NE("", h.SetResource("q/settings", Settings(64, 33)));
  const bess::pb::QueueSettings unchanged = Settings(1024, bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("q/settings", {{nullptr, &unchanged}}));
}

TEST(QueueSettingsTest, RequiresPausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Queue", bess::pb::QueueArg(), "q"));
  EXPECT_TRUE(RequiresPausedWorkers("q/settings"));
}

TEST(QueueSettingsTest, SettersAreLegacyOnly) {
  ModuleHarness h;
  Module *q = h.Create("Queue", bess::pb::QueueArg(), "q");
  ASSERT_NE(nullptr, q);
  bess::pb::QueueCommandSetSizeArg size;
  size.set_size(64);
  EXPECT_EQ(ENOTSUP, h.Action(q, "set_size", size).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(q, "set_burst", bess::pb::QueueCommandSetBurstArg()).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(q, "set_runtime_config", bess::pb::QueueArg()).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(q, "get_runtime_config", bess::pb::EmptyArg()).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(q, "get_initial_arg", bess::pb::EmptyArg()).error().code());
  // The legacy command still runs, and the resource reads what it set.
  h.MustCommand(q, "set_size", size);
  const bess::pb::QueueSettings s = Settings(64, bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("q/settings", {{nullptr, &s}}));
}

}  // namespace
