// SPDX-License-Identifier: BSD-3-Clause

// MPLSPop's "<name>/settings" singleton (D-129) in process.

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

// An MPLS packet: Packet() with ether type 0x8847 and one label after the
// Ethernet header.
Bytes MplsPacket() {
  Bytes pkt = Packet();
  pkt[12] = 0x88;
  pkt[13] = 0x47;
  const uint8_t label[4] = {0x00, 0x01, 0x01, 0x40};
  pkt.insert(pkt.begin() + 14, label, label + 4);
  return pkt;
}

bess::pb::MplsPopArg Settings(bool remove, uint32_t type) {
  bess::pb::MplsPopArg arg;
  arg.set_remove_eth_header(remove);
  arg.set_next_eth_type(type);
  return arg;
}

TEST(MplsPopSettingsTest, ResourceChangesThePop) {
  ModuleHarness h;
  Module *m = h.Create("MPLSPop", bess::pb::EmptyArg(), "mp");
  ASSERT_NE(nullptr, m);
  const Bytes in = MplsPacket();

  ASSERT_EQ("", h.SetResource("mp/settings", Settings(false, 0x86dd)));
  auto out = h.Run(m, 0, {in}, {0, 1});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(in.size() - 4, out[0][0].size());
  EXPECT_EQ(0x86, out[0][0][12]);
  EXPECT_EQ(0xdd, out[0][0][13]);

  ASSERT_EQ("", h.SetResource("mp/settings", Settings(true, 0x0800)));
  out = h.Run(m, 0, {in}, {0, 1});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(in.size() - 18, out[0][0].size());
}

TEST(MplsPopSettingsTest, ReadBackMatches) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("MPLSPop", bess::pb::EmptyArg(), "mp"));
  const bess::pb::MplsPopArg initial = Settings(false, 0x0800);
  EXPECT_EQ("", h.ReadBackDiffers("mp/settings", {{nullptr, &initial}}));
  const bess::pb::MplsPopArg s = Settings(true, 0x86dd);
  ASSERT_EQ("", h.SetResource("mp/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("mp/settings", {{nullptr, &s}}));
}

TEST(MplsPopSettingsTest, WideEtherTypeRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("MPLSPop", bess::pb::EmptyArg(), "mp"));
  EXPECT_NE("", h.SetResource("mp/settings", Settings(false, 0x10000)));
}

TEST(MplsPopSettingsTest, RequiresPausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("MPLSPop", bess::pb::EmptyArg(), "mp"));
  EXPECT_TRUE(RequiresPausedWorkers("mp/settings"));
}

}  // namespace
