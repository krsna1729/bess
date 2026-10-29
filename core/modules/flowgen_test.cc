// SPDX-License-Identifier: BSD-3-Clause

// FlowGen's "<name>/settings" singleton (D-129) in process: what `update` and
// `set_burst` set, through the resource's codec, changes the packets the
// task generates; it reads back canonically (every field named), unset fields
// keep their value as for `update`, what they refuse is refused, it requires
// paused workers, and `update` and `set_burst` went with the legacy service
// (D-140). A template that does not hold the headers each packet is written
// and summed through, or whose lengths do not fit it, is refused.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "packet_parse.h"
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

// -- the template: refused at Init unless it holds what each packet needs -----

// `tmpl` with byte `at` set to `v`.
std::string With(std::string tmpl, size_t at, uint8_t v) {
  tmpl.at(at) = static_cast<char>(v);
  return tmpl;
}

// `tmpl` with the 16-bit big-endian field at `at` set to `v`.
std::string With16(std::string tmpl, size_t at, uint16_t v) {
  tmpl.at(at) = static_cast<char>(unsigned{v} >> 8u);
  tmpl.at(at + 1) = static_cast<char>(unsigned{v} & 0xffu);
  return tmpl;
}

// Init's refusal of `tmpl` ("" when it accepted it).
std::string InitError(ModuleHarness &h, const std::string &tmpl) {
  bess::pb::FlowGenArg arg = Arg();
  arg.set_template_(tmpl);
  return h.CreateError("FlowGen", arg);
}

// The template holds whole the Ethernet, IPv4 and UDP or TCP headers each
// packet is written and summed through: one cut anywhere short of them is
// refused, at Init and on an update. The old code accepted every UDP one
// (reading the EtherType and protocol from a template as short as 24 bytes)
// and wrote ports past the end of each.
TEST(FlowGenTemplateTest, ATemplateShorterThanItsHeadersIsRefused) {
  ModuleHarness h;
  const std::string udp = Template(false, 42);
  for (size_t len : {14u, 24u, 33u, 34u, 41u}) {
    EXPECT_NE("", InitError(h, udp.substr(0, len))) << len << " bytes";
  }
  EXPECT_EQ("", InitError(h, udp));
  ASSERT_NE(nullptr, h.Create("FlowGen", Arg(), "fg"));
  EXPECT_NE("", h.SetResource("fg/settings", Settings(udp.substr(0, 41), 32)));
  EXPECT_EQ("", h.SetResource("fg/settings", Settings(udp, 32)));
}

// The IPv4 header must be one: version 4, an IHL of 5 or more, and all of
// it in the template before the UDP header. The old code accepted all
// three, and with IHL 15 wrote the ports 40 bytes past the end of the
// 42-byte template.
TEST(FlowGenTemplateTest, AnIpv4HeaderThatDoesNotFitIsRefused) {
  ModuleHarness h;
  const std::string udp = Template(false, 60);
  EXPECT_NE("", InitError(h, With(udp, 14, 0x4f))) << "IHL 15 (60 bytes)";
  EXPECT_NE("", InitError(h, With(udp, 14, 0x44))) << "IHL 4";
  EXPECT_NE("", InitError(h, With(udp, 14, 0x65))) << "version 6";
  EXPECT_NE("", InitError(h, With(Template(false, 42), 14, 0x46)))
      << "IHL 6: the UDP header ends past the 42-byte template";

  bess::testing::PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("10.0.0.1", "10.0.0.2");
  b.Last<bess::testing::PacketBuilder::IPv4Hdr>().options = {1, 1, 1, 0};
  const Bytes options = b.Udp(1000, 2000).PadTo(64).Build();
  EXPECT_EQ("", InitError(h, std::string(options.begin(), options.end())))
      << "IPv4 options are allowed in a UDP template";
}

// The checksums read what the lengths say: the IPv4 total length must fit
// the template (padding may follow it) and cover the headers, and the UDP
// length must fit the IPv4 payload. The old code accepted each, and summed
// up to 64 KiB past the template.
TEST(FlowGenTemplateTest, LengthsThatDoNotFitAreRefused) {
  ModuleHarness h;
  const std::string udp = Template(false, 60);  // IPv4 length 46, UDP 26
  EXPECT_NE("", InitError(h, With16(udp, 16, 47))) << "IPv4 length past the template";
  EXPECT_NE("", InitError(h, With16(udp, 16, 65535)));
  EXPECT_NE("", InitError(h, With16(udp, 16, 27))) << "IPv4 length short of the UDP header";
  EXPECT_NE("", InitError(h, With16(udp, 38, 27))) << "UDP length past the IPv4 payload";
  EXPECT_NE("", InitError(h, With16(udp, 38, 65535)));
  EXPECT_NE("", InitError(h, With16(udp, 38, 7))) << "UDP length short of its header";
  EXPECT_EQ("", InitError(h, With16(With16(udp, 16, 28), 38, 8)))
      << "a datagram padded to the template";
  const std::string tcp = Template(true, 60);  // IPv4 length 46
  EXPECT_NE("", InitError(h, With16(tcp, 16, 47))) << "TCP: the IPv4 length sets the checksum's span";
  EXPECT_NE("", InitError(h, With16(tcp, 16, 39)));
  EXPECT_EQ("", InitError(h, tcp));
}

// FlowGen's SYN and FIN are 60-byte frames carrying a 40-byte IPv4
// datagram: a TCP template with IPv4 or TCP options cannot be one and is
// refused, as is one under 60 bytes. The old code accepted both kinds of
// options and sent SYNs and FINs whose lengths contradict their headers.
TEST(FlowGenTemplateTest, ATcpTemplateWithOptionsIsRefused) {
  ModuleHarness h;
  const std::string tcp = Template(true, 64);
  EXPECT_EQ("", InitError(h, tcp));
  EXPECT_NE("", InitError(h, Template(true, 59)));
  EXPECT_NE("", InitError(h, With(tcp, 46, 0x60))) << "TCP data offset 6";
  bess::testing::PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("10.0.0.1", "10.0.0.2");
  b.Last<bess::testing::PacketBuilder::IPv4Hdr>().options = {1, 1, 1, 0};
  const Bytes options = b.Tcp(1000, 2000).PadTo(64).Build();
  EXPECT_NE("", InitError(h, std::string(options.begin(), options.end()))) << "IPv4 options";
}

// The packets are exactly the template's size, and the checked parse finds
// each whole, a UDP template with IPv4 options and a TCP template's SYNs
// included.
TEST(FlowGenTemplateTest, PacketsAreWholeAndTheTemplateSize) {
  ModuleHarness h;
  bess::testing::PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("10.0.0.1", "10.0.0.2");
  b.Last<bess::testing::PacketBuilder::IPv4Hdr>().options = {1, 1, 1, 0};
  const Bytes options = b.Udp(1000, 2000).PadTo(70).Build();
  bess::pb::FlowGenArg arg = Arg();
  arg.set_template_(std::string(options.begin(), options.end()));
  Module *udp = h.Create("FlowGen", arg, "fg_udp");
  ASSERT_NE(nullptr, udp);
  arg.set_template_(Template(true, 80));
  Module *tcp = h.Create("FlowGen", arg, "fg_tcp");
  ASSERT_NE(nullptr, tcp);
  for (Module *m : {udp, tcp}) {
    const auto out = h.RunTask(m, {0});
    ASSERT_EQ(1u, out.count(0));
    ASSERT_FALSE(out.at(0).empty());
    for (const Bytes &pkt : out.at(0)) {
      EXPECT_EQ(m == udp ? 70u : 60u, pkt.size()) << "a new flow starts with a SYN";
      bess::packet::ParsedFrame parsed;
      EXPECT_EQ(bess::packet::ParseStatus::kOk,
                bess::packet::ParseFrame(std::span(pkt.data(), pkt.size()), parsed, pkt.size()));
    }
  }
}

}  // namespace
