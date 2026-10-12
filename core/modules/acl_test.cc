// SPDX-License-Identifier: BSD-3-Clause

// ACL in process (was bessctl/module_tests/acl.py): first match wins,
// unmatched traffic is dropped, and a refused command (a wire port that
// would narrow, a malformed prefix) changes nothing, all or nothing.

#include <gtest/gtest.h>

#include <cerrno>
#include <random>
#include <string>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::ModuleHarness;
using bess::testing::TcpPacket;

struct Rule {
  std::string src_ip, dst_ip;
  uint32_t src_port = 0, dst_port = 0;
  bool drop = false;
};

bess::pb::ACLArg Rules(std::initializer_list<Rule> rules) {
  bess::pb::ACLArg arg;
  for (const Rule &r : rules) {
    auto *rule = arg.add_rules();
    rule->set_src_ip(r.src_ip);
    rule->set_dst_ip(r.dst_ip);
    rule->set_src_port(r.src_port);
    rule->set_dst_port(r.dst_port);
    rule->set_drop(r.drop);
  }
  return arg;
}

// Whether `pkt` comes out (unchanged) on gate 0.
bool Passes(ModuleHarness &h, Module *acl, const Bytes &pkt) {
  auto out = h.Run(acl, 0, {pkt}, {0});
  if (out[0].empty()) {
    return false;
  }
  EXPECT_EQ(1u, out[0].size());
  EXPECT_EQ(pkt, out[0][0]);
  return true;
}

TEST(AclTest, RulesAndRefusedCommands) {
  std::mt19937 rng(1);
  const Bytes a = TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);
  const Bytes b = TcpPacket({.sip = "11.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);

  ModuleHarness h;
  Module *acl = h.Create("ACL", Rules({{.src_ip = "10.0.0.0/8"}}));
  ASSERT_NE(nullptr, acl);
  EXPECT_TRUE(Passes(h, acl, a));
  EXPECT_FALSE(Passes(h, acl, b)) << "unmatched traffic is dropped";

  // 65616 would narrow to 80 and forward `b`; it must be refused.
  EXPECT_EQ(EINVAL, h.Command(acl, "add", Rules({{.dst_port = 65616}})).error().code());
  EXPECT_FALSE(Passes(h, acl, b)) << "a refused add changes nothing";

  EXPECT_EQ(EINVAL,
            h.Command(acl, "add", Rules({{.src_ip = "11.0.0.0/8"}, {.dst_ip = "bogus/8"}}))
                .error()
                .code());
  EXPECT_FALSE(Passes(h, acl, b)) << "all or nothing: the good rule was not added";
  EXPECT_TRUE(Passes(h, acl, a));

  h.MustCommand(acl, "add", Rules({{.dst_port = 80}}));
  EXPECT_TRUE(Passes(h, acl, b));

  h.MustCommand(acl, "clear", bess::pb::EmptyArg());
  EXPECT_FALSE(Passes(h, acl, a));
  EXPECT_FALSE(Passes(h, acl, b));
}

TEST(AclTest, FirstMatchWins) {
  std::mt19937 rng(2);
  const Bytes a = TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1"}, rng);
  ModuleHarness h;
  Module *drop_first = h.Create(
      "ACL", Rules({{.src_ip = "10.0.0.0/8", .drop = true}, {.src_ip = "10.1.0.0/16"}}));
  ASSERT_NE(nullptr, drop_first);
  EXPECT_FALSE(Passes(h, drop_first, a));
  // The same rules the other way round forward it.
  Module *pass_first = h.Create(
      "ACL", Rules({{.src_ip = "10.1.0.0/16"}, {.src_ip = "10.0.0.0/8", .drop = true}}));
  ASSERT_NE(nullptr, pass_first);
  EXPECT_TRUE(Passes(h, pass_first, a));
}

}  // namespace
