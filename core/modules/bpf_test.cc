// SPDX-License-Identifier: BSD-3-Clause

// BPF in process (was bessctl/module_tests/bpf.py): a packet leaves on the
// gate of the highest-priority filter it matches, unmatched ones on gate 0.

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "pb/module_msg.pb.h"
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
  h.MustCommand(bpf, "add", Filter(0, "tcp src port 92", 1));

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
  h.MustCommand(bpf, "add", Filter(2, "tcp src port 92", 1));
  h.MustCommand(bpf, "add", Filter(1, "ip host 22.22.22.22", 2));

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

}  // namespace
