// SPDX-License-Identifier: BSD-3-Clause

// VLANPush's "<name>/settings" singleton (D-129) in process.

#include <gtest/gtest.h>

#include <cerrno>
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

bess::pb::VLANPushArg Tci(uint64_t tci) {
  bess::pb::VLANPushArg arg;
  arg.set_tci(tci);
  return arg;
}

TEST(VLANPushSettingsTest, ResourceSetsTheTag) {
  ModuleHarness h;
  Module *m = h.Create("VLANPush", Tci(1), "vp");
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.SetResource("vp/settings", Tci(0x2abc)));
  const Bytes in = Packet();
  auto out = h.Run(m, 0, {in}, {0});
  ASSERT_EQ(1u, out[0].size());
  const Bytes &pkt = out[0][0];
  ASSERT_EQ(in.size() + 4, pkt.size());
  EXPECT_EQ(0x81, pkt[12]);
  EXPECT_EQ(0x00, pkt[13]);
  EXPECT_EQ(0x2a, pkt[14]);
  EXPECT_EQ(0xbc, pkt[15]);
}

TEST(VLANPushSettingsTest, ReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("VLANPush", Tci(5), "vp"));
  const bess::pb::VLANPushArg initial = Tci(5);
  EXPECT_EQ("", h.ReadBackDiffers("vp/settings", {{nullptr, &initial}}));
  const bess::pb::VLANPushArg s = Tci(0xffff);
  ASSERT_EQ("", h.SetResource("vp/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("vp/settings", {{nullptr, &s}}));
}

TEST(VLANPushSettingsTest, WideTciRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("VLANPush", Tci(5), "vp"));
  EXPECT_NE("", h.SetResource("vp/settings", Tci(0x10000)));
}

TEST(VLANPushSettingsTest, RequiresPausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("VLANPush", Tci(5), "vp"));
  EXPECT_TRUE(RequiresPausedWorkers("vp/settings"));
}

}  // namespace
