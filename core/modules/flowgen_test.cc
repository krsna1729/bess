// SPDX-License-Identifier: BSD-3-Clause

// FlowGen's "<name>/settings" singleton (D-129) in process: what `update` and
// `set_burst` set, through the resource's codec, changes the packets the
// task generates; it reads back canonically (every field named), unset fields
// keep their value as for `update`, what they refuse is refused, it requires
// paused workers, and `update` and `set_burst` went with the legacy service
// (D-140).

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
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
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

// An IPv4 UDP or TCP packet of `len` bytes. Its first byte is not 0: update
// (and so the settings) take an empty template as "unchanged" only while the
// current one does not start with a 0 byte.
std::string Template(bool tcp, size_t len) {
  bess::testing::PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4("10.0.0.1", "10.0.0.2");
  if (tcp) {
    b.Tcp(1000, 2000);
  } else {
    b.Udp(1000, 2000);
  }
  const Bytes bytes = b.PadTo(len).Build();
  return std::string(bytes.begin(), bytes.end());
}

// A new flow every nanosecond: every task run has a full burst due.
constexpr double kPps = 1e12;
constexpr double kFlowRate = 1e9;

bess::pb::FlowGenArg Arg() {
  bess::pb::FlowGenArg arg;
  arg.set_template_(Template(false, 60));
  arg.set_pps(kPps);
  arg.set_flow_rate(kFlowRate);
  arg.set_flow_duration(1);
  return arg;
}

bess::pb::FlowGenSettings Settings(const std::string &tmpl, uint64_t burst) {
  bess::pb::FlowGenSettings s;
  s.set_template_(tmpl);
  s.set_pps(kPps);
  s.set_flow_rate(kFlowRate);
  s.set_flow_duration(1);
  s.set_arrival("uniform");
  s.set_duration("uniform");
  s.set_burst(burst);
  return s;
}

TEST(FlowGenSettingsTest, TemplateAndBurstChangeWhatTheTaskGenerates) {
  ModuleHarness h;
  Module *fg = h.Create("FlowGen", Arg(), "fg");
  ASSERT_NE(nullptr, fg);
  auto out = h.RunTask(fg, {0});
  ASSERT_EQ(bess::PacketBatch::kMaxBurst, out[0].size());
  EXPECT_EQ(60u, out[0][0].size());

  ASSERT_EQ("", h.SetResource("fg/settings", Settings(Template(false, 100), 4)));
  out = h.RunTask(fg, {0});
  ASSERT_EQ(4u, out[0].size());
  for (const Bytes &pkt : out[0]) {
    EXPECT_EQ(100u, pkt.size());
  }
}

TEST(FlowGenSettingsTest, ReadBackRoundTrips) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("FlowGen", Arg(), "fg"));
  const bess::pb::FlowGenSettings initial =
      Settings(Template(false, 60), bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("fg/settings", {{nullptr, &initial}}));

  bess::pb::FlowGenSettings s = Settings(Template(false, 80), 9);
  s.set_pps(5000);
  s.set_flow_rate(50);
  s.set_flow_duration(2.5);
  s.set_arrival("exponential");
  s.set_duration("pareto");
  ASSERT_EQ("", h.SetResource("fg/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("fg/settings", {{nullptr, &s}}));

  // As for update, an unset field keeps its value; the read-back names it.
  bess::pb::FlowGenSettings burst_only;
  burst_only.set_burst(0);
  ASSERT_EQ("", h.SetResource("fg/settings", burst_only));
  bess::pb::FlowGenSettings expected = s;
  expected.set_burst(0);
  EXPECT_EQ("", h.ReadBackDiffers("fg/settings", {{nullptr, &expected}}));
}

TEST(FlowGenSettingsTest, BadValuesRefused) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("FlowGen", Arg(), "fg"));
  const std::string udp = Template(false, 60);
  EXPECT_NE("", h.SetResource("fg/settings", Settings(Template(true, 60), 32)))
      << "UDP -> TCP: 'template' can not be updated";
  EXPECT_NE("", h.SetResource("fg/settings", Settings(std::string(60, '\0'), 32)))
      << "not IPv4";
  EXPECT_NE("", h.SetResource("fg/settings", Settings(std::string(2000, '\0'), 32)))
      << "too big";
  EXPECT_NE("", h.SetResource("fg/settings", Settings(udp, 33)));
  bess::pb::FlowGenSettings bad = Settings(udp, 32);
  bad.set_pps(-1);
  EXPECT_NE("", h.SetResource("fg/settings", bad));
  bad = Settings(udp, 32);
  bad.set_pps(10);
  bad.set_flow_rate(100);
  EXPECT_NE("", h.SetResource("fg/settings", bad)) << "flow rate above pps";
  bad = Settings(udp, 32);
  bad.set_arrival("poisson");
  EXPECT_NE("", h.SetResource("fg/settings", bad));
  bad = Settings(udp, 32);
  bad.set_duration("forever");
  EXPECT_NE("", h.SetResource("fg/settings", bad));
  const bess::pb::FlowGenSettings unchanged = Settings(udp, bess::PacketBatch::kMaxBurst);
  EXPECT_EQ("", h.ReadBackDiffers("fg/settings", {{nullptr, &unchanged}}));
}

TEST(FlowGenSettingsTest, RequiresPausedWorkers) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("FlowGen", Arg(), "fg"));
  EXPECT_TRUE(RequiresPausedWorkers("fg/settings"));
}

}  // namespace
