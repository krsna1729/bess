// SPDX-License-Identifier: BSD-3-Clause

// ACL in process (was bessctl/module_tests/acl.py): first match wins,
// unmatched traffic is dropped, and a refused set (a wire port that
// would narrow, a malformed prefix) changes nothing, all or nothing. D-129:
// "<name>/rules" holds the whole rule list. The addresses and ports come
// from the checked parse: a rule constrains only the fields it names (an
// all-wildcard rule matches a frame that is not IPv4), a malformed frame is
// dropped, and only TCP and UDP (not a later fragment) have ports.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

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
using bess::testing::MakePacket;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
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

// Whether the packet `frame` (a chain whose first segment ends at `split`,
// 0: one segment) comes out unchanged on gate 0.
bool PassesChain(ModuleHarness &h, Module *acl, const Bytes &frame, size_t split) {
  auto out = h.RunPackets(acl, 0, {MakePacket(frame, split)}, acl, {0});
  if (out[0].empty()) {
    return false;
  }
  EXPECT_EQ(1u, out[0].size());
  EXPECT_EQ(frame, out[0][0]);
  return true;
}

// Ether / IPv4 10.0.0.5 -> 20.0.0.1 / UDP 1000 -> 2000, no padding (42
// bytes); `frag` as on the wire.
Bytes Udp(uint16_t frag = 0) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("10.0.0.5", "20.0.0.1");
  b.Last<PacketBuilder::IPv4Hdr>().frag = frag;
  return b.Udp(1000, 2000).Build();
}

// Every field a wildcard: forwards every frame ACL can judge.
bess::pb::ACLArg PassAll() {
  return Rules({{}});
}

// ARP and IPv6 carry no IPv4 fields, so a rule that names none (every field
// a wildcard: "" or /0, port 0) matches them and one that names any does
// not; first-match order still holds. Under the old code the first three
// held only by accident (garbage at the IPv4 offsets matched the wildcards).
TEST(AclTest, AFrameThatIsNotIpv4TakesTheFirstAllWildcardRule) {
  ModuleHarness h;
  const Bytes arp = PacketBuilder()
                        .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:01")
                        .Arp(1, "02:00:00:00:00:01", "10.0.0.5", "00:00:00:00:00:00", "10.0.0.1")
                        .PadTo(60)
                        .Build();
  const Bytes ipv6 = PacketBuilder()
                         .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                         .IPv6("2001:db8::a00:5", "2001:db8::1")
                         .Udp(1000, 2000)
                         .Build();
  Module *all = h.Create("ACL", PassAll());
  ASSERT_NE(nullptr, all);
  EXPECT_TRUE(Passes(h, all, arp));
  EXPECT_TRUE(Passes(h, all, ipv6));
  Module *zero = h.Create("ACL", Rules({{.src_ip = "0.0.0.0/0", .dst_ip = "0.0.0.0/0"}}));
  ASSERT_NE(nullptr, zero);
  EXPECT_TRUE(Passes(h, zero, arp)) << "a /0 prefix is a wildcard";
  Module *drop_all = h.Create("ACL", Rules({{.drop = true}, {}}));
  ASSERT_NE(nullptr, drop_all);
  EXPECT_FALSE(Passes(h, drop_all, arp)) << "the first all-wildcard rule drops";
  Module *named_first = h.Create("ACL", Rules({{.src_ip = "10.0.0.0/8", .drop = true}, {}}));
  ASSERT_NE(nullptr, named_first);
  EXPECT_TRUE(Passes(h, named_first, arp)) << "a rule naming a field does not match ARP";
  EXPECT_TRUE(Passes(h, named_first, ipv6));
}

// A frame that is not IPv4 and that no all-wildcard rule matches is dropped
// (counted), as any packet no rule matches. The IPv6 source here carries
// 10.0.0.5 in bytes 26-29, where the old code read an IPv4 source address
// and passed the frame by the 10.0.0.0/8 rule.
TEST(AclTest, AFrameThatIsNotIpv4IsDroppedWithoutAnAllWildcardRule) {
  ModuleHarness h;
  Module *ten = h.Create("ACL", Rules({{.src_ip = "10.0.0.0/8"}, {.dst_port = 2000}}));
  ASSERT_NE(nullptr, ten);
  const Bytes arp = PacketBuilder()
                        .Ether("ff:ff:ff:ff:ff:ff", "02:00:00:00:00:01")
                        .Arp(1, "02:00:00:00:00:01", "10.0.0.5", "00:00:00:00:00:00", "10.0.0.1")
                        .PadTo(60)
                        .Build();
  const Bytes ipv6 = PacketBuilder()
                         .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                         .IPv6("2001:db8:a00:5::1", "2001:db8::1")
                         .Udp(1000, 2000)
                         .Build();
  ASSERT_EQ((Bytes{10, 0, 0, 5}), Bytes(ipv6.begin() + 26, ipv6.begin() + 30));
  const uint64_t dropped = ten->deadends();
  EXPECT_FALSE(Passes(h, ten, arp));
  EXPECT_FALSE(Passes(h, ten, ipv6));
  EXPECT_EQ(dropped + 2, ten->deadends());
  EXPECT_TRUE(Passes(h, ten, Udp()));
}

// A frame the parse calls malformed is dropped before any rule, even an
// all-wildcard allow: here an IPv4 total length past the frame. The old
// code passed it.
TEST(AclTest, AMalformedFrameIsDroppedEvenByAnAllWildcardRule) {
  ModuleHarness h;
  Module *all = h.Create("ACL", PassAll());
  ASSERT_NE(nullptr, all);
  Bytes lying = Udp();
  lying[16] = 0x03;  // total length 0x03xx: past the 42-byte frame
  EXPECT_FALSE(Passes(h, all, lying));
  EXPECT_TRUE(Passes(h, all, Udp()));
}

// The headers must be whole in the frame: a UDP frame cut short anywhere
// before the end of its UDP header (1-41 bytes) is malformed and dropped,
// even by a rule with every field a wildcard; the 42-byte frame passes. The
// old code passed every one, judged on bytes past the frame.
TEST(AclTest, AFrameShorterThanItsHeadersIsDropped) {
  ModuleHarness h;
  Module *acl = h.Create("ACL", PassAll());
  ASSERT_NE(nullptr, acl);
  const Bytes full = Udp();
  ASSERT_EQ(42u, full.size());
  for (size_t len = 1; len < full.size(); len++) {
    EXPECT_FALSE(Passes(h, acl, Bytes(full.begin(), full.begin() + len))) << len << " bytes";
  }
  EXPECT_TRUE(Passes(h, acl, full));
}

// Only the first segment counts (D-121): a chain split inside the Ethernet,
// IPv4 or UDP header is dropped; one split after the UDP header passes. The
// old code passed every one.
TEST(AclTest, AChainSplitInsideTheHeadersIsDropped) {
  ModuleHarness h;
  Module *acl = h.Create("ACL", PassAll());
  ASSERT_NE(nullptr, acl);
  const Bytes frame = PacketBuilder()
                          .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                          .IPv4("10.0.0.5", "20.0.0.1")
                          .Udp(1000, 2000)
                          .PadTo(60)
                          .Build();
  for (size_t split : {6u, 14u, 20u, 26u, 34u, 38u, 41u}) {
    EXPECT_FALSE(PassesChain(h, acl, frame, split)) << "first segment of " << split << " bytes";
  }
  for (size_t split : {42u, 50u}) {
    EXPECT_TRUE(PassesChain(h, acl, frame, split)) << "first segment of " << split << " bytes";
  }
}

// Ports are TCP's and UDP's only. A non-first fragment has no UDP header:
// its ports are 0, so a rule on port 2000 does not match it (a wildcard rule
// does). ICMP has no ports: an echo request's type and code (0x0800) are not
// a source port 2048. The old code read both as ports and passed them.
TEST(AclTest, OnlyTcpAndUdpHavePorts) {
  ModuleHarness h;
  Module *port = h.Create("ACL", Rules({{.dst_port = 2000}}));
  ASSERT_NE(nullptr, port);
  Module *icmp_port = h.Create("ACL", Rules({{.src_port = 2048}}));
  ASSERT_NE(nullptr, icmp_port);
  Module *all = h.Create("ACL", PassAll());
  ASSERT_NE(nullptr, all);

  EXPECT_TRUE(Passes(h, port, Udp()));
  EXPECT_TRUE(Passes(h, port, Udp(0x2000))) << "a first fragment has its UDP header";
  const Bytes later = Udp(0x0010);  // offset 128 bytes
  EXPECT_FALSE(Passes(h, port, later));
  EXPECT_TRUE(Passes(h, all, later));
  EXPECT_TRUE(Passes(h, port,
                     PacketBuilder()
                         .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                         .IPv4("10.0.0.5", "20.0.0.1")
                         .Tcp(1000, 2000)
                         .PadTo(60)
                         .Build()));

  const Bytes echo = PacketBuilder()
                         .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                         .IPv4("10.0.0.5", "20.0.0.1")
                         .Icmp(8, 0)
                         .PadTo(60)
                         .Build();
  EXPECT_FALSE(Passes(h, icmp_port, echo));
  EXPECT_TRUE(Passes(h, all, echo));
}

// The parse finds IPv4 behind a VLAN tag, behind two (QinQ) and past IPv4
// options. The old code read a tagged frame's addresses 4 or 8 bytes early
// and dropped it.
TEST(AclTest, TaggedFramesAndOptionsAreJudged) {
  ModuleHarness h;
  Module *acl = h.Create("ACL", Rules({{.src_ip = "10.0.0.0/8", .dst_port = 2000}}));
  ASSERT_NE(nullptr, acl);
  const Bytes tagged = PacketBuilder()
                           .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                           .Dot1Q(7)
                           .IPv4("10.0.0.5", "20.0.0.1")
                           .Udp(1000, 2000)
                           .PadTo(64)
                           .Build();
  EXPECT_TRUE(Passes(h, acl, tagged));
  const Bytes qinq = PacketBuilder()
                         .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                         .Dot1AD(5)
                         .Dot1Q(7)
                         .IPv4("10.0.0.5", "20.0.0.1")
                         .Udp(1000, 2000)
                         .PadTo(68)
                         .Build();
  EXPECT_TRUE(Passes(h, acl, qinq));
  PacketBuilder options;
  options.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("10.0.0.5", "20.0.0.1");
  options.Last<PacketBuilder::IPv4Hdr>().options = Bytes{1, 1, 1, 0};  // NOP NOP NOP EOL
  EXPECT_TRUE(Passes(h, acl, options.Udp(1000, 2000).PadTo(64).Build()));
}

// Every packet leaves once: N in = N out + dropped, over a mix of passed,
// rule-dropped, short and non-IPv4 frames.
TEST(AclTest, EveryPacketIsForwardedOrDropped) {
  ModuleHarness h;
  Module *acl = h.Create("ACL", Rules({{.src_ip = "10.0.0.0/8"}}));
  ASSERT_NE(nullptr, acl);
  const Bytes udp = Udp();
  PacketBuilder other;
  other.Ether("02:00:00:00:00:02", "02:00:00:00:00:01").IPv4("11.0.0.5", "20.0.0.1");
  const std::vector<Bytes> in = {
      udp,
      other.Udp(1, 2).Build(),
      Bytes(udp.begin(), udp.begin() + 20),
      PacketBuilder()
          .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
          .IPv6("::1", "::2")
          .Udp(1, 2)
          .Build(),
      udp,
  };
  const auto out = h.Run(acl, 0, in, {0});
  ASSERT_EQ(1u, out.count(0));
  EXPECT_EQ(2u, out.at(0).size());
  EXPECT_EQ(in.size(), out.at(0).size() + acl->deadends());
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

  const bess::pb::ACLArg none;
  EXPECT_EQ("", h.SetResource(rules, none));
  EXPECT_FALSE(Passes(h, acl, a));
  EXPECT_FALSE(Passes(h, acl, b));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{nullptr, &none}}));
}

}  // namespace
