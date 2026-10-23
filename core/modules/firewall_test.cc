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

// A TCP header (data offset 5, no flags) with the two ports, as the wire
// writes it.
Bytes Ports(uint16_t sport, uint16_t dport) {
  Bytes out(20, 0);
  out[0] = static_cast<uint8_t>(sport >> 8);
  out[1] = static_cast<uint8_t>(sport & 0xff);
  out[2] = static_cast<uint8_t>(dport >> 8);
  out[3] = static_cast<uint8_t>(dport & 0xff);
  out[12] = 5 << 4;
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

// IPv6 2001:db8::1 -> 2001:db8::2 carrying `chain` (its first header `first`)
// and then `l4`, behind `tags` VLAN tags (1: 802.1Q; 2: 802.1ad, 802.1Q).
Bytes V6(uint8_t first, const Bytes &chain, const Bytes &l4, int tags = 0) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:01", "02:00:00:00:00:02");
  if (tags == 2) {
    b.Dot1AD(5);
  }
  if (tags >= 1) {
    b.Dot1Q(7);
  }
  b.Add(PacketBuilder::IPv6Hdr{.src = Ip6("2001:db8::1"), .dst = Ip6("2001:db8::2"), .next = first});
  return b.Add(PacketBuilder::Raw{Cat(chain, l4)}).PadTo(60).Build();
}

// `n` destination-options headers of 8 bytes, the last naming `last`.
Bytes DestOpts(size_t n, uint8_t last) {
  Bytes chain(8 * n, 0);
  for (size_t i = 0; i < n; i++) {
    chain[8 * i] = i + 1 == n ? last : 60;
  }
  return chain;
}

TEST(FirewallTest, ATransportBehindUpToEightExtensionHeadersIsRead) {
  // The walk is the checked parse's (D-188): up to eight extension headers,
  // then the transport. Its ports are in the frame, so a rule that constrains
  // them must match (before D-188 the walk stopped at four, and a transport
  // behind five to eight was judged with an extension header as its
  // protocol).
  ModuleHarness h;
  Module *port_rule = h.Create(
      "Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 6, .dst_port_low = 80, .dst_port_high = 80}}),
      "fw6_chain");
  ASSERT_NE(nullptr, port_rule);
  // The other port still does not match, so the match is the port and not the
  // protocol alone.
  Module *wrong_port = h.Create(
      "Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 6, .dst_port_low = 443, .dst_port_high = 443}}),
      "fw6_chain_wrong");
  ASSERT_NE(nullptr, wrong_port);
  Module *catch_all = h.Create("Firewall", Rules({{}}), "fw6_chain_all");
  ASSERT_NE(nullptr, catch_all);
  for (size_t n = 1; n <= 8; n++) {
    const Bytes packet = V6(60, DestOpts(n, 6), Ports(1000, 80));
    EXPECT_TRUE(Passes(h, port_rule, packet)) << n;
    EXPECT_FALSE(Passes(h, wrong_port, packet)) << n;
  }
  // A mixed chain: hop-by-hop, routing, a first fragment, destination options.
  Bytes chain(32, 0);
  chain[0] = 43;   // hop-by-hop -> routing
  chain[8] = 44;   // routing -> fragment
  chain[16] = 60;  // fragment (first: offset 0) -> destination options
  chain[24] = 6;   // destination options -> TCP
  EXPECT_TRUE(Passes(h, port_rule, V6(0, chain, Ports(1000, 80))));
  // Past the bound the packet is malformed: nothing judges it, not even a
  // catch-all.
  EXPECT_FALSE(Passes(h, catch_all, V6(60, DestOpts(9, 6), Ports(1000, 80))));
}

TEST(FirewallTest, AnIpv6AuthenticationHeaderIsWalked) {
  // AH (12 bytes: length field 1, in 4-byte units past the first 8) and then
  // TCP: the rule sees TCP and its ports. ESP ends the walk.
  Bytes ah(12, 0);
  ah[0] = 6;
  ah[1] = 1;
  ModuleHarness h;
  Module *port_rule = h.Create(
      "Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 6, .dst_port_low = 80, .dst_port_high = 80}}),
      "fw6_ah");
  ASSERT_NE(nullptr, port_rule);
  Module *esp_rule = h.Create("Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 50}}), "fw6_esp");
  ASSERT_NE(nullptr, esp_rule);
  EXPECT_TRUE(Passes(h, port_rule, V6(51, ah, Ports(1000, 80))));
  EXPECT_FALSE(Passes(h, port_rule, V6(51, ah, Ports(1000, 443))));
  ah[0] = 50;  // AH -> ESP
  EXPECT_TRUE(Passes(h, esp_rule, V6(51, ah, Bytes(16, 0))));
  EXPECT_TRUE(Passes(h, esp_rule, V6(50, {}, Bytes(16, 0))));
}

TEST(FirewallTest, TaggedFramesAreJudged) {
  // No tag, an 802.1Q tag, and 802.1ad + 802.1Q: the parse reads through up to
  // two tags (D-188; before, a tagged frame was not judged and was dropped).
  ModuleHarness h;
  Module *v4_port = h.Create(
      "Firewall", Rules({{.src_ip = "10.0.0.0/8", .proto = 6, .dst_port_low = 80, .dst_port_high = 80}}),
      "fw_tag4");
  ASSERT_NE(nullptr, v4_port);
  Module *v4_udp = h.Create(
      "Firewall", Rules({{.src_ip = "10.0.0.0/8", .proto = 17, .dst_port_low = 53, .dst_port_high = 53}}),
      "fw_tag4_udp");
  ASSERT_NE(nullptr, v4_udp);
  Module *v4_any = h.Create("Firewall", Rules({{.src_ip = "10.0.0.0/8", .proto = 6}}), "fw_tag4_any");
  ASSERT_NE(nullptr, v4_any);
  Module *v6_port = h.Create(
      "Firewall", Rules({{.src_ip = "2001:db8::/32", .proto = 6, .dst_port_low = 80, .dst_port_high = 80}}),
      "fw_tag6");
  ASSERT_NE(nullptr, v6_port);
  auto v4 = [](int tags) {
    PacketBuilder b;
    b.Ether("02:00:00:00:00:01", "02:00:00:00:00:02");
    if (tags == 2) {
      b.Dot1AD(5);
    }
    if (tags >= 1) {
      b.Dot1Q(7);
    }
    return b;
  };
  for (int tags = 0; tags <= 2; tags++) {
    const Bytes tcp = v4(tags).IPv4("10.1.2.3", "20.0.0.1").Tcp(1000, 80).PadTo(64).Build();
    const Bytes tcp_443 = v4(tags).IPv4("10.1.2.3", "20.0.0.1").Tcp(1000, 443).PadTo(64).Build();
    const Bytes udp = v4(tags).IPv4("10.1.2.3", "20.0.0.1").Udp(1000, 53).PadTo(64).Build();
    const Bytes fragment = v4(tags)
                               .Add(PacketBuilder::IPv4Hdr{.src = Ip4("10.1.2.3"),
                                                           .dst = Ip4("20.0.0.1"),
                                                           .proto = 6,
                                                           .frag = 0x2008})
                               .Payload("later fragment bytes")
                               .PadTo(64)
                               .Build();
    EXPECT_TRUE(Passes(h, v4_port, tcp)) << tags;
    EXPECT_FALSE(Passes(h, v4_port, tcp_443)) << tags;
    EXPECT_TRUE(Passes(h, v4_udp, udp)) << tags;
    EXPECT_FALSE(Passes(h, v4_port, udp)) << tags;
    // A later fragment behind tags: judged, with ports 0.
    EXPECT_FALSE(Passes(h, v4_port, fragment)) << tags;
    EXPECT_TRUE(Passes(h, v4_any, fragment)) << tags;
    EXPECT_TRUE(Passes(h, v6_port, V6(6, {}, Ports(1000, 80), tags))) << tags;
    EXPECT_FALSE(Passes(h, v6_port, V6(6, {}, Ports(1000, 443), tags))) << tags;
  }
}

TEST(FirewallTest, MalformedPacketsAreNotJudged) {
  // A packet whose lengths lie, whose EtherType and IP version disagree, or
  // whose transport header is impossible is dropped as one the table cannot
  // judge, even by a catch-all (D-188; before, all of these but the IHL were
  // judged).
  const Bytes tcp4 = PacketBuilder()
                         .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                         .IPv4("10.1.2.3", "20.0.0.1")
                         .Tcp(1000, 80)
                         .PadTo(60)
                         .Build();
  const Bytes udp4 = PacketBuilder()
                         .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                         .IPv4("10.1.2.3", "20.0.0.1")
                         .Udp(1000, 53)
                         .PadTo(60)
                         .Build();
  const Bytes tcp6 = V6(6, {}, Ports(1000, 80));
  auto with = [](Bytes b, size_t at, std::initializer_list<uint8_t> bytes) {
    for (const uint8_t v : bytes) {
      b[at++] = v;
    }
    return b;
  };
  const std::pair<const char *, Bytes> malformed[] = {
      {"IPv4 total length past the frame", with(tcp4, 16, {0x01, 0x00})},
      {"IPv4 total length below the header", with(tcp4, 16, {0x00, 0x10})},
      {"IPv4 header length 15 (60 bytes)", with(tcp4, 14, {0x4f})},
      {"EtherType IPv4, IPv6 inside", with(tcp6, 12, {0x08, 0x00})},
      {"EtherType IPv6, IPv4 inside", with(tcp4, 12, {0x86, 0xdd})},
      {"TCP data offset 4", with(tcp4, 14 + 20 + 12, {0x40})},
      {"UDP length 7", with(udp4, 14 + 20 + 4, {0x00, 0x07})},
      {"UDP length past the IP packet", with(udp4, 14 + 20 + 4, {0x00, 0xc8})},
      {"IPv6 payload length past the frame", with(tcp6, 14 + 4, {0x01, 0x00})},
      {"IPv6 extension header past the payload", V6(60, with(DestOpts(1, 6), 1, {200}), Ports(1, 2))},
  };
  ModuleHarness h;
  Module *fw = h.Create("Firewall", Rules({{}}), "fw_malformed");
  ASSERT_NE(nullptr, fw);
  EXPECT_TRUE(Passes(h, fw, tcp4));
  EXPECT_TRUE(Passes(h, fw, udp4));
  EXPECT_TRUE(Passes(h, fw, tcp6));
  for (const auto &[what, packet] : malformed) {
    EXPECT_FALSE(Passes(h, fw, packet)) << what;
  }
}

TEST(FirewallTest, IcmpMatchesWithPortsZero) {
  // ICMP has no ports: an echo is judged with both ports 0, never by its
  // identifier (which the parse carries in its port fields for ConnTrack).
  const Bytes echo4 = PacketBuilder()
                          .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                          .IPv4("10.1.2.3", "20.0.0.1")
                          .Add(PacketBuilder::IcmpHdr{.type = 8, .id = 7, .seq = 1})
                          .PadTo(60)
                          .Build();
  const Bytes echo6 = PacketBuilder()
                          .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                          .Add(PacketBuilder::IPv6Hdr{.src = Ip6("2001:db8::1"),
                                                      .dst = Ip6("2001:db8::2"),
                                                      .next = 58})
                          .Add(PacketBuilder::IcmpHdr{.type = 128, .id = 7, .seq = 1})
                          .PadTo(70)
                          .Build();
  ModuleHarness h;
  for (const auto &[proto, src, packet] :
       {std::tuple{1u, "10.0.0.0/8", echo4}, std::tuple{58u, "2001:db8::/32", echo6}}) {
    const std::string name = "fw_icmp" + std::to_string(proto);
    Module *zero = h.Create(
        "Firewall",
        Rules({{.src_ip = src, .proto = proto, .src_port_high = 0, .dst_port_high = 0}}),
        name + "_zero");
    ASSERT_NE(nullptr, zero);
    Module *id = h.Create(
        "Firewall", Rules({{.src_ip = src, .proto = proto, .dst_port_low = 7, .dst_port_high = 7}}),
        name + "_id");
    ASSERT_NE(nullptr, id);
    EXPECT_TRUE(Passes(h, zero, packet)) << proto;
    EXPECT_FALSE(Passes(h, id, packet)) << proto;
  }
}

}  // namespace
