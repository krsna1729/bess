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

bess::pb::BPFArg Filter(int64_t priority, const std::string &filter, int64_t gate) {
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

  const Bytes udp = UdpPacket({.sip = "12.34.56.78", .dip = "12.34.56.78"}, rng);
  const Bytes tcp92 = TcpPacket({.sip = "12.34.56.78", .dip = "12.34.56.78", .sport = 92}, rng);

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
  const Bytes host = UdpPacket({.sip = "22.22.22.22", .dip = "12.34.56.78", .sport = 700}, rng);
  // Both: the port filter has the higher priority.
  const Bytes both = TcpPacket({.sip = "12.34.56.78", .dip = "22.22.22.22", .sport = 92}, rng);
  // Neither.
  const Bytes neither =
      TcpPacket({.sip = "12.34.56.78", .dip = "12.34.56.78", .sport = 700}, rng);

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
  const Bytes host = UdpPacket({.sip = "22.22.22.22", .dip = "12.34.56.78", .sport = 700}, rng);
  const Bytes both = TcpPacket({.sip = "12.34.56.78", .dip = "22.22.22.22", .sport = 92}, rng);
  const Bytes neither =
      TcpPacket({.sip = "12.34.56.78", .dip = "12.34.56.78", .sport = 700}, rng);

  ModuleHarness h;
  const auto initial = Filter(0, "tcp src port 700", 3);
  Module *bpf = h.Create("BPF", initial, "bpf_res");
  ASSERT_NE(nullptr, bpf);
  const std::string filters = "bpf_res/filters";
  EXPECT_TRUE(RequiresPausedWorkers(filters));
  EXPECT_EQ("", h.ReadBackDiffers(filters, {{nullptr, &initial}})) << "Init's filters";

  // Lower priority first: the list reads back in this order.
  bess::pb::BPFArg two = Filter(1, "ip host 22.22.22.22", 2);
  two.MergeFrom(Filter(2, "tcp src port 92", 1));
  EXPECT_EQ("", h.SetResource(filters, two));
  auto out = h.Run(bpf, 0, {neither, both, host}, {0, 1, 2, 3});
  EXPECT_EQ((std::vector<Bytes>{neither}), out[0]) << "the Init filter was replaced";
  EXPECT_EQ((std::vector<Bytes>{both}), out[1]);
  EXPECT_EQ((std::vector<Bytes>{host}), out[2]);
  EXPECT_EQ("", h.ReadBackDiffers(filters, {{nullptr, &two}}));

  bess::pb::BPFArg bad = Filter(5, "udp", 1);
  bad.MergeFrom(Filter(6, "not a filter (", 1));
  EXPECT_NE("", h.SetResource(filters, bad));
  EXPECT_NE("", h.SetResource(filters, Filter(5, "udp", MAX_GATES)));
  EXPECT_EQ("", h.ReadBackDiffers(filters, {{nullptr, &two}})) << "refused sets change nothing";
  EXPECT_EQ((std::vector<Bytes>{host}), h.Run(bpf, 0, {host}, {2})[2]);

  const bess::pb::BPFArg none;
  EXPECT_EQ("", h.SetResource(filters, none));
  out = h.Run(bpf, 0, {neither, both, host}, {0, 1, 2, 3});
  EXPECT_EQ((std::vector<Bytes>{neither, both, host}), out[0]);
  EXPECT_EQ("", h.ReadBackDiffers(filters, {{nullptr, &none}}));
}

}  // namespace
