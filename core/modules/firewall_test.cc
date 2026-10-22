// SPDX-License-Identifier: BSD-3-Clause

// Firewall in process: first match wins over one policy that holds both
// families, a packet the table cannot judge is dropped even by a catch-all,
// and a fragment is not judged by ports it does not carry. D-179:
// "<name>/rules" holds one entry per rule by id, ordered by priority; a
// transaction changes the policy whole or not at all.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <optional>
#include <random>
#include <string>

#include <google/protobuf/any.pb.h>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/allocation_faults.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "testing/rng.h"

namespace {

using bess::testing::Bytes;
using bess::testing::FlowSpec;
using bess::testing::Ip4;
using bess::testing::Ip6;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::TcpPacket;
using bess::testing::UdpPacket;

struct Rule {
  std::string src_ip, dst_ip;
  uint32_t proto = 0;
  uint32_t src_port_low = 0;
  std::optional<uint32_t> src_port_high;
  uint32_t dst_port_low = 0;
  std::optional<uint32_t> dst_port_high;
  bool drop = false;
  std::optional<uint32_t> action_id;
};

bess::pb::FirewallArg Rules(std::initializer_list<Rule> rules) {
  bess::pb::FirewallArg arg;
  for (const Rule &r : rules) {
    auto *rule = arg.add_rules();
    rule->set_src_ip(r.src_ip);
    rule->set_dst_ip(r.dst_ip);
    rule->set_proto(r.proto);
    rule->set_src_port_low(r.src_port_low);
    if (r.src_port_high) {
      rule->set_src_port_high(*r.src_port_high);
    }
    rule->set_dst_port_low(r.dst_port_low);
    if (r.dst_port_high) {
      rule->set_dst_port_high(*r.dst_port_high);
    }
    rule->set_drop(r.drop);
    if (r.action_id) {
      rule->set_action_id(*r.action_id);
    }
  }
  return arg;
}

bess::pb::FirewallRuleKey Id(uint32_t id) {
  bess::pb::FirewallRuleKey key;
  key.set_id(id);
  return key;
}

bess::pb::FirewallRuleValue Entry(int64_t priority, const Rule &r) {
  bess::pb::FirewallRuleValue value;
  value.set_priority(priority);
  *value.mutable_rule() = Rules({r}).rules(0);
  return value;
}

// An operation as a client sends it, decoded by the resource's codec; no
// value erases.
bess::dataplane::Op CodecOp(ModuleHarness &h, const std::string &resource,
                            uint32_t id, const bess::pb::FirewallRuleValue *value) {
  const bess::framework::ResourceCodec *codec = h.Codec(resource);
  EXPECT_NE(nullptr, codec);
  google::protobuf::Any packed_key;
  EXPECT_TRUE(packed_key.PackFrom(Id(id)));
  auto key = codec->Key(packed_key.type_url(), packed_key.value());
  EXPECT_TRUE(key.has_value());
  if (value == nullptr) {
    return bess::dataplane::Op::Erase(resource, *key);
  }
  google::protobuf::Any packed_value;
  EXPECT_TRUE(packed_value.PackFrom(*value));
  auto decoded = codec->Value(packed_value.type_url(), packed_value.value());
  EXPECT_TRUE(decoded.has_value());
  return bess::dataplane::Op::Upsert(resource, *key, std::move(*decoded));
}

// Whether `pkt` comes out (unchanged) on gate 0.
bool Passes(ModuleHarness &h, Module *fw, const Bytes &pkt) {
  auto out = h.Run(fw, 0, {pkt}, {0});
  if (out[0].empty()) {
    return false;
  }
  EXPECT_EQ(1u, out[0].size());
  EXPECT_EQ(pkt, out[0][0]);
  return true;
}

// A TCP header's two ports, as the wire writes them.
Bytes Ports(uint16_t sport, uint16_t dport) {
  Bytes out(20, 0);
  out[0] = static_cast<uint8_t>(sport >> 8);
  out[1] = static_cast<uint8_t>(sport & 0xff);
  out[2] = static_cast<uint8_t>(dport >> 8);
  out[3] = static_cast<uint8_t>(dport & 0xff);
  return out;
}

Bytes Cat(Bytes a, const Bytes &b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
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

TEST(FirewallTest, FirstMatchWinsAndUnmatchedIsDropped) {
  std::mt19937 rng = bess::testing::SeededRng(7);
  const Bytes dns =
      UdpPacket({.sip = "10.2.2.2", .dip = "20.0.0.1", .sport = 1000, .dport = 53}, rng);
  const Bytes web =
      TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 85}, rng);
  const Bytes ssh =
      TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 22}, rng);

  ModuleHarness h;
  Module *fw = h.Create("Firewall",
                        Rules({{.src_ip = "10.0.0.0/8", .proto = 17, .dst_port_low = 53, .dst_port_high = 53},
                               {.src_ip = "10.0.0.0/8", .proto = 6, .dst_port_low = 80, .dst_port_high = 90}}),
                        "fw_main");
  ASSERT_NE(nullptr, fw);
  EXPECT_TRUE(Passes(h, fw, dns));
  EXPECT_TRUE(Passes(h, fw, web));
  // Nothing matches: the packet is dropped, not passed by default.
  EXPECT_FALSE(Passes(h, fw, ssh));

  // Rule order is the policy: a drop in front of the web rule stops it, and
  // the narrow drop does not touch the DNS rule behind it.
  Module *fw2 = h.Create(
      "Firewall",
      Rules({{.src_ip = "10.1.0.0/16", .drop = true},
             {.src_ip = "10.0.0.0/8", .proto = 6, .dst_port_low = 80, .dst_port_high = 90},
             {.src_ip = "10.0.0.0/8", .proto = 17, .dst_port_low = 53, .dst_port_high = 53}}),
      "fw_drop_first");
  ASSERT_NE(nullptr, fw2);
  EXPECT_FALSE(Passes(h, fw2, web));
  EXPECT_TRUE(Passes(h, fw2, dns));
}

// The action attribute's name is configuration, so a graph with a second
// action table over the same rules can give each its own output.
TEST(FirewallTest, TheActionAttributeIsTheConfiguredName) {
  ModuleHarness h;
  bess::pb::FirewallArg policy = Rules({{.drop = false}});
  policy.set_action_attr("flow_action");
  Module *fw = h.Create("Firewall", policy, "fw_named");
  ASSERT_NE(nullptr, fw);

  bool wrote = false;
  for (const auto &attr : fw->all_attrs()) {
    if (attr.name == "flow_action") {
      wrote = true;
      EXPECT_EQ(bess::metadata::Attribute::AccessMode::kWrite, attr.mode);
    }
    EXPECT_NE("action_id", attr.name) << "the default name was not registered";
  }
  EXPECT_TRUE(wrote);
}

TEST(FirewallTest, OnePolicyHoldsBothFamilies) {
  std::mt19937 rng = bess::testing::SeededRng(11);
  const Bytes v4web =
      TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);
  const Bytes v4other =
      TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 443}, rng);
  const Bytes v6web = PacketBuilder()
                          .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                          .IPv6("2001:db8::1", "2001:db8::2")
                          .Tcp(1000, 80)
                          .PadTo(60)
                          .Build();
  const Bytes v6other = PacketBuilder()
                            .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                            .IPv6("2001:db8::1", "2001:db8::2")
                            .Tcp(1000, 443)
                            .PadTo(60)
                            .Build();

  ModuleHarness h;
  Module *fw = h.Create(
      "Firewall",
      Rules({{.src_ip = "10.0.0.0/8", .proto = 6, .dst_port_low = 80, .dst_port_high = 80},
             {.src_ip = "2001:db8::/32", .proto = 6, .dst_port_low = 80, .dst_port_high = 80}}),
      "fw_both");
  ASSERT_NE(nullptr, fw);
  // Each family is judged by its own rule: the v6 rule cannot claim a v4
  // packet whose first four bytes happen to fall in 10/8, and the other way
  // round.
  EXPECT_TRUE(Passes(h, fw, v4web));
  EXPECT_TRUE(Passes(h, fw, v6web));
  EXPECT_FALSE(Passes(h, fw, v4other));
  EXPECT_FALSE(Passes(h, fw, v6other));

  // A rule with no prefix names no family and sees both.
  Module *any_family = h.Create("Firewall", Rules({{.drop = false}}), "fw_any");
  ASSERT_NE(nullptr, any_family);
  EXPECT_TRUE(Passes(h, any_family, v4other));
  EXPECT_TRUE(Passes(h, any_family, v6other));
  EXPECT_FALSE(Passes(h, any_family,
                      PacketBuilder()
                          .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                          .Arp(1, "02:00:00:00:00:02", "10.0.0.1", "00:00:00:00:00:00", "10.0.0.2")
                          .PadTo(60)
                          .Build()));
}

TEST(FirewallTest, AFamilyThePolicyDoesNotNameIsDropped) {
  std::mt19937 rng = bess::testing::SeededRng(13);
  const Bytes v4 = TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1, .dport = 2}, rng);
  const Bytes v6 = PacketBuilder()
                       .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                       .IPv6("2001:db8::1", "2001:db8::2")
                       .Tcp(1, 2)
                       .PadTo(60)
                       .Build();

  ModuleHarness h;
  Module *v4_only = h.Create("Firewall", Rules({{.src_ip = "0.0.0.0/0"}}), "fw_v4");
  ASSERT_NE(nullptr, v4_only);
  EXPECT_TRUE(Passes(h, v4_only, v4));
  EXPECT_FALSE(Passes(h, v4_only, v6));

  Module *v6_only = h.Create("Firewall", Rules({{.src_ip = "::/0"}}), "fw_v6");
  ASSERT_NE(nullptr, v6_only);
  EXPECT_TRUE(Passes(h, v6_only, v6));
  EXPECT_FALSE(Passes(h, v6_only, v4));
}

TEST(FirewallTest, RefusesRulesItCannotExpress) {
  ModuleHarness h;
  // A prefix that is not CIDR, or a length beyond the family's width.
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.src_ip = "bogus/8"}})));
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.src_ip = "10.0.0.0"}})));
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.src_ip = "10.0.0.0/33"}})));
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.src_ip = "2001:db8::/129"}})));
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.src_ip = "10.0.0.0/8/8"}})));
  // One rule cannot name both families.
  EXPECT_NE("", h.CreateError(
                    "Firewall", Rules({{.src_ip = "10.0.0.0/8", .dst_ip = "2001:db8::/32"}})));
  // A port out of range (before narrowing), or a range the wrong way round.
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.dst_port_low = 65616}})));
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.dst_port_low = 90, .dst_port_high = 80}})));
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.proto = 256}})));

  // The accepted shapes: a bare prefix, both families, ranges, no rules.
  EXPECT_EQ("", h.CreateError("Firewall", Rules({{.src_ip = "10.0.0.0/8"}})));
  EXPECT_EQ("", h.CreateError("Firewall", Rules({{.src_ip = "2001:db8::/32", .dst_ip = "2001:db8:1::/48"}})));
  EXPECT_EQ("", h.CreateError("Firewall", Rules({{.dst_port_low = 80, .dst_port_high = 90}})));
  EXPECT_EQ("", h.CreateError("Firewall", bess::pb::FirewallArg()));

  // A rule that drops cannot name an action: the packet never reaches a
  // module that would resolve it.
  EXPECT_NE("", h.CreateError("Firewall", Rules({{.drop = true, .action_id = 7}})));
}

// D-141: the ActionId a rule names travels with the rule, through the
// resource's read-back, the way the peers' rule values carry theirs. Init's
// rules are ids 1..n at priority 0.
TEST(FirewallTest, AnActionIdTravelsWithTheRule) {
  ModuleHarness h;
  const Rule acted{.src_ip = "10.0.0.0/8", .action_id = 7};
  const Rule dropped{.src_ip = "12.0.0.0/8", .drop = true};
  Module *fw = h.Create("Firewall", Rules({acted, dropped}), "fw_act");
  ASSERT_NE(nullptr, fw);
  const auto k1 = Id(1);
  const auto k2 = Id(2);
  const auto v1 = Entry(0, acted);
  const auto v2 = Entry(0, dropped);
  EXPECT_EQ("", h.ReadBackDiffers("fw_act/rules", {{&k1, &v1}, {&k2, &v2}}));
  // A rule that drops cannot name an action (the refusal Init gives); the
  // rule stays as it was.
  EXPECT_NE("", h.UpsertResource("fw_act/rules", k2,
                                 Entry(0, {.drop = true, .action_id = 7})));
  EXPECT_EQ("", h.ReadBackDiffers("fw_act/rules", {{&k1, &v1}, {&k2, &v2}}));
}

TEST(FirewallTest, PriorityOrdersTheRulesAndTheIdBreaksTies) {
  std::mt19937 rng = bess::testing::SeededRng(17);
  const Bytes a = TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);
  const Bytes b = TcpPacket({.sip = "10.2.0.1", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);

  ModuleHarness h;
  Module *fw = h.Create("Firewall", Rules({{.src_ip = "10.0.0.0/8"}}), "fw_prio");
  ASSERT_NE(nullptr, fw);
  const std::string rules = "fw_prio/rules";
  // An RCU swap, so a write never pauses the workers.
  EXPECT_FALSE(RequiresPausedWorkers(rules));
  EXPECT_TRUE(Passes(h, fw, a));
  EXPECT_TRUE(Passes(h, fw, b));

  // A higher priority is checked before a lower id.
  EXPECT_EQ("", h.UpsertResource(rules, Id(7), Entry(10, {.src_ip = "10.1.0.0/16", .drop = true})));
  EXPECT_FALSE(Passes(h, fw, a));
  EXPECT_TRUE(Passes(h, fw, b));

  // Equal priorities: the lower id first.
  const auto pass_24 = Entry(10, {.src_ip = "10.1.2.0/24"});
  EXPECT_EQ("", h.UpsertResource(rules, Id(3), pass_24));
  EXPECT_TRUE(Passes(h, fw, a));

  // Raising rule 7's priority moves it ahead; its id stays.
  const auto drop_first = Entry(20, {.src_ip = "10.1.0.0/16", .drop = true});
  EXPECT_EQ("", h.UpsertResource(rules, Id(7), drop_first));
  EXPECT_FALSE(Passes(h, fw, a));
  const auto k1 = Id(1);
  const auto k3 = Id(3);
  const auto k7 = Id(7);
  const auto v1 = Entry(0, {.src_ip = "10.0.0.0/8"});
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{&k1, &v1}, {&k3, &pass_24}, {&k7, &drop_first}}));

  // Erasing a rule removes only it; a rule that is not there is refused.
  EXPECT_EQ("", h.EraseResource(rules, k7));
  EXPECT_TRUE(Passes(h, fw, a));
  EXPECT_NE("", h.EraseResource(rules, k7));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{&k1, &v1}, {&k3, &pass_24}}));
}

// One transaction is one policy change: every operation is checked before the
// table is built, and a refusal anywhere leaves the policy and the rules as
// they were.
TEST(FirewallTest, ATransactionChangesThePolicyWholeOrNotAtAll) {
  std::mt19937 rng = bess::testing::SeededRng(23);
  const Bytes a = TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);
  const Bytes b = TcpPacket({.sip = "11.1.2.3", .dip = "20.0.0.1", .sport = 1000, .dport = 80}, rng);

  ModuleHarness h;
  Module *fw = h.Create("Firewall", Rules({{.src_ip = "10.0.0.0/8"}}), "fw_tx");
  ASSERT_NE(nullptr, fw);
  const std::string rules = "fw_tx/rules";
  auto op = [&](uint32_t id, const bess::pb::FirewallRuleValue *value) {
    return CodecOp(h, rules, id, value);
  };

  // Swap which network passes in one transaction.
  const auto pass_11 = Entry(0, {.src_ip = "11.0.0.0/8"});
  EXPECT_EQ("", h.Apply({op(1, nullptr), op(2, &pass_11)}));
  EXPECT_FALSE(Passes(h, fw, a));
  EXPECT_TRUE(Passes(h, fw, b));

  // A refused operation (an erase of a rule that is not there) refuses the
  // upsert beside it.
  const auto pass_10 = Entry(0, {.src_ip = "10.0.0.0/8"});
  EXPECT_NE("", h.Apply({op(3, &pass_10), op(9, nullptr)}));
  EXPECT_FALSE(Passes(h, fw, a));
  const auto k2 = Id(2);
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{&k2, &pass_11}}));

  // The codec checks each rule as it arrives, so a rule the table cannot
  // express never reaches a transaction.
  for (const Rule &bad : {Rule{.src_ip = "2001:db8::/129"},
                          Rule{.src_ip = "10.0.0.0/8", .dst_ip = "2001:db8::/32"},
                          Rule{.dst_port_low = 65616},
                          Rule{.dst_port_low = 90, .dst_port_high = 80}}) {
    EXPECT_NE("", h.UpsertResource(rules, Id(4), Entry(0, bad)));
  }
  // A value without its rule is refused rather than read as the empty rule
  // (match everything, pass), which a catch-all writes as "rule": {}.
  bess::pb::FirewallRuleValue no_rule;
  no_rule.set_priority(1);
  EXPECT_NE("", h.UpsertResource(rules, Id(4), no_rule));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{&k2, &pass_11}}));
  EXPECT_TRUE(Passes(h, fw, b));

  // No rules: everything is dropped.
  EXPECT_EQ("", h.EraseResource(rules, k2));
  EXPECT_FALSE(Passes(h, fw, a));
  EXPECT_FALSE(Passes(h, fw, b));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {}));
}

// The engine reserved everything publication needs: inserting a rule,
// replacing one and erasing one - alone and in one transaction - publish
// without allocating.
TEST(FirewallTest, PublishingAChangeAllocatesNothing) {
  ModuleHarness h;
  Module *fw = h.Create("Firewall", Rules({{.src_ip = "10.0.0.0/8"}}), "fw_alloc");
  ASSERT_NE(nullptr, fw);
  const std::string rules = "fw_alloc/rules";
  static std::optional<bess::fault_injection::AllocationFaults> window;
  static size_t allocations;
  allocations = 0;
  bess::dataplane::internal::g_publish_window_hook = [](bool entering) {
    if (entering) {
      window.emplace();
    } else if (window) {
      allocations += window->allocations();
      window.reset();
    }
  };
  const auto drop_11 = Entry(5, {.src_ip = "11.0.0.0/8", .drop = true});
  const auto pass_12 = Entry(0, {.src_ip = "12.0.0.0/8"});
  EXPECT_EQ("", h.UpsertResource(rules, Id(2), drop_11));  // insert
  EXPECT_EQ("", h.UpsertResource(rules, Id(1), pass_12));  // replace
  // A replace, an insert and an erase in one transaction.
  EXPECT_EQ("", h.Apply({CodecOp(h, rules, 1, &drop_11), CodecOp(h, rules, 3, &pass_12),
                         CodecOp(h, rules, 2, nullptr)}));
  EXPECT_EQ("", h.EraseResource(rules, Id(3)));  // erase
  bess::dataplane::internal::g_publish_window_hook = nullptr;
  EXPECT_EQ(0u, allocations) << "a firewall publish allocated";
  const auto k1 = Id(1);
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{&k1, &drop_11}}));
}

TEST(FirewallTest, APacketThisTableCannotJudgeIsDropped) {
  std::mt19937 rng = bess::testing::SeededRng(19);
  const Bytes tcp = TcpPacket({.sip = "10.1.2.3", .dip = "20.0.0.1", .sport = 1, .dport = 2}, rng);
  const Bytes arp = PacketBuilder()
                        .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                        .Arp(1, "02:00:00:00:00:02", "10.0.0.1", "00:00:00:00:00:00", "10.0.0.2")
                        .PadTo(60)
                        .Build();
  const Bytes unknown = PacketBuilder()
                            .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                            .Payload("not an IP packet")
                            .PadTo(60)
                            .Build();
  // An IPv4 header claiming more header bytes than the packet holds.
  Bytes truncated = PacketBuilder()
                        .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                        .IPv4("10.1.2.3", "20.0.0.1")
                        .Build();
  truncated[14] = 0x4f;  // header length 15 * 4 = 60 bytes

  ModuleHarness h;
  // A catch-all pass: it must not claim a packet the table cannot read.
  Module *fw = h.Create("Firewall", Rules({{}}), "fw_catchall");
  ASSERT_NE(nullptr, fw);
  EXPECT_TRUE(Passes(h, fw, tcp));
  EXPECT_FALSE(Passes(h, fw, arp));
  EXPECT_FALSE(Passes(h, fw, unknown));
  EXPECT_FALSE(Passes(h, fw, truncated));
}

TEST(FirewallTest, AFragmentIsNotJudgedByPortsItDoesNotCarry) {
  // A non-first fragment carries no transport header: its ports are unknown,
  // so only a rule whose port range starts at 0 can match it.
  const Bytes fragment = PacketBuilder()
                             .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                             .Add(PacketBuilder::IPv4Hdr{.src = Ip4("10.1.2.3"),
                                                         .dst = Ip4("20.0.0.1"),
                                                         .proto = 6,
                                                         .frag = 0x2008})
                             .Payload("later fragment bytes")
                             .PadTo(60)
                             .Build();
  // The first fragment of the same datagram: offset 0, so the ports are read.
  const Bytes first = PacketBuilder()
                          .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                          .Add(PacketBuilder::IPv4Hdr{.src = Ip4("10.1.2.3"),
                                                      .dst = Ip4("20.0.0.1"),
                                                      .proto = 6,
                                                      .frag = 0x2000})
                          .Tcp(1000, 80)
                          .PadTo(60)
                          .Build();

  ModuleHarness h;
  Module *port_rule = h.Create(
      "Firewall", Rules({{.src_ip = "10.0.0.0/8", .proto = 6, .dst_port_low = 80, .dst_port_high = 80}}),
      "fw_port");
  ASSERT_NE(nullptr, port_rule);
  EXPECT_FALSE(Passes(h, port_rule, fragment));
  EXPECT_TRUE(Passes(h, port_rule, first));

  Module *any_port = h.Create("Firewall", Rules({{.src_ip = "10.0.0.0/8", .proto = 6}}), "fw_anyport");
  ASSERT_NE(nullptr, any_port);
  EXPECT_TRUE(Passes(h, any_port, fragment));
}

TEST(FirewallTest, AnIpv6PacketIsJudgedThroughItsExtensionHeaders) {
  // A hop-by-hop header (next header 6) in front of the TCP header: a rule
  // that names TCP must see the transport protocol and the ports behind it.
  Bytes behind_ext(8, 0);
  behind_ext[0] = 6;  // next header: TCP
  behind_ext[1] = 0;  // length: (0 + 1) * 8 bytes
  const Bytes hop_by_hop = PacketBuilder()
                               .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                               .Add(PacketBuilder::IPv6Hdr{.src = Ip6("2001:db8::1"),
                                                           .dst = Ip6("2001:db8::2"),
                                                           .next = 0})
                               .Add(PacketBuilder::Raw{Cat(behind_ext, Ports(1000, 80))})
                               .PadTo(60)
                               .Build();

  // A non-first fragment: the fragment header (44) says the transport header
  // is elsewhere.
  Bytes fragment_header(8, 0);
  fragment_header[0] = 6;     // next header: TCP
  fragment_header[2] = 0x00;  // offset 8 bytes -> not the first fragment
  fragment_header[3] = 0x08;
  const Bytes v6_fragment = PacketBuilder()
                                .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                                .Add(PacketBuilder::IPv6Hdr{.src = Ip6("2001:db8::1"),
                                                            .dst = Ip6("2001:db8::2"),
                                                            .next = 44})
                                .Add(PacketBuilder::Raw{Cat(fragment_header, Ports(1000, 80))})
                                .PadTo(60)
                                .Build();

  ModuleHarness h;
  Module *port_rule = h.Create(
      "Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 6, .dst_port_low = 80, .dst_port_high = 80}}),
      "fw6_port");
  ASSERT_NE(nullptr, port_rule);
  EXPECT_TRUE(Passes(h, port_rule, hop_by_hop));
  EXPECT_FALSE(Passes(h, port_rule, v6_fragment));

  Module *any_port = h.Create("Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 6}}), "fw6_any");
  ASSERT_NE(nullptr, any_port);
  EXPECT_TRUE(Passes(h, any_port, hop_by_hop));
  EXPECT_TRUE(Passes(h, any_port, v6_fragment));
}

TEST(FirewallTest, ATransportBehindFourExtensionHeadersIsStillRead) {
  // Four extension headers and then the transport: the walk follows four
  // extension headers, and the transport may sit exactly at that bound (the
  // mobile-IP fragment shape). Its ports are in the frame, so a rule that
  // constrains them must match - judging it with ports 0 would drop traffic
  // whose ports are right there.
  Bytes chain(32, 0);
  chain[0] = 43;   // hop-by-hop -> routing
  chain[8] = 44;   // routing -> fragment
  chain[16] = 60;  // fragment (first: offset 0) -> destination options
  chain[24] = 6;   // destination options -> TCP
  const Bytes packet = PacketBuilder()
                           .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                           .Add(PacketBuilder::IPv6Hdr{.src = Ip6("2001:db8::1"),
                                                       .dst = Ip6("2001:db8::2"),
                                                       .next = 0})
                           .Add(PacketBuilder::Raw{Cat(chain, Ports(1000, 80))})
                           .PadTo(96)
                           .Build();

  ModuleHarness h;
  Module *port_rule = h.Create(
      "Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 6, .dst_port_low = 80, .dst_port_high = 80}}),
      "fw6_four");
  ASSERT_NE(nullptr, port_rule);
  EXPECT_TRUE(Passes(h, port_rule, packet));
  // The other port still does not match, so the match is the port and not the
  // protocol alone.
  Module *wrong_port = h.Create(
      "Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 6, .dst_port_low = 443, .dst_port_high = 443}}),
      "fw6_four_wrong");
  ASSERT_NE(nullptr, wrong_port);
  EXPECT_FALSE(Passes(h, wrong_port, packet));
}

}  // namespace
