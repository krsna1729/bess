// SPDX-License-Identifier: BSD-3-Clause

// Queue's "<name>/settings" singleton (D-129) in process: size, burst,
// prefetch and backpressure set through the resource's codec, read back, and
// the setters it supersedes refused as actions.

#include <gtest/gtest.h>

#include <cerrno>
#include <random>
#include <optional>
#include <string>
#include <string_view>
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

TEST(QueueObservationTest, OverflowDrainAndResizeKeepLifetimeCounts) {
  auto observation = [](std::string_view name) -> std::optional<double> {
    for (const auto &sample : bess::runtime::runtime().metrics().Collect()) {
      if (sample.name != "bess_module_observations_total") {
        continue;
      }
      bool module_matches = false;
      bool observation_matches = false;
      for (const auto &[key, value] : sample.labels) {
        module_matches = module_matches || (key == "module" && value == "q");
        observation_matches =
            observation_matches || (key == "observation" && value == name);
      }
      if (module_matches && observation_matches) {
        EXPECT_EQ(bess::stats::MetricKind::kCounter, sample.kind);
        return sample.value;
      }
    }
    return std::nullopt;
  };
  {
    ModuleHarness h;
    bess::pb::QueueArg arg;
    arg.set_size(4);  // rte_ring holds size - 1 packets
    Module *q = h.Create("Queue", arg, "q");
    ASSERT_NE(nullptr, q);
    const auto packets = Packets(5);
    h.Run(q, 0, packets, {});
    EXPECT_EQ(3.0, observation("enqueued_packets"));
    EXPECT_EQ(2.0, observation("queue_full_packets"));
    EXPECT_EQ(0.0, observation("dequeued_packets"));
    const std::vector<Bytes> accepted(packets.begin(), packets.begin() + 3);
    EXPECT_EQ(accepted, h.RunTask(q, {0})[0]);
    EXPECT_EQ(3.0, observation("dequeued_packets"));
    ASSERT_EQ("", h.SetResource("q/settings", Settings(8, 4)));
    EXPECT_EQ(3.0, observation("enqueued_packets"));
    EXPECT_EQ(2.0, observation("queue_full_packets"));
    EXPECT_EQ(3.0, observation("dequeued_packets"));
  }
  EXPECT_EQ(std::nullopt, observation("queue_full_packets"));
}

}  // namespace
