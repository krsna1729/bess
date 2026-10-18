// SPDX-License-Identifier: BSD-3-Clause

// The firewall table in process: the policy a rule list means, checked
// against a scalar reference over random rule sets and keys of both families,
// plus the properties the design rests on - a port range costs no group (what
// a WildcardMatch rule cannot say without spending its tuple budget), a v4
// rule never sees a v6 key, and a rule the table cannot express exactly is
// refused rather than widened.

#include <gtest/gtest.h>
#include <netinet/in.h>

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "classifier/firewall_table.h"
#include "testing/rng.h"

namespace {

using bess::classifier::FirewallAddr;
using bess::classifier::FirewallFamily;
using bess::classifier::FirewallKey;
using bess::classifier::FirewallRule;
using bess::classifier::FirewallScan;
using bess::classifier::FirewallTable;

constexpr FirewallFamily k4 = FirewallFamily::kIpv4;
constexpr FirewallFamily k6 = FirewallFamily::kIpv6;
constexpr FirewallFamily kAny = FirewallFamily::kAny;
constexpr uint8_t kTcp = 6;
constexpr uint8_t kUdp = 17;

FirewallAddr Addr(FirewallFamily family, const std::string &text) {
  FirewallAddr out{};
  if (family == k4) {
    in_addr v4{};
    EXPECT_EQ(1, inet_pton(AF_INET, text.c_str(), &v4)) << text;
    std::memcpy(out.data(), &v4, 4);
  } else {
    in6_addr v6{};
    EXPECT_EQ(1, inet_pton(AF_INET6, text.c_str(), &v6)) << text;
    std::memcpy(out.data(), &v6, 16);
  }
  return out;
}

// A rule: pass by default, so a test reads the winner as a rule id.
FirewallRule Rule(FirewallFamily family, const std::string &src, uint8_t src_len,
                  const std::string &dst, uint8_t dst_len, uint8_t proto = 0,
                  uint16_t src_low = 0, uint16_t src_high = 0xffff,
                  uint16_t dst_low = 0, uint16_t dst_high = 0xffff,
                  uint16_t action = 1) {
  FirewallRule r;
  r.family = family;
  if (!src.empty()) {
    r.src_net = Addr(family, src);
  }
  r.src_len = src_len;
  if (!dst.empty()) {
    r.dst_net = Addr(family, dst);
  }
  r.dst_len = dst_len;
  r.proto = proto;
  r.src_port_low = src_low;
  r.src_port_high = src_high;
  r.dst_port_low = dst_low;
  r.dst_port_high = dst_high;
  r.action = action;
  return r;
}

FirewallKey Key4(const std::string &src, const std::string &dst, uint8_t proto,
                 uint16_t sport, uint16_t dport) {
  FirewallKey key;
  key.family = k4;
  key.src = Addr(k4, src);
  key.dst = Addr(k4, dst);
  key.proto = proto;
  key.src_port = sport;
  key.dst_port = dport;
  return key;
}

FirewallKey Key6(const std::string &src, const std::string &dst, uint8_t proto,
                 uint16_t sport, uint16_t dport) {
  FirewallKey key;
  key.family = k6;
  key.src = Addr(k6, src);
  key.dst = Addr(k6, dst);
  key.proto = proto;
  key.src_port = sport;
  key.dst_port = dport;
  return key;
}

std::unique_ptr<const FirewallTable> Built(std::vector<FirewallRule> rules) {
  auto table = FirewallTable::Build(std::move(rules));
  EXPECT_TRUE(table.has_value()) << (table ? "" : table.error());
  return table ? std::move(*table) : nullptr;
}

// The winning rule id, or 0.
uint32_t Winner(const FirewallTable &table, const FirewallKey &key) {
  FirewallTable::Match match;
  return table.Lookup(key, &match) ? match.rule_id : 0;
}

// What the list of rules means, straight: the first rule that matches wins.
// Deliberately the slow, obvious reading - it is the definition the index must
// reproduce, not a second implementation of the index.
uint32_t Reference(const std::vector<FirewallRule> &rules,
                   const FirewallKey &key) {
  for (size_t i = 0; i < rules.size(); i++) {
    const FirewallRule &r = rules[i];
    if (r.family != kAny && r.family != key.family) {
      continue;
    }
    if (r.proto != 0 && r.proto != key.proto) {
      continue;
    }
    if (r.src_len != 0 &&
        FirewallTable::MaskedAddr(key.src, r.src_len, r.family) !=
            FirewallTable::MaskedAddr(r.src_net, r.src_len, r.family)) {
      continue;
    }
    if (r.dst_len != 0 &&
        FirewallTable::MaskedAddr(key.dst, r.dst_len, r.family) !=
            FirewallTable::MaskedAddr(r.dst_net, r.dst_len, r.family)) {
      continue;
    }
    if (key.src_port < r.src_port_low || key.src_port > r.src_port_high) {
      continue;
    }
    if (key.dst_port < r.dst_port_low || key.dst_port > r.dst_port_high) {
      continue;
    }
    return static_cast<uint32_t>(i) + 1;
  }
  return 0;
}

TEST(FirewallTableTest, FirstMatchWinsInPolicyOrder) {
  const FirewallKey inside = Key4("10.1.2.3", "20.0.0.1", kTcp, 1000, 80);
  // Inside the broad rule's network but outside the narrow rule's.
  const FirewallKey outside = Key4("10.9.9.9", "20.0.0.1", kTcp, 1000, 80);

  // The narrow rule first: it wins for traffic it covers.
  auto narrow_first =
      Built({Rule(k4, "10.1.0.0", 16, "", 0, kTcp, 0, 0xffff, 80, 80),
             Rule(k4, "10.0.0.0", 8, "", 0)});
  ASSERT_NE(nullptr, narrow_first);
  EXPECT_EQ(1u, Winner(*narrow_first, inside));
  EXPECT_EQ(2u, Winner(*narrow_first, outside));

  // The same rules the other way round: the broad one wins for both.
  auto broad_first =
      Built({Rule(k4, "10.0.0.0", 8, "", 0),
             Rule(k4, "10.1.0.0", 16, "", 0, kTcp, 0, 0xffff, 80, 80)});
  ASSERT_NE(nullptr, broad_first);
  EXPECT_EQ(1u, Winner(*broad_first, inside));
  EXPECT_EQ(1u, Winner(*broad_first, outside));
}

TEST(FirewallTableTest, PrefixesMatchTheirNetwork) {
  // A sloppy prefix means its network (10.1.2.3/8 is 10/8), /32 is exact, /0
  // is any address of the family.
  auto sloppy = Built({Rule(k4, "10.1.2.3", 8, "", 0)});
  ASSERT_NE(nullptr, sloppy);
  EXPECT_EQ(1u, Winner(*sloppy, Key4("10.9.9.9", "8.8.8.8", kTcp, 1, 2)));
  EXPECT_EQ(0u, Winner(*sloppy, Key4("11.9.9.9", "8.8.8.8", kTcp, 1, 2)));

  auto host = Built({Rule(k4, "10.1.2.3", 32, "", 0)});
  ASSERT_NE(nullptr, host);
  EXPECT_EQ(1u, Winner(*host, Key4("10.1.2.3", "8.8.8.8", kTcp, 1, 2)));
  EXPECT_EQ(0u, Winner(*host, Key4("10.1.2.4", "8.8.8.8", kTcp, 1, 2)));

  auto any = Built({Rule(k4, "0.0.0.0", 0, "", 0)});
  ASSERT_NE(nullptr, any);
  EXPECT_EQ(1u, Winner(*any, Key4("1.2.3.4", "5.6.7.8", kUdp, 9, 9)));

  // Both fields at once: a source network and a destination network.
  auto pair = Built({Rule(k4, "10.0.0.0", 8, "20.0.0.0", 16)});
  ASSERT_NE(nullptr, pair);
  EXPECT_EQ(1u, Winner(*pair, Key4("10.1.1.1", "20.0.5.5", kTcp, 1, 2)));
  EXPECT_EQ(0u, Winner(*pair, Key4("10.1.1.1", "21.0.5.5", kTcp, 1, 2)));
}

TEST(FirewallTableTest, PortRangesAreInclusive) {
  auto table = Built({Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 1000, 2000, 80, 90)});
  ASSERT_NE(nullptr, table);
  EXPECT_EQ(1u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 1000, 80)));
  EXPECT_EQ(1u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 2000, 90)));
  EXPECT_EQ(0u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 999, 80)));
  EXPECT_EQ(0u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 2001, 90)));
  EXPECT_EQ(0u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 1500, 79)));
  EXPECT_EQ(0u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 1500, 91)));
  // The protocol must agree too, and a rule with no protocol accepts both.
  EXPECT_EQ(0u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kUdp, 1500, 85)));
  auto any_proto =
      Built({Rule(k4, "10.0.0.0", 8, "", 0, 0, 1000, 2000, 80, 90)});
  ASSERT_NE(nullptr, any_proto);
  EXPECT_EQ(1u, Winner(*any_proto, Key4("10.0.0.1", "1.1.1.1", kUdp, 1500, 85)));
}

TEST(FirewallTableTest, PortRangesCostNoGroup) {
  // The same prefixes and protocol, once with exact ports and once with wide
  // ranges: a range must not add a group (nor an entry), because the ports are
  // checked after the masked key and never enter the index. This is the
  // property a WildcardMatch rule cannot have: it must write a range as 2n-2
  // prefixes per field, and pays for each one in its tuple budget (D-102).
  const std::vector<FirewallRule> exact = {
      Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 80, 80),
      Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 443, 443),
  };
  const std::vector<FirewallRule> ranges = {
      Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 80, 200),
      Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 300, 4000),
  };
  auto a = Built(exact);
  auto b = Built(ranges);
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  EXPECT_EQ(a->stats().groups, b->stats().groups);
  EXPECT_EQ(1u, a->stats().groups);

  // One group per (family, protocol, source length, destination length), and
  // rules that share all four share their group whatever their ports are.
  auto two_protos = Built({Rule(k4, "10.0.0.0", 8, "", 0, kTcp),
                           Rule(k4, "10.0.0.0", 8, "", 0, kUdp),
                           Rule(k4, "10.0.0.0", 8, "", 0, 0),
                           Rule(k4, "10.0.0.0", 8, "20.0.0.0", 16, kTcp),
                           Rule(k4, "10.0.0.0", 24, "", 0, kTcp)});
  ASSERT_NE(nullptr, two_protos);
  EXPECT_EQ(5u, two_protos->stats().groups);
  EXPECT_EQ(5u, two_protos->stats().rules);
  EXPECT_EQ(5u, two_protos->stats().ipv4_rules);
  EXPECT_EQ(0u, two_protos->stats().ipv6_rules);
}

TEST(FirewallTableTest, FamiliesDoNotMix) {
  const FirewallKey v4 = Key4("10.0.0.1", "1.1.1.1", kTcp, 1000, 80);
  const FirewallKey v6 = Key6("2001:db8::1", "2001:db8:1::1", kTcp, 1000, 80);

  // A v6 rule sees v6 packets only, even when the addresses could be read as
  // the same bytes; and the other way round.
  auto only6 = Built({Rule(k6, "2001:db8::", 32, "", 0)});
  ASSERT_NE(nullptr, only6);
  EXPECT_EQ(1u, Winner(*only6, v6));
  EXPECT_EQ(0u, Winner(*only6, v4));
  EXPECT_FALSE(only6->HasFamily(k4));
  EXPECT_TRUE(only6->HasFamily(k6));

  auto only4 = Built({Rule(k4, "10.0.0.0", 8, "", 0)});
  ASSERT_NE(nullptr, only4);
  EXPECT_EQ(1u, Winner(*only4, v4));
  EXPECT_EQ(0u, Winner(*only4, v6));
  EXPECT_TRUE(only4->HasFamily(k4));
  EXPECT_FALSE(only4->HasFamily(k6));

  // A rule with no prefix at all names no family and matches both.
  auto both = Built({Rule(kAny, "", 0, "", 0)});
  ASSERT_NE(nullptr, both);
  EXPECT_EQ(1u, Winner(*both, v4));
  EXPECT_EQ(1u, Winner(*both, v6));
  EXPECT_TRUE(both->HasFamily(k4));
  EXPECT_TRUE(both->HasFamily(k6));
  EXPECT_EQ(1u, both->stats().any_family_rules);

  // One policy, both families, in rule order across the families.
  auto mixed = Built({Rule(k4, "10.0.0.0", 8, "", 0, 0, 0, 0xffff, 0, 0xffff, 7),
                      Rule(k6, "2001:db8::", 32, "", 0, 0, 0, 0xffff, 0, 0xffff,
                           9)});
  ASSERT_NE(nullptr, mixed);
  FirewallTable::Match match;
  ASSERT_TRUE(mixed->Lookup(v4, &match));
  EXPECT_EQ(7u, match.action);
  ASSERT_TRUE(mixed->Lookup(v6, &match));
  EXPECT_EQ(9u, match.action);
  EXPECT_EQ(2u, mixed->stats().groups);
  EXPECT_EQ(1u, mixed->stats().ipv4_rules);
  EXPECT_EQ(1u, mixed->stats().ipv6_rules);
}

TEST(FirewallTableTest, RefusesWhatItCannotExpressExactly) {
  // A prefix length beyond the family's width: refused, not clipped.
  EXPECT_FALSE(FirewallTable::Build({Rule(k4, "10.0.0.0", 33, "", 0)})
                   .has_value());
  EXPECT_FALSE(FirewallTable::Build({Rule(k4, "", 0, "10.0.0.0", 33)})
                   .has_value());
  EXPECT_FALSE(FirewallTable::Build({Rule(k6, "2001:db8::", 129, "", 0)})
                   .has_value());
  // A port range with its low bound above its high.
  EXPECT_FALSE(FirewallTable::Build({Rule(k4, "", 0, "", 0, 0, 90, 80)})
                   .has_value());
  EXPECT_FALSE(FirewallTable::Build({Rule(k4, "", 0, "", 0, 0, 0, 0xffff, 443, 80)})
                   .has_value());
  // A rule that names no family cannot carry a prefix: there is no width to
  // read the length against.
  EXPECT_FALSE(FirewallTable::Build({Rule(kAny, "", 8, "", 0)}).has_value());
  EXPECT_FALSE(FirewallTable::Build({Rule(kAny, "", 0, "", 16)}).has_value());

  // The refusal names the rule, and the accepted lengths are exactly the
  // widths.
  auto refusal = FirewallTable::Build({Rule(k4, "10.0.0.0", 8, "", 0),
                                       Rule(k6, "2001:db8::", 129, "", 0)});
  ASSERT_FALSE(refusal.has_value());
  EXPECT_NE(std::string::npos, refusal.error().find("rule 2")) << refusal.error();
  EXPECT_TRUE(FirewallTable::Build({Rule(k4, "10.0.0.0", 32, "", 0)}).has_value());
  EXPECT_TRUE(FirewallTable::Build({Rule(k6, "2001:db8::", 128, "", 0)})
                  .has_value());
  EXPECT_TRUE(FirewallTable::Build({}).has_value());
}

TEST(FirewallTableTest, BytesBeyondTheFamilyWidthAreIgnored) {
  // A caller that hands a v4 address with junk behind it gets the same table
  // as one that does not: Build masks to the family's width.
  FirewallRule dirty = Rule(k4, "10.0.0.0", 8, "", 0);
  FirewallRule clean = dirty;
  for (size_t i = 4; i < 16; i++) {
    dirty.src_net[i] = 0xab;
    dirty.dst_net[i] = 0xcd;
  }
  auto a = Built({dirty});
  auto b = Built({clean});
  ASSERT_NE(nullptr, a);
  ASSERT_NE(nullptr, b);
  EXPECT_EQ(1u, Winner(*a, Key4("10.1.1.1", "1.1.1.1", kTcp, 1, 2)));
  EXPECT_EQ(Winner(*b, Key4("10.1.1.1", "1.1.1.1", kTcp, 1, 2)),
            Winner(*a, Key4("10.1.1.1", "1.1.1.1", kTcp, 1, 2)));
}

TEST(FirewallTableTest, PrefixLengthsAreDistinctMasksInOnePolicy) {
  // Four prefix lengths of the same address space in one policy: four
  // groups, four different masks, and the narrowest rule first, so the
  // winner's depth says which mask the lookup actually applied.
  auto table = Built({
      Rule(k4, "10.1.2.3", 32, "", 0),
      Rule(k4, "10.1.2.0", 24, "", 0),
      Rule(k4, "10.1.0.0", 16, "", 0),
      Rule(k4, "10.0.0.0", 8, "", 0),
  });
  ASSERT_NE(nullptr, table);
  EXPECT_EQ(4u, table->stats().groups);
  EXPECT_EQ(1u, Winner(*table, Key4("10.1.2.3", "1.1.1.1", kTcp, 1, 2)));
  EXPECT_EQ(2u, Winner(*table, Key4("10.1.2.9", "1.1.1.1", kTcp, 1, 2)));
  EXPECT_EQ(3u, Winner(*table, Key4("10.1.9.9", "1.1.1.1", kTcp, 1, 2)));
  EXPECT_EQ(4u, Winner(*table, Key4("10.9.9.9", "1.1.1.1", kTcp, 1, 2)));
  EXPECT_EQ(0u, Winner(*table, Key4("11.9.9.9", "1.1.1.1", kTcp, 1, 2)));
  // A mask on the destination side only, in the same table: the fifth group.
  auto with_dst = Built({
      Rule(k4, "", 0, "20.0.0.0", 24),
      Rule(k4, "10.0.0.0", 8, "", 0),
  });
  ASSERT_NE(nullptr, with_dst);
  EXPECT_EQ(2u, with_dst->stats().groups);
  EXPECT_EQ(1u, Winner(*with_dst, Key4("30.0.0.1", "20.0.0.9", kTcp, 1, 2)));
  EXPECT_EQ(2u, Winner(*with_dst, Key4("10.0.0.1", "30.0.0.9", kTcp, 1, 2)));
  EXPECT_EQ(0u, Winner(*with_dst, Key4("30.0.0.1", "30.0.0.9", kTcp, 1, 2)));
}

TEST(FirewallTableTest, OneKeyDecidesByPortsAndProtocol) {
  // The same prefixes, protocol and family in one group: one key for all of
  // them, and the port range (then the rule order) decides the winner - the
  // case that must not go through the index.
  auto table = Built({
      Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 80, 90),
      Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 443, 443),
      Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 80, 80),
  });
  ASSERT_NE(nullptr, table);
  EXPECT_EQ(1u, table->stats().groups);
  EXPECT_EQ(1u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 1, 85)));
  EXPECT_EQ(2u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 1, 443)));
  // 80 is inside the first rule's range too, and the first rule wins.
  EXPECT_EQ(1u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 1, 80)));
  // Outside every range (81 would still be inside the first rule's).
  EXPECT_EQ(0u, Winner(*table, Key4("10.0.0.1", "1.1.1.1", kTcp, 1, 100)));
  // A source port range instead: the same group, the other mask.
  auto by_src = Built({
      Rule(k4, "10.0.0.0", 8, "", 0, kUdp, 5000, 6000, 0, 0xffff),
      Rule(k4, "10.0.0.0", 8, "", 0, kUdp),
  });
  ASSERT_NE(nullptr, by_src);
  EXPECT_EQ(1u, Winner(*by_src, Key4("10.0.0.1", "1.1.1.1", kUdp, 5500, 9)));
  EXPECT_EQ(2u, Winner(*by_src, Key4("10.0.0.1", "1.1.1.1", kUdp, 4000, 9)));
}

TEST(FirewallTableTest, AKeyWithUnknownPortsOnlyMeetsAWideRule) {
  // A fragment (a packet whose ports cannot be read) is looked up with both
  // ports 0, which is a value like any other: only a rule whose range starts
  // at 0 can match it.
  auto table = Built({Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 80, 90),
                      Rule(k4, "10.0.0.0", 8, "", 0, kTcp)});
  ASSERT_NE(nullptr, table);
  EXPECT_EQ(2u, Winner(*table, Key4("10.1.1.1", "1.1.1.1", kTcp, 0, 0)));
  EXPECT_EQ(1u, Winner(*table, Key4("10.1.1.1", "1.1.1.1", kTcp, 0, 85)));
}

TEST(FirewallTableTest, LookupBatchReportsMaskActionAndRule) {
  auto table = Built({Rule(k4, "10.0.0.0", 8, "", 0, kTcp, 0, 0xffff, 80, 80, 3),
                      Rule(k6, "2001:db8::", 32, "", 0, 0, 0, 0xffff, 0, 0xffff,
                           5)});
  ASSERT_NE(nullptr, table);
  const FirewallKey keys[4] = {
      Key4("10.1.1.1", "1.1.1.1", kTcp, 1000, 80),  // rule 1
      Key4("11.1.1.1", "1.1.1.1", kTcp, 1000, 80),  // nothing
      Key6("2001:db8::1", "2001:db8::2", kUdp, 1, 2),  // rule 2
      Key4("10.1.1.1", "1.1.1.1", kUdp, 1000, 80),  // wrong protocol
  };
  FirewallTable::Match matches[4];
  const uint64_t matched = table->LookupBatch(keys, 4, matches, true);
  EXPECT_EQ(0b0101u, matched);
  EXPECT_EQ(3u, matches[0].action);
  EXPECT_EQ(1u, matches[0].rule_id);
  EXPECT_EQ(5u, matches[2].action);
  EXPECT_EQ(2u, matches[2].rule_id);

  // Without rule ids the match's id is left as the caller had it (the packet
  // path does not read it).
  FirewallTable::Match only_action[4];
  for (auto &m : only_action) {
    m.rule_id = 77;
  }
  EXPECT_EQ(0b0101u, table->LookupBatch(keys, 4, only_action, false));
  EXPECT_EQ(3u, only_action[0].action);
  EXPECT_EQ(77u, only_action[0].rule_id);
  EXPECT_EQ(77u, only_action[2].rule_id);
}

// One random rule and one random key from the corpus the reference test walks:
// the index and the scan are judged on the same inputs, so their agreement is
// not a coincidence of two generators.
FirewallRule RandomRule(std::mt19937 &rng) {
  std::uniform_int_distribution<int> family_pick(0, 2);
  std::uniform_int_distribution<int> proto_pick(0, 2);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<int> port(0, 65535);
  std::uniform_int_distribution<int> action(0, 3);

  FirewallRule r;
  const int pick = family_pick(rng);
  r.family = pick == 0 ? k4 : (pick == 1 ? k6 : kAny);
  // A family with no prefix at all cannot carry a length (the table refuses
  // that), so such a rule stays at 0.
  if (r.family != kAny) {
    r.src_len = static_cast<uint8_t>(rng() % (r.family == k4 ? 33 : 129));
    r.dst_len = static_cast<uint8_t>(rng() % (r.family == k4 ? 33 : 129));
    for (size_t b = 0; b < 16; b++) {
      r.src_net[b] = static_cast<uint8_t>(byte(rng));
      r.dst_net[b] = static_cast<uint8_t>(byte(rng));
    }
  }
  const int p = proto_pick(rng);
  r.proto = p == 0 ? 0 : (p == 1 ? kTcp : kUdp);
  const bool wide_src = rng() % 3 == 0;
  const bool wide_dst = rng() % 3 == 0;
  if (!wide_src) {
    r.src_port_low = static_cast<uint16_t>(port(rng));
    r.src_port_high = static_cast<uint16_t>(
        std::min<int>(0xffff, r.src_port_low + static_cast<int>(rng() % 500)));
  }
  if (!wide_dst) {
    r.dst_port_low = static_cast<uint16_t>(port(rng));
    r.dst_port_high = static_cast<uint16_t>(
        std::min<int>(0xffff, r.dst_port_low + static_cast<int>(rng() % 500)));
  }
  r.action = static_cast<uint16_t>(action(rng));
  return r;
}

FirewallKey RandomKey(std::mt19937 &rng) {
  std::uniform_int_distribution<int> proto_pick(0, 2);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<int> port(0, 65535);

  FirewallKey key;
  const bool v6 = rng() % 2 == 0;
  key.family = v6 ? k6 : k4;
  for (size_t b = 0; b < 16; b++) {
    // A v4 key's addresses are four bytes wide, as a packet's are.
    key.src[b] = v6 || b < 4 ? static_cast<uint8_t>(byte(rng)) : 0;
    key.dst[b] = v6 || b < 4 ? static_cast<uint8_t>(byte(rng)) : 0;
  }
  const int p = proto_pick(rng);
  key.proto = p == 0 ? 0 : (p == 1 ? kTcp : kUdp);
  key.src_port = static_cast<uint16_t>(port(rng));
  key.dst_port = static_cast<uint16_t>(port(rng));
  return key;
}

TEST(FirewallTableTest, MatchesTheScalarReferenceOverRandomRuleSets) {
  std::mt19937 rng = bess::testing::SeededRng(20261018);

  for (int round = 0; round < 200; round++) {
    const int count = 1 + static_cast<int>(rng() % 12);
    std::vector<FirewallRule> rules;
    rules.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
      rules.push_back(RandomRule(rng));
    }

    auto table = FirewallTable::Build(rules);
    ASSERT_TRUE(table.has_value()) << table.error();
    for (int k = 0; k < 200; k++) {
      const FirewallKey key = RandomKey(rng);

      FirewallTable::Match match;
      const bool found = (*table)->Lookup(key, &match);
      const uint32_t want = Reference(rules, key);
      ASSERT_EQ(want != 0, found) << "round " << round << " key " << k;
      if (want != 0) {
        EXPECT_EQ(want, match.rule_id) << "round " << round << " key " << k;
        EXPECT_EQ(rules[want - 1].action, match.action);
      }
    }
  }
}

// The scan is a second implementation of the same answer, so the test that
// matters is agreement: with the index, with the scalar reference, and about
// which policies are expressible at all. A policy the index refuses the scan
// must refuse with the same message, or the two would disagree about what a
// caller may program.
TEST(FirewallTableTest, TheScanAgreesWithTheIndexOverRandomRuleSets) {
  std::mt19937 rng = bess::testing::SeededRng(20261019);

  for (int round = 0; round < 200; round++) {
    const int count = 1 + static_cast<int>(rng() % 12);
    std::vector<FirewallRule> rules;
    rules.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
      rules.push_back(RandomRule(rng));
    }

    auto table = FirewallTable::Build(rules);
    auto scan = FirewallScan::Create(rules);
    ASSERT_EQ(table.has_value(), scan.has_value())
        << "round " << round << ": the index and the scan disagree about the "
        << "policy";
    if (!table.has_value()) {
      EXPECT_EQ(table.error(), scan.error()) << "round " << round;
      continue;
    }

    EXPECT_EQ((*table)->rule_count(), scan->rule_count());
    EXPECT_EQ((*table)->HasFamily(k4), scan->HasFamily(k4));
    EXPECT_EQ((*table)->HasFamily(k6), scan->HasFamily(k6));

    for (int k = 0; k < 200; k++) {
      const FirewallKey key = RandomKey(rng);

      FirewallTable::Match indexed;
      FirewallTable::Match scanned;
      const bool found_indexed = (*table)->Lookup(key, &indexed);
      const bool found_scanned = scan->Lookup(key, &scanned);
      const uint32_t want = Reference(rules, key);
      // The reference is the arbiter, so it is asserted first: when the two
      // structures disagree, the failure says which one is wrong.
      ASSERT_EQ(want != 0, found_scanned) << "round " << round << " key " << k;
      ASSERT_EQ(want != 0, found_indexed) << "round " << round << " key " << k;
      ASSERT_EQ(found_indexed, found_scanned)
          << "round " << round << " key " << k;
      if (want != 0) {
        EXPECT_EQ(want, scanned.rule_id) << "round " << round << " key " << k;
        EXPECT_EQ(indexed.rule_id, scanned.rule_id);
        EXPECT_EQ(indexed.action, scanned.action);
        EXPECT_EQ(rules[want - 1].action, scanned.action);
      }
    }

    // A batch: the same mask, the same actions, the same ids - and a key
    // nothing matches is left untouched by both.
    std::array<FirewallKey, 64> keys;
    for (FirewallKey &key : keys) {
      key = RandomKey(rng);
    }
    std::array<FirewallTable::Match, 64> indexed_matches;
    std::array<FirewallTable::Match, 64> scanned_matches;
    for (size_t i = 0; i < keys.size(); i++) {
      indexed_matches[i] = FirewallTable::Match{0xffff, 0xffffffff};
      scanned_matches[i] = FirewallTable::Match{0xffff, 0xffffffff};
    }
    const uint64_t indexed_mask = (*table)->LookupBatch(
        keys.data(), keys.size(), indexed_matches.data());
    const uint64_t scanned_mask = scan->LookupBatch(
        keys.data(), keys.size(), scanned_matches.data());
    EXPECT_EQ(indexed_mask, scanned_mask) << "round " << round;
    for (size_t i = 0; i < keys.size(); i++) {
      EXPECT_EQ(indexed_matches[i].action, scanned_matches[i].action)
          << "round " << round << " key " << i;
      EXPECT_EQ(indexed_matches[i].rule_id, scanned_matches[i].rule_id)
          << "round " << round << " key " << i;
      if ((indexed_mask & (uint64_t{1} << i)) == 0) {
        // Untouched: both leave the caller's value alone on a miss.
        EXPECT_EQ(0xffff, scanned_matches[i].action);
        EXPECT_EQ(0xffffffffu, scanned_matches[i].rule_id);
      }
    }
  }
}

}  // namespace
