// SPDX-License-Identifier: BSD-3-Clause

// Source's "<name>/settings" singleton (D-129) in process: packet size and
// burst set through the resource's codec change what the task emits, read
// back canonically, refuse what the setters refuse, and the setters it
// supersedes are legacy only.

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "pktbatch.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"

namespace {

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

bess::pb::SourceSettings Settings(uint64_t pkt_size, uint64_t burst) {
  bess::pb::SourceSettings s;
  s.set_pkt_size(pkt_size);
  s.set_burst(burst);
  return s;
}

TEST(SourceSettingsTest, SizeAndBurstChangeWhatTheTaskEmits) {
  ModuleHarness h;
  Module *src = h.Create("Source", bess::pb::SourceArg(), "src");
  ASSERT_NE(nullptr, src);
  auto out = h.RunTask(src, {0});
  ASSERT_EQ(bess::PacketBatch::kMaxBurst, out[0].size());
  EXPECT_EQ(60u, out[0][0].size());

  ASSERT_EQ("", h.SetResource("src/settings", Settings(100, 5)));
  out = h.RunTask(src, {0});
  ASSERT_EQ(5u, out[0].size());
  for (const auto &pkt : out[0]) {
    EXPECT_EQ(100u, pkt.size());
  }
}

TEST(SourceSettingsTest, ReadBackRoundTrips) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Source", bess::pb::SourceArg(), "src"));
  const bess::pb::SourceSettings initial = Settings(60, bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("src/settings", {{nullptr, &initial}}));
  const bess::pb::SourceSettings s = Settings(128, 7);
  ASSERT_EQ("", h.SetResource("src/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("src/settings", {{nullptr, &s}}));
  // burst 0 is valid (and the proto3 default: it reads back omitted).
  const bess::pb::SourceSettings zero = Settings(64, 0);
  ASSERT_EQ("", h.SetResource("src/settings", zero));
  EXPECT_EQ("", h.ReadBackDiffers("src/settings", {{nullptr, &zero}}));
}

TEST(SourceSettingsTest, BadValuesRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Source", bess::pb::SourceArg(), "src"));
  EXPECT_NE("", h.SetResource("src/settings", Settings(0, 32)));
  EXPECT_NE("", h.SetResource("src/settings", Settings(uint64_t{1} << 20, 32)));
  EXPECT_NE("", h.SetResource("src/settings", Settings(60, 33)));
  const bess::pb::SourceSettings unchanged = Settings(60, bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("src/settings", {{nullptr, &unchanged}}));
}

TEST(SourceSettingsTest, AppliesLive) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("Source", bess::pb::SourceArg(), "src"));
  EXPECT_FALSE(RequiresPausedWorkers("src/settings"));
}

TEST(SourceSettingsTest, SettersAreLegacyOnly) {
  ModuleHarness h;
  Module *src = h.Create("Source", bess::pb::SourceArg(), "src");
  ASSERT_NE(nullptr, src);
  bess::pb::SourceCommandSetPktSizeArg size;
  size.set_pkt_size(200);
  bess::pb::SourceCommandSetBurstArg burst;
  burst.set_burst(3);
  EXPECT_EQ(ENOTSUP, h.Action(src, "set_pkt_size", size).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(src, "set_burst", burst).error().code());
  // The legacy commands still run, validate as before, and the resource
  // reads what they set.
  h.MustCommand(src, "set_pkt_size", size);
  h.MustCommand(src, "set_burst", burst);
  bess::pb::SourceCommandSetBurstArg too_big;
  too_big.set_burst(33);
  EXPECT_EQ(EINVAL, h.Command(src, "set_burst", too_big).error().code());
  const bess::pb::SourceSettings s = Settings(200, 3);
  EXPECT_EQ("", h.ReadBackDiffers("src/settings", {{nullptr, &s}}));
}

}  // namespace
