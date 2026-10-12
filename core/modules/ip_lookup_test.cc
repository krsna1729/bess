// SPDX-License-Identifier: BSD-3-Clause

// IPLookup in process (was bessctl/module_tests/iplookup.py): longest-prefix
// routes to gates, deleted and unknown routes, prefixes with host bits, and
// wire gates that do not fit gate_idx_t.

#include <gtest/gtest.h>

#include <cerrno>
#include <random>
#include <string>
#include <vector>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::TcpPacket;

bess::pb::IPLookupCommandAddArg Route(const std::string &prefix, uint64_t len, uint64_t gate) {
  bess::pb::IPLookupCommandAddArg arg;
  arg.set_prefix(prefix);
  arg.set_prefix_len(len);
  arg.set_gate(gate);
  return arg;
}

bess::pb::IPLookupCommandDeleteArg Prefix(const std::string &prefix, uint64_t len) {
  bess::pb::IPLookupCommandDeleteArg arg;
  arg.set_prefix(prefix);
  arg.set_prefix_len(len);
  return arg;
}

TEST(IpLookupTest, RoutesToTheGateOfTheMatchingPrefix) {
  std::mt19937 rng(1);
  const Bytes to22 = TcpPacket({.sip = "12.22.22.22", .dip = "22.22.22.22"}, rng);
  const Bytes to32 = TcpPacket({.sip = "12.22.22.22", .dip = "32.22.22.22"}, rng);
  const Bytes to42 = TcpPacket({.sip = "12.22.22.22", .dip = "42.22.22.22"}, rng);

  ModuleHarness h;
  Module *ipl = h.Create("IPLookup", bess::pb::IPLookupArg());
  ASSERT_NE(nullptr, ipl);
  h.MustCommand(ipl, "add", Route("22.22.22.0", 24, 0));
  h.MustCommand(ipl, "add", Route("32.22.22.0", 24, 1));
  h.MustCommand(ipl, "add", Route("42.22.22.0", 24, 1));
  h.MustCommand(ipl, "delete", Prefix("42.22.22.0", 24));
  EXPECT_NE(0, h.Command(ipl, "delete", Prefix("52.22.22.0", 24)).error().code())
      << "deleting a route that is not there";

  auto out = h.Run(ipl, 0, {to22, to32, to42}, {0, 1});
  EXPECT_EQ((std::vector<Bytes>{to22}), out[0]);
  EXPECT_EQ((std::vector<Bytes>{to32}), out[1]) << "the deleted route forwards nothing";
}

TEST(IpLookupTest, PrefixWithHostBitsIsRefused) {
  ModuleHarness h;
  Module *ipl = h.Create("IPLookup", bess::pb::IPLookupArg());
  ASSERT_NE(nullptr, ipl);
  EXPECT_EQ(EINVAL, h.Command(ipl, "add", Route("22.22.22.0", 16, 0)).error().code());
}

// The gate is uint64 on the wire; 65536 + g used to route to gate g.
TEST(IpLookupTest, GateDoesNotWrap) {
  std::mt19937 rng(2);
  const Bytes pkt = TcpPacket({.sip = "12.0.0.1", .dip = "10.1.2.3"}, rng);
  ModuleHarness h;
  Module *ipl = h.Create("IPLookup", bess::pb::IPLookupArg());
  ASSERT_NE(nullptr, ipl);
  EXPECT_EQ(EINVAL, h.Command(ipl, "add", Route("10.0.0.0", 8, 65536 + 1)).error().code());
  EXPECT_EQ(EINVAL, h.Command(ipl, "add", Route("10.0.0.0", 8, 1ull << 32)).error().code());
  EXPECT_TRUE(h.Run(ipl, 0, {pkt}, {0, 1})[1].empty()) << "a refused route is not installed";
  h.MustCommand(ipl, "add", Route("10.0.0.0", 8, 1));
  EXPECT_EQ((std::vector<Bytes>{pkt}), h.Run(ipl, 0, {pkt}, {0, 1})[1]);
}

}  // namespace
