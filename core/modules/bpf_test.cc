// SPDX-License-Identifier: BSD-3-Clause

// BPF in process (was bessctl/module_tests/bpf.py): a packet leaves on the
// gate of the highest-priority filter it matches, unmatched ones on gate 0.
// D-129: "<name>/filters" holds the whole filter list; its commands went
// with the legacy service (D-140).

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
using bess::testing::ModuleHarness;
using bess::testing::TcpPacket;
using bess::testing::UdpPacket;

bess::pb::BPFArg Filter(int64_t priority, const std::string &filter,
                        int64_t gate) {
  bess::pb::BPFArg arg;
  auto *f = arg.add_filters();
  f->set_priority(priority);
  f->set_filter(filter);
  f->set_gate(gate);
  return arg;
}

TEST(BpfTest, SingleRuleSteersMatchesAndPassesTheRestOnGate0) {
  std::mt19937 rng(1);
  ModuleHarness h;
  Module *bpf = h.Create("BPF", bess::pb::BPFArg());
  ASSERT_NE(nullptr, bpf);
  EXPECT_EQ("", h.SetResource(std::string(bpf->name()) + "/filters",
                              Filter(0, "tcp src port 92", 1)));

  const Bytes udp =
      UdpPacket({.sip = "12.34.56.78", .dip = "12.34.56.78"}, rng);
  const Bytes tcp92 =
      TcpPacket({.sip = "12.34.56.78", .dip = "12.34.56.78", .sport = 92}, rng);

  auto out = h.Run(bpf, 0, {udp, tcp92}, {0, 1});
  EXPECT_EQ((std::vector<Bytes>{udp}), out[0]);
  EXPECT_EQ((std::vector<Bytes>{tcp92}), out[1]);
}

TEST(BpfTest, HighestPriorityMatchWins) {
  std::mt19937 rng(2);
  ModuleHarness h;
  Module *bpf = h.Create("BPF", bess::pb::BPFArg());
  ASSERT_NE(nullptr, bpf);
  bess::pb::BPFArg two = Filter(2, "tcp src port 92", 1);
  two.MergeFrom(Filter(1, "ip host 22.22.22.22", 2));
  EXPECT_EQ("", h.SetResource(std::string(bpf->name()) + "/filters", two));

  // Only the host filter.
  const Bytes host = UdpPacket(
      {.sip = "22.22.22.22", .dip = "12.34.56.78", .sport = 700}, rng);
  // Both: the port filter has the higher priority.
  const Bytes both =
      TcpPacket({.sip = "12.34.56.78", .dip = "22.22.22.22", .sport = 92}, rng);
  // Neither.
  const Bytes neither = TcpPacket(
      {.sip = "12.34.56.78", .dip = "12.34.56.78", .sport = 700}, rng);

  auto out = h.Run(bpf, 0, {neither, both, host}, {0, 1, 2});
  EXPECT_EQ((std::vector<Bytes>{neither}), out[0]);
  EXPECT_EQ((std::vector<Bytes>{both}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{host}), out[2]);
}

// Whether the codec bound to `resource` requires paused workers.
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

// "<name>/filters" is the whole filter list (paused workers): setting it
// replaces the list, it reads back as given (not in priority order), a list
// with a filter that does not compile is refused whole, and an empty list
// sends everything to gate 0.
TEST(BpfTest, FiltersResourceReplacesTheList) {
  std::mt19937 rng(3);
  const Bytes host = UdpPacket(
      {.sip = "22.22.22.22", .dip = "12.34.56.78", .sport = 700}, rng);
  const Bytes both =
      TcpPacket({.sip = "12.34.56.78", .dip = "22.22.22.22", .sport = 92}, rng);
  const Bytes neither = TcpPacket(
      {.sip = "12.34.56.78", .dip = "12.34.56.78", .sport = 700}, rng);

  ModuleHarness h;
  const auto initial = Filter(0, "tcp src port 700", 3);
  Module *bpf = h.Create("BPF", initial, "bpf_res");
  ASSERT_NE(nullptr, bpf);
  const std::string filters = "bpf_res/filters";
  EXPECT_TRUE(RequiresPausedWorkers(filters));
  EXPECT_EQ("", h.ReadBackDiffers(filters, {{nullptr, &initial}}))
      << "Init's filters";

  // Lower priority first: the list reads back in this order.
  bess::pb::BPFArg two = Filter(1, "ip host 22.22.22.22", 2);
  two.MergeFrom(Filter(2, "tcp src port 92", 1));
  EXPECT_EQ("", h.SetResource(filters, two));
  auto out = h.Run(bpf, 0, {neither, both, host}, {0, 1, 2, 3});
  EXPECT_EQ((std::vector<Bytes>{neither}), out[0])
      << "the Init filter was replaced";
  EXPECT_EQ((std::vector<Bytes>{both}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{host}), out[2]);
  EXPECT_EQ("", h.ReadBackDiffers(filters, {{nullptr, &two}}));

  bess::pb::BPFArg bad = Filter(5, "udp", 1);
  bad.MergeFrom(Filter(6, "not a filter (", 1));
  EXPECT_NE("", h.SetResource(filters, bad));
  EXPECT_NE("", h.SetResource(filters, Filter(5, "udp", MAX_GATES)));
  EXPECT_EQ("", h.ReadBackDiffers(filters, {{nullptr, &two}}))
      << "refused sets change nothing";
  EXPECT_EQ((std::vector<Bytes>{host}), h.Run(bpf, 0, {host}, {2})[2]);

  const bess::pb::BPFArg none;
  EXPECT_EQ("", h.SetResource(filters, none));
  out = h.Run(bpf, 0, {neither, both, host}, {0, 1, 2, 3});
  EXPECT_EQ((std::vector<Bytes>{neither, both, host}), out[0]);
  EXPECT_EQ("", h.ReadBackDiffers(filters, {{nullptr, &none}}));
}
TEST(BpfTest, DefaultGateSteersUnmatchedTraffic) {
  ModuleHarness h;
  bess::pb::BPFArg arg;
  auto *f = arg.add_filters();
  f->set_priority(1);
  f->set_filter("tcp src port 92");
  f->set_gate(1);
  arg.set_default_gate(4);

  Module *bpf = h.Create("BPF", arg);
  ASSERT_NE(nullptr, bpf);

  const Bytes udp = bess::testing::PacketBuilder()
                        .Ether("02:00:00:00:00:02", "02:00:00:00:00:01", 0x0800)
                        .IPv4("12.34.56.78", "12.34.56.78")
                        .Udp(100, 200)
                        .Build();
  const Bytes tcp92 =
      bess::testing::PacketBuilder()
          .Ether("02:00:00:00:00:02", "02:00:00:00:00:01", 0x0800)
          .IPv4("12.34.56.78", "12.34.56.78")
          .Tcp(92, 200)
          .Build();

  auto out = h.Run(bpf, 0, {udp, tcp92}, {1, 4});
  EXPECT_EQ((std::vector<Bytes>{tcp92}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{udp}), out[4]);

  // Invalid default gate rejected
  bess::pb::BPFArg bad;
  bad.set_default_gate(MAX_GATES);
  EXPECT_NE("", h.SetResource(std::string(bpf->name()) + "/filters", bad));

  // Resource update updates default_gate dynamically
  bess::pb::BPFArg updated;
  auto *uf = updated.add_filters();
  uf->set_priority(1);
  uf->set_filter("tcp src port 92");
  uf->set_gate(1);
  updated.set_default_gate(3);
  EXPECT_EQ("", h.SetResource(std::string(bpf->name()) + "/filters", updated));
  out = h.Run(bpf, 0, {udp, tcp92}, {1, 3});
  EXPECT_EQ((std::vector<Bytes>{tcp92}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{udp}), out[3]);

  // Empty filter list with default gate 3 routes to default gate 3
  bess::pb::BPFArg empty;
  empty.set_default_gate(3);
  EXPECT_EQ("", h.SetResource(std::string(bpf->name()) + "/filters", empty));
  out = h.Run(bpf, 0, {udp, tcp92}, {3});
  EXPECT_EQ((std::vector<Bytes>{udp, tcp92}), out[3]);
}

TEST(BpfTest, Fast16BitFilterSteersAndGuardsShortPackets) {
  ModuleHarness h;
  bess::pb::BPFArg arg;
  auto *f = arg.add_filters();
  f->set_priority(1);
  f->set_filter("arp");
  f->set_gate(1);
  arg.set_default_gate(2);

  Module *bpf = h.Create("BPF", arg);
  ASSERT_NE(nullptr, bpf);

  const Bytes arp = bess::testing::PacketBuilder()
                        .Ether("02:00:00:00:00:02", "02:00:00:00:00:01", 0x0806)
                        .Payload(std::string(28, '\0'))
                        .Build();
  const Bytes ip = bess::testing::PacketBuilder()
                       .Ether("02:00:00:00:00:02", "02:00:00:00:00:01", 0x0800)
                       .IPv4("1.2.3.4", "5.6.7.8")
                       .Udp(100, 200)
                       .Build();
  const Bytes truncated = {0x02, 0x00, 0x00, 0x00,
                           0x00, 0x01};  // 6 bytes, shorter than EtherType

  auto out = h.Run(bpf, 0, {arp, ip, truncated}, {1, 2});
  EXPECT_EQ((std::vector<Bytes>{arp}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{ip, truncated}), out[2]);
}

TEST(BpfTest, Fast32BitFilterSteersExactMatches) {
  ModuleHarness h;
  bess::pb::BPFArg arg;
  auto *f = arg.add_filters();
  f->set_priority(1);
  f->set_filter("ip host 1.2.3.4");
  f->set_gate(1);
  arg.set_default_gate(2);

  Module *bpf = h.Create("BPF", arg);
  ASSERT_NE(nullptr, bpf);

  const Bytes match =
      bess::testing::PacketBuilder()
          .Ether("02:00:00:00:00:02", "02:00:00:00:00:01", 0x0800)
          .IPv4("1.2.3.4", "5.6.7.8")
          .Udp(100, 200)
          .Build();
  const Bytes miss =
      bess::testing::PacketBuilder()
          .Ether("02:00:00:00:00:02", "02:00:00:00:00:01", 0x0800)
          .IPv4("9.9.9.9", "5.6.7.8")
          .Udp(100, 200)
          .Build();

  auto out = h.Run(bpf, 0, {match, miss}, {1, 2});
  EXPECT_EQ((std::vector<Bytes>{match}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{miss}), out[2]);
}

TEST(BpfTest, BurstOf32MixedPacketsSteeredCorrectly) {
  ModuleHarness h;
  bess::pb::BPFArg arg;
  auto *f = arg.add_filters();
  f->set_priority(1);
  f->set_filter("arp");
  f->set_gate(1);
  arg.set_default_gate(2);

  Module *bpf = h.Create("BPF", arg);
  ASSERT_NE(nullptr, bpf);

  const Bytes arp = bess::testing::PacketBuilder()
                        .Ether("02:00:00:00:00:02", "02:00:00:00:00:01", 0x0806)
                        .Payload(std::string(28, '\0'))
                        .Build();
  const Bytes ip = bess::testing::PacketBuilder()
                       .Ether("02:00:00:00:00:02", "02:00:00:00:00:01", 0x0800)
                       .IPv4("1.2.3.4", "5.6.7.8")
                       .Udp(100, 200)
                       .Build();

  std::vector<Bytes> burst;
  burst.reserve(32);
  std::vector<Bytes> want_gate1;
  std::vector<Bytes> want_gate2;

  for (size_t i = 0; i < 32; i++) {
    if (i % 2 == 0) {
      burst.push_back(arp);
      want_gate1.push_back(arp);
    } else {
      burst.push_back(ip);
      want_gate2.push_back(ip);
    }
  }

  auto out = h.Run(bpf, 0, burst, {1, 2});
  EXPECT_EQ(want_gate1, out[1]);
  EXPECT_EQ(want_gate2, out[2]);
}

}  // namespace
