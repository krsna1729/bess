// SPDX-License-Identifier: BSD-3-Clause

// ACL in process (was bessctl/module_tests/acl.py): first match wins,
// unmatched traffic is dropped, and a refused command (a wire port that
// would narrow, a malformed prefix) changes nothing, all or nothing. D-129:
// "<name>/rules" holds the whole rule list, and add/clear are legacy-only.

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

// "<name>/rules" is the whole ordered rule list: setting it replaces the
// list (an RCU swap: no paused workers), it reads back as given, a list with
// a bad rule is refused whole, and an empty list drops everything.
TEST(AclTest, RulesResourceReplacesTheList) {
  std::mt19937 rng(3);
  const Bytes a = TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);
  const Bytes b = TcpPacket({.sip = "11.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);

  ModuleHarness h;
  const auto initial = Rules({{.src_ip = "10.0.0.0/8"}});
  Module *acl = h.Create("ACL", initial, "acl_res");
  ASSERT_NE(nullptr, acl);
  const std::string rules = "acl_res/rules";
  EXPECT_FALSE(RequiresPausedWorkers(rules));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{nullptr, &initial}})) << "Init's rules";

  // Order matters (first match): b passes, a (in 10/8) is dropped.
  const auto two = Rules({{.src_ip = "11.0.0.0/8"}, {.src_ip = "10.0.0.0/8", .drop = true}});
  EXPECT_EQ("", h.SetResource(rules, two));
  EXPECT_FALSE(Passes(h, acl, a));
  EXPECT_TRUE(Passes(h, acl, b));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{nullptr, &two}}));

  EXPECT_NE("", h.SetResource(rules, Rules({{.src_ip = "10.0.0.0/8"}, {.dst_ip = "bogus/8"}})));
  EXPECT_NE("", h.SetResource(rules, Rules({{.dst_port = 65616}})));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{nullptr, &two}})) << "refused sets change nothing";
  EXPECT_FALSE(Passes(h, acl, a));
  EXPECT_TRUE(Passes(h, acl, b));

  // The legacy add appends to the same list.
  h.MustCommand(acl, "add", Rules({{.dst_port = 80}}));
  const auto three = Rules(
      {{.src_ip = "11.0.0.0/8"}, {.src_ip = "10.0.0.0/8", .drop = true}, {.dst_port = 80}});
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{nullptr, &three}}));

  const bess::pb::ACLArg none;
  EXPECT_EQ("", h.SetResource(rules, none));
  EXPECT_FALSE(Passes(h, acl, a));
  EXPECT_FALSE(Passes(h, acl, b));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{nullptr, &none}}));
}

// add and clear edit "<name>/rules": control_v2's InvokeAction refuses them.
TEST(AclTest, CommandsAreLegacyOnly) {
  std::mt19937 rng(4);
  const Bytes b = TcpPacket({.sip = "11.1.2.3", .dip = "20.0.0.1"}, rng);
  ModuleHarness h;
  Module *acl = h.Create("ACL", bess::pb::ACLArg());
  ASSERT_NE(nullptr, acl);
  EXPECT_EQ(ENOTSUP, h.Action(acl, "add", Rules({{.src_ip = "11.0.0.0/8"}})).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(acl, "clear", bess::pb::EmptyArg()).error().code());
  EXPECT_FALSE(Passes(h, acl, b)) << "the refused add did nothing";
  // The legacy ModuleCommand still runs them.
  h.MustCommand(acl, "add", Rules({{.src_ip = "11.0.0.0/8"}}));
  EXPECT_TRUE(Passes(h, acl, b));
}

}  // namespace
