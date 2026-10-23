// SPDX-License-Identifier: BSD-3-Clause

// HashLB in process (was bessctl/module_tests/hash_lb.py): every packet
// leaves on one of the configured gates, a wire gate that does not fit
// gate_idx_t is refused (not narrowed), and a refused command changes
// nothing.

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "arch/crc32c.h"
#include "dataplane/member_select.h"
#include "dataplane/transaction_engine.h"
#include "framework/exact_match_table.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "module_graph.h"
#include "modules/hash_lb.h"
#include "packet_parse.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"
#include "testing/parse_corpus.h"
#include "testing/rng.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::TcpPacket;

std::vector<Bytes> Flows(int n = 64) {
  std::mt19937 rng(7);
  std::vector<Bytes> pkts;
  for (int i = 0; i < n; i++) {
    pkts.push_back(TcpPacket({.sip = "10.0." + std::to_string(i / 250) + "." +
                                     std::to_string(i % 250 + 1),
                              .dip = "10.1.0.1",
                              .sport = 1000 + i,
                              .dport = 80},
                             rng));
  }
  return pkts;
}

const std::vector<gate_idx_t> kGates = {0, 1, 2, 3, 4, 5, 6, 7};

// The gates that got packets; every packet must come out unchanged on one.
std::set<gate_idx_t> GatesUsed(const std::map<gate_idx_t, std::vector<Bytes>> &out,
                               const std::vector<Bytes> &in) {
  std::set<gate_idx_t> used;
  std::multiset<Bytes> seen;
  for (const auto &[g, pkts] : out) {
    if (!pkts.empty()) {
      used.insert(g);
    }
    seen.insert(pkts.begin(), pkts.end());
  }
  EXPECT_EQ(std::multiset<Bytes>(in.begin(), in.end()), seen) << "every packet, once";
  return used;
}

TEST(HashLbTest, NoGatesIsRefusedAtCreation) {
  ModuleHarness h;
  EXPECT_EQ(EINVAL, h.CreateFailure("HashLB", bess::pb::HashLBArg()).code());
}

// -- "<name>/settings" (D-129) ---------------------------------------------------

bess::pb::HashLBSettings LbSettings(const std::string &mode,
                                    std::initializer_list<int64_t> gates) {
  bess::pb::HashLBSettings s;
  s.set_mode(mode);
  for (int64_t g : gates) {
    s.add_gates(g);
  }
  return s;
}

bess::pb::HashLBArg LbArg(std::initializer_list<int64_t> gates) {
  bess::pb::HashLBArg arg;
  for (int64_t g : gates) {
    arg.add_gates(g);
  }
  return arg;
}

// -- custom fields ----------------------------------------------------------------

bess::pb::Field OffsetField(int64_t offset, uint32_t bytes) {
  bess::pb::Field f;
  f.set_offset(offset);
  f.set_num_bytes(bytes);
  return f;
}

bess::pb::Field AttrField(const std::string &name, uint32_t bytes) {
  bess::pb::Field f;
  f.set_attr_name(name);
  f.set_num_bytes(bytes);
  return f;
}

bess::pb::HashLBSettings FieldSettings(
    const std::vector<gate_idx_t> &gates,
    const std::vector<bess::pb::Field> &fields) {
  bess::pb::HashLBSettings s;
  s.set_mode("");
  for (gate_idx_t g : gates) {
    s.add_gates(g);
  }
  for (const bess::pb::Field &f : fields) {
    *s.add_fields() = f;
  }
  return s;
}

// The custom tuple's hash as the module has always computed it: CRC32C over
// the key bytes (the fields' bytes in field order) zero-padded to whole 8-byte
// words -- ExactMatchKeyHash over the field table the module built. The gate
// is that hash mapped over the gate list (RangeSelect). The extractor is
// shared with the classifier now; a valid packet's gate must not change.
uint32_t KeyHash(const Bytes &key) {
  uint32_t hash = 0;
  size_t at = 0;
  for (; at + sizeof(uint64_t) <= key.size(); at += sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word, key.data() + at, sizeof(word));
    hash = bess::arch::Crc32c(word, hash);
  }
  if (at < key.size()) {
    uint64_t word = 0;
    std::memcpy(&word, key.data() + at, key.size() - at);
    hash = bess::arch::Crc32c(word, hash);
  }
  return hash;
}

gate_idx_t ExpectedGate(const Bytes &key,
                        const std::vector<gate_idx_t> &gates) {
  return gates[bess::dataplane::RangeSelect(KeyHash(key), gates.size())];
}

// The hash the module's old field table (framework/exact_match_table.h,
// no longer used by the module) computes for `pkt` over packet-offset fields:
// the oracle the shared extractor's key must reproduce. Its key bytes are
// ExactMatchKeyHash's input, zero-padded to the table's aligned key size.
uint32_t LegacyKeyHash(bess::framework::ExactMatchTable<int> &table,
                       const Bytes &pkt) {
  const bess::framework::ExactMatchKey key = table.MakeKey(pkt.data());
  const bess::framework::ExactMatchKeyHash hash(table.total_key_size());
  return hash(key);
}

// The gate `out` holds `pkt` on (the packets under test are distinct).
gate_idx_t GateOf(const std::map<gate_idx_t, std::vector<Bytes>> &out,
                  const Bytes &pkt) {
  for (const auto &[gate, pkts] : out) {
    if (std::find(pkts.begin(), pkts.end(), pkt) != pkts.end()) {
      return gate;
    }
  }
  return DROP_GATE;
}

// True when "<resource>"'s codec says applying it requires paused workers.
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

TEST(HashLbSettingsTest, GatesAndModeDecideTheOutputGate) {
  const std::vector<Bytes> pkts = Flows();
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  EXPECT_EQ((std::set<gate_idx_t>{1, 2}), GatesUsed(h.Run(lb, 0, pkts, kGates), pkts))
      << "all flows on the configuration's gates";

  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l4", {3, 4, 5})));
  EXPECT_EQ((std::set<gate_idx_t>{3, 4, 5}), GatesUsed(h.Run(lb, 0, pkts, kGates), pkts));

  // Every flow has the same destination: a custom tuple over the
  // destination address alone sends them all to one gate.
  bess::pb::HashLBSettings dst_only = LbSettings("", {3, 4, 5});
  bess::pb::Field *field = dst_only.add_fields();
  field->set_offset(30);  // IPv4 destination
  field->set_num_bytes(4);
  ASSERT_EQ("", h.SetResource("lb/settings", dst_only));
  EXPECT_EQ(1u, GatesUsed(h.Run(lb, 0, pkts, kGates), pkts).size());

  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l3", {6})));
  EXPECT_EQ((std::set<gate_idx_t>{6}), GatesUsed(h.Run(lb, 0, pkts, kGates), pkts));
}

TEST(HashLbSettingsTest, ReadBackRoundTrips) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("HashLB", LbArg({1, 2}), "lb"));
  const bess::pb::HashLBSettings initial = LbSettings("l4", {1, 2});
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &initial}}))
      << "no mode given: l4";
  const bess::pb::HashLBSettings s = LbSettings("l2", {0, 7, 9});
  ASSERT_EQ("", h.SetResource("lb/settings", s));
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &s}}));

  bess::pb::HashLBSettings fields = LbSettings("", {2});
  bess::pb::Field *f = fields.add_fields();
  f->set_offset(26);
  f->set_num_bytes(8);
  ASSERT_EQ("", h.SetResource("lb/settings", fields));
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &fields}}));
}

TEST(HashLbSettingsTest, BadValuesRefused) {
  const std::vector<Bytes> pkts = Flows();
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  const auto before = h.Run(lb, 0, pkts, kGates);
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("l4", {})));
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("l4", {65536})))
      << "not narrowed to gate 0";
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("l4", {3, 70000})));
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("bogus", {3})));
  EXPECT_NE("", h.SetResource("lb/settings", LbSettings("", {3})));
  const bess::pb::HashLBSettings unchanged = LbSettings("l4", {1, 2});
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &unchanged}}));
  EXPECT_EQ(before, h.Run(lb, 0, pkts, kGates)) << "refused values change nothing";
}

TEST(HashLbSettingsTest, AppliesLive) {
  ModuleHarness h;
  ASSERT_NE(nullptr, h.Create("HashLB", LbArg({1, 2}), "lb"));
  EXPECT_FALSE(RequiresPausedWorkers("lb/settings"));
}

// The custom tuple's key is the fields' bytes in field order and its gate is
// the hash over that key, unchanged from the field table this mode used to
// build.
TEST(HashLbCustomFieldsTest, MultiFieldKeyAndBucketSelectionAreUnchanged) {
  const std::vector<Bytes> pkts = Flows();
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  ASSERT_EQ("", h.SetResource(
                    "lb/settings",
                    FieldSettings(kGates, {OffsetField(26, 4),   // IPv4 src
                                           OffsetField(30, 4),   // IPv4 dst
                                           OffsetField(34, 2),   // TCP sport
                                           OffsetField(36, 2)})));

  const auto out = h.Run(lb, 0, pkts, kGates);
  GatesUsed(out, pkts);
  // The module's previous key constructor and hash, over the same fields: the
  // shared extractor must hash the same bytes.
  bess::framework::ExactMatchTable<int> legacy;
  ASSERT_EQ(0, legacy.AddField(26, 4, 0, 0).first);
  ASSERT_EQ(0, legacy.AddField(30, 4, 0, 1).first);
  ASSERT_EQ(0, legacy.AddField(34, 2, 0, 2).first);
  ASSERT_EQ(0, legacy.AddField(36, 2, 0, 3).first);

  std::set<gate_idx_t> used;
  for (const Bytes &pkt : pkts) {
    ASSERT_GE(pkt.size(), 38u);
    const Bytes key(pkt.begin() + 26, pkt.begin() + 38);
    EXPECT_EQ(LegacyKeyHash(legacy, pkt), KeyHash(key))
        << "the multi-field key is unchanged";
    const gate_idx_t expected = ExpectedGate(key, kGates);
    EXPECT_EQ(expected, GateOf(out, pkt)) << "packet of " << pkt.size();
    used.insert(expected);
  }
  EXPECT_GT(used.size(), 1u) << "the tuple spreads flows over the gates";
}

// A key shorter than 8 bytes is hashed zero-padded, as the word-aligned field
// table always did: a 1-byte field is one byte and seven zeros.
TEST(HashLbCustomFieldsTest, ASubWordFieldKeepsItsPaddingAndGate) {
  const std::vector<Bytes> pkts = Flows();
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  ASSERT_EQ("", h.SetResource("lb/settings",
                              FieldSettings(kGates, {OffsetField(35, 1)})));

  const auto out = h.Run(lb, 0, pkts, kGates);
  GatesUsed(out, pkts);
  // The old field table's key for a 1-byte field: one byte, seven zeros.
  bess::framework::ExactMatchTable<int> legacy;
  ASSERT_EQ(0, legacy.AddField(35, 1, 0, 0).first);
  for (const Bytes &pkt : pkts) {
    const Bytes key(pkt.begin() + 35, pkt.begin() + 36);
    EXPECT_EQ(LegacyKeyHash(legacy, pkt), KeyHash(key));
    EXPECT_EQ(ExpectedGate(key, kGates), GateOf(out, pkt));
  }
}

// A field may name a metadata attribute declared at creation: its bytes join
// the key in field order, and the extraction plan is rebuilt on the resume
// that assigns the attribute's offset.
TEST(HashLbCustomFieldsTest, AFieldMayBeAMetadataAttribute) {
  ModuleHarness h;
  const bess::pb::HashLBSettings field_settings =
      FieldSettings(kGates, {AttrField("hashlb_tuple_meta", 2),
                             OffsetField(26, 4)});
  bess::pb::HashLBArg arg = LbArg({1, 2});
  *arg.mutable_fields() = field_settings.fields();
  Module *lb = h.Create("HashLB", arg, "lb");
  ASSERT_NE(nullptr, lb);
  ASSERT_EQ("", h.SetResource("lb/settings", field_settings));

  std::mt19937 rng = bess::testing::SeededRng(11);
  const Bytes pkt = TcpPacket(
      {.sip = "10.9.8.7", .dip = "10.1.0.1", .sport = 1234, .dport = 80}, rng);
  const std::vector<std::string> values = {std::string("\x77\x90", 2),
                                           std::string("\x88\x80", 2)};
  // One writer per value, through a Merge: two branches write the same
  // attribute, as the ExactMatch metadata tests do.
  Module *merge = h.Create("Merge", bess::pb::MergeArg());
  ASSERT_NE(nullptr, merge);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(merge, 0, lb, 0, true));
  std::set<gate_idx_t> used;
  for (size_t i = 0; i < values.size(); i++) {
    bess::pb::SetMetadataArg tag;
    auto *attr = tag.add_attrs();
    attr->set_name("hashlb_tuple_meta");
    attr->set_size(2);
    attr->set_value_bin(values[i]);
    Module *set = h.Create("SetMetadata", tag, "tag" + std::to_string(i));
    ASSERT_NE(nullptr, set);
    ASSERT_EQ(0, ModuleGraph::ConnectModules(set, 0, merge,
                                             static_cast<gate_idx_t>(i), true));

    const auto out = h.RunPipeline(set, 0, {pkt}, lb, kGates);
    Bytes key(values[i].begin(), values[i].end());
    key.insert(key.end(), pkt.begin() + 26, pkt.begin() + 30);
    const gate_idx_t expected = ExpectedGate(key, kGates);
    EXPECT_EQ(expected, GateOf(out, pkt)) << "metadata value " << i;
    used.insert(expected);

    // Re-applying the fields rebuilds the plan around the already registered
    // attribute (re-registering it would fail with EEXIST) and keeps the gate.
    ASSERT_EQ("", h.SetResource("lb/settings", field_settings));
    EXPECT_EQ(expected,
              GateOf(h.RunPipeline(set, 0, {pkt}, lb, kGates), pkt))
        << "re-applied fields, metadata value " << i;
  }
  EXPECT_GT(used.size(), 1u) << "the attribute's bytes are part of the key";
}

// A live settings update cannot register a new metadata attribute: its offset
// is assigned by a resume, and a resource write does not resume. The refusal
// leaves the published configuration unchanged.
TEST(HashLbCustomFieldsTest, ALiveUpdateCannotAddAMetadataAttribute) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  const bess::pb::HashLBSettings good =
      FieldSettings(kGates, {OffsetField(26, 4)});
  ASSERT_EQ("", h.SetResource("lb/settings", good));
  EXPECT_NE("", h.SetResource(
                    "lb/settings",
                    FieldSettings(kGates, {AttrField("hashlb_live_meta", 2),
                                           OffsetField(26, 4)})));
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &good}}))
      << "the refused update changed the published settings";
}

// A packet short for any field has no key at all -- not a partial one -- and
// is dropped, like a short frame in the fixed modes. (The harness injects
// single-segment packets; a chained packet's later segment is equally out of
// reach of a field, and kCheck fails extraction the same way.)
TEST(HashLbCustomFieldsTest, AShortPacketHasNoKeyAndIsDropped) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  ASSERT_EQ("", h.SetResource(
                    "lb/settings",
                    FieldSettings(kGates, {OffsetField(14, 2),
                                           OffsetField(30, 4)})));

  for (const Bytes &short_pkt : {Bytes(20), Bytes(31), Bytes(33)}) {
    const auto out = h.Run(lb, 0, {short_pkt}, kGates);
    for (const auto &[gate, pkts] : out) {
      EXPECT_TRUE(pkts.empty()) << "gate " << gate << ", " << short_pkt.size()
                                << " bytes";
    }
  }

  // One byte more and both fields fit: the packet has a gate again.
  std::mt19937 rng = bess::testing::SeededRng(3);
  const Bytes fits =
      TcpPacket({.sip = "10.0.0.1", .dip = "10.0.0.2", .sport = 1, .dport = 2},
                rng);
  const auto out = h.Run(lb, 0, {fits}, kGates);
  EXPECT_NE(DROP_GATE, GateOf(out, fits));
}

// The custom tuple's limits, as the field table enforced them.
TEST(HashLbCustomFieldsTest, FieldLimitsAreEnforced) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  const bess::pb::HashLBSettings good =
      FieldSettings(kGates, {OffsetField(26, 4)});
  ASSERT_EQ("", h.SetResource("lb/settings", good));

  EXPECT_NE("", h.SetResource("lb/settings",
                              FieldSettings(kGates, {OffsetField(26, 0)})));
  EXPECT_NE("", h.SetResource("lb/settings",
                              FieldSettings(kGates, {OffsetField(26, 9)})));
  EXPECT_NE("", h.SetResource("lb/settings",
                              FieldSettings(kGates, {OffsetField(1025, 1)})));
  const std::vector<bess::pb::Field> nine(9, OffsetField(26, 1));
  EXPECT_NE("", h.SetResource("lb/settings", FieldSettings(kGates, nine)));
  EXPECT_EQ("", h.ReadBackDiffers("lb/settings", {{nullptr, &good}}))
      << "refused fields change nothing";
}

// Each mode reads a header; a frame too short for it is dropped, not read
// past its first segment.
TEST(HashLbTest, AFrameShorterThanTheHashedHeaderIsDropped) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  auto none = [&](const std::vector<Bytes> &in) {
    auto out = h.Run(lb, 0, in, kGates);
    for (const auto &[g, pkts] : out) {
      EXPECT_TRUE(pkts.empty()) << "gate " << g;
    }
  };

  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l2", {1, 2})));
  none({Bytes(11)});  // shorter than the two MAC addresses

  Bytes no_ip(33, 0);
  no_ip[12] = 0x08;  // EtherType IPv4
  no_ip[14] = 0x45;  // IPv4, IHL 5: a header, but the frame stops inside it
  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l3", {1, 2})));
  none({no_ip});

  Bytes no_ports(60, 0);
  no_ports[12] = 0x08;
  no_ports[14] = 0x4f;  // IPv4, IHL 15: 60 bytes of header, none of them here
  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l4", {1, 2})));
  none({no_ports});

  Bytes no_ports_short(37, 0);
  no_ports_short[12] = 0x08;
  no_ports_short[14] = 0x45;  // a 20-byte header, but not 4 bytes of ports
  none({no_ports_short});
}

// -- L3 and L4 over the checked parse (D-188) ---------------------------------------

using bess::testing::Ip4;
using bess::testing::Ip6;
using bess::testing::PacketBuilder;

// Ethernet behind `tags` VLAN tags (1: 802.1Q; 2: 802.1ad then 802.1Q).
PacketBuilder Tagged(int tags) {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:01", "02:00:00:00:00:02");
  if (tags == 2) {
    b.Dot1AD(5);
  }
  if (tags >= 1) {
    b.Dot1Q(7);
  }
  return b;
}

Bytes Tcp4(const std::string &src, const std::string &dst, uint16_t sport, uint16_t dport,
           int tags = 0) {
  return Tagged(tags).IPv4(src, dst).Tcp(sport, dport).PadTo(64).Build();
}

Bytes Tcp6(const std::string &src, const std::string &dst, uint16_t sport, uint16_t dport,
           int tags = 0) {
  return Tagged(tags).IPv6(src, dst).Tcp(sport, dport).PadTo(80).Build();
}

// The gate of each packet, run one at a time (DROP_GATE: dropped).
std::vector<gate_idx_t> Gates(ModuleHarness &h, Module *lb, const std::vector<Bytes> &pkts) {
  std::vector<gate_idx_t> gates;
  gates.reserve(pkts.size());
  for (const Bytes &pkt : pkts) {
    gates.push_back(GateOf(h.Run(lb, 0, {pkt}, kGates), pkt));
  }
  return gates;
}

// The parse reads through VLAN tags: a flow keeps its gate with no tag, one
// tag or two (before, a tagged frame hashed the wrong bytes or was dropped).
TEST(HashLbParsedTest, ATaggedFlowKeepsItsGate) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({0, 1, 2, 3, 4, 5, 6, 7}), "lb");
  ASSERT_NE(nullptr, lb);
  for (const char *mode : {"l3", "l4"}) {
    ASSERT_EQ("", h.SetResource("lb/settings", LbSettings(mode, {0, 1, 2, 3, 4, 5, 6, 7})));
    for (int i = 0; i < 32; i++) {
      const std::string src = "10.0.0." + std::to_string(i + 1);
      const std::string src6 = "2001:db8::" + std::to_string(i + 1);
      const auto v4 = Gates(h, lb, {Tcp4(src, "10.1.0.1", 1000 + i, 80, 0),
                                    Tcp4(src, "10.1.0.1", 1000 + i, 80, 1),
                                    Tcp4(src, "10.1.0.1", 1000 + i, 80, 2)});
      ASSERT_NE(DROP_GATE, v4[0]) << mode << " " << i;
      EXPECT_EQ(v4[0], v4[1]) << mode << " " << i;
      EXPECT_EQ(v4[0], v4[2]) << mode << " " << i;
      const auto v6 = Gates(h, lb, {Tcp6(src6, "2001:db8:1::1", 1000 + i, 80, 0),
                                    Tcp6(src6, "2001:db8:1::1", 1000 + i, 80, 1),
                                    Tcp6(src6, "2001:db8:1::1", 1000 + i, 80, 2)});
      ASSERT_NE(DROP_GATE, v6[0]) << mode << " " << i;
      EXPECT_EQ(v6[0], v6[1]) << mode << " " << i;
      EXPECT_EQ(v6[0], v6[2]) << mode << " " << i;
    }
  }
}

// IPv6 is hashed (before, L4 dropped it and L3 hashed bytes of its header that
// are not the addresses): flows spread over every gate, a flow's packets
// share one, and in L4 a flow's ports count.
TEST(HashLbParsedTest, Ipv6FlowsSpreadOverTheGates) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2, 3, 4}), "lb");
  ASSERT_NE(nullptr, lb);
  std::vector<Bytes> by_address, by_port;
  for (int i = 0; i < 64; i++) {
    by_address.push_back(Tcp6("2001:db8::" + std::to_string(i + 1), "2001:db8:1::1", 1000, 80));
    by_port.push_back(Tcp6("2001:db8::1", "2001:db8:1::1", static_cast<uint16_t>(1000 + i), 80));
  }
  for (const char *mode : {"l3", "l4"}) {
    ASSERT_EQ("", h.SetResource("lb/settings", LbSettings(mode, {1, 2, 3, 4})));
    EXPECT_EQ((std::set<gate_idx_t>{1, 2, 3, 4}),
              GatesUsed(h.Run(lb, 0, by_address, kGates), by_address))
        << mode;
  }
  // L3: one address pair, one gate, whatever the ports; L4: the ports spread.
  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l3", {1, 2, 3, 4})));
  EXPECT_EQ(1u, GatesUsed(h.Run(lb, 0, by_port, kGates), by_port).size());
  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l4", {1, 2, 3, 4})));
  EXPECT_EQ((std::set<gate_idx_t>{1, 2, 3, 4}), GatesUsed(h.Run(lb, 0, by_port, kGates), by_port));
}

// In L4 a fragment hashes on its addresses alone: a later fragment has no
// ports, so the first fragment's are left out too, and all of one datagram's
// fragments share a gate (before, a later fragment's payload bytes were
// hashed as ports).
TEST(HashLbParsedTest, OneDatagramsFragmentsShareAGate) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({0, 1, 2, 3, 4, 5, 6, 7}), "lb");
  ASSERT_NE(nullptr, lb);
  ASSERT_EQ("", h.SetResource("lb/settings", LbSettings("l4", {0, 1, 2, 3, 4, 5, 6, 7})));
  std::set<gate_idx_t> datagram_gates;
  for (int i = 0; i < 32; i++) {
    const std::string src = "10.0.0." + std::to_string(i + 1);
    auto fragment = [&](uint16_t frag, bool udp_header) {
      PacketBuilder b = Tagged(0);
      b.Add(PacketBuilder::IPv4Hdr{.src = Ip4(src), .dst = Ip4("10.1.0.1"), .proto = 17, .frag = frag});
      if (udp_header) {
        b.Udp(static_cast<uint16_t>(2000 + i), 53);
      }
      return b.Payload(std::string(24, static_cast<char>('a' + i % 26))).Build();
    };
    const auto v4 = Gates(h, lb, {fragment(0x2000, true),     // first: MF, offset 0
                                  fragment(0x2004, false),    // MF, offset 32
                                  fragment(0x0008, false)});  // last, offset 64
    ASSERT_NE(DROP_GATE, v4[0]) << i;
    EXPECT_EQ(v4[0], v4[1]) << i;
    EXPECT_EQ(v4[0], v4[2]) << i;
    datagram_gates.insert(v4[0]);

    // IPv6: a fragment header (44) in front of UDP, then a later fragment.
    auto fragment6 = [&](uint8_t offset_lo, bool udp_header) {
      Bytes frag_header = {17, 0, 0, static_cast<uint8_t>(unsigned{offset_lo} | 1u), 0, 0, 0,
                           static_cast<uint8_t>(i)};
      PacketBuilder b = Tagged(0);
      b.Add(PacketBuilder::IPv6Hdr{.src = Ip6("2001:db8::" + std::to_string(i + 1)),
                                   .dst = Ip6("2001:db8:1::1"),
                                   .next = 44});
      b.Payload(frag_header);
      if (udp_header) {
        b.Udp(static_cast<uint16_t>(2000 + i), 53);
      }
      return b.Payload(std::string(24, 'x')).Build();
    };
    const auto v6 = Gates(h, lb, {fragment6(0x00, true), fragment6(0x20, false)});
    ASSERT_NE(DROP_GATE, v6[0]) << i;
    EXPECT_EQ(v6[0], v6[1]) << i;
  }
  EXPECT_GT(datagram_gates.size(), 4u) << "datagrams still spread";
}

// A frame with nothing to hash -- not IP (ARP), malformed, or shorter than an
// IP header -- is dropped in both modes (before, L3 hashed an ARP frame's
// bytes as if they were addresses).
TEST(HashLbParsedTest, NonIpAndMalformedFramesAreDropped) {
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({1, 2}), "lb");
  ASSERT_NE(nullptr, lb);
  const Bytes arp = PacketBuilder()
                        .Ether("02:00:00:00:00:01", "02:00:00:00:00:02")
                        .Arp(1, "02:00:00:00:00:02", "10.0.0.1", "00:00:00:00:00:00", "10.0.0.2")
                        .PadTo(60)
                        .Build();
  Bytes lie = Tcp4("10.0.0.1", "10.1.0.1", 1000, 80);
  lie[16] = 0x01;  // IPv4 total length 256 + 50: past the frame
  Bytes mismatch = Tcp6("2001:db8::1", "2001:db8:1::1", 1000, 80);
  mismatch[12] = 0x08;  // EtherType IPv4 over an IPv6 header
  mismatch[13] = 0x00;
  for (const char *mode : {"l3", "l4"}) {
    ASSERT_EQ("", h.SetResource("lb/settings", LbSettings(mode, {1, 2})));
    EXPECT_EQ((std::vector<gate_idx_t>{DROP_GATE, DROP_GATE, DROP_GATE, DROP_GATE}),
              Gates(h, lb, {arp, lie, mismatch, Bytes(40, 0)}))
        << mode;
    EXPECT_NE(DROP_GATE, Gates(h, lb, {Tcp4("10.0.0.1", "10.1.0.1", 1000, 80)})[0]) << mode;
  }
}

// The mix is the one the modes always had, so an untagged IPv4 TCP flow keeps
// its gate: CRC32C of the addresses (and in L4 the ports, as on the wire, and
// the protocol) XORed. XOR makes it symmetric: a flow's reply shares its gate.
TEST(HashLbParsedTest, AnUntaggedIpv4FlowKeepsItsGateAndItsReplyShares) {
  const std::vector<gate_idx_t> gates = {0, 1, 2, 3, 4, 5, 6, 7};
  auto load32 = [](const Bytes &p, size_t at) {
    uint32_t v;
    std::memcpy(&v, p.data() + at, sizeof(v));
    return v;
  };
  auto load16 = [](const Bytes &p, size_t at) {
    uint16_t v;
    std::memcpy(&v, p.data() + at, sizeof(v));
    return uint32_t{v};
  };
  ModuleHarness h;
  Module *lb = h.Create("HashLB", LbArg({0, 1, 2, 3, 4, 5, 6, 7}), "lb");
  ASSERT_NE(nullptr, lb);
  for (const bool l4 : {false, true}) {
    ASSERT_EQ("", h.SetResource("lb/settings", LbSettings(l4 ? "l4" : "l3", {0, 1, 2, 3, 4, 5, 6, 7})));
    for (int i = 0; i < 64; i++) {
      const std::string src = "10.0." + std::to_string(i / 8) + "." + std::to_string(i + 1);
      const Bytes pkt = Tcp4(src, "10.1.0.1", static_cast<uint16_t>(1000 + i), 80);
      uint32_t v = load32(pkt, 26) ^ load32(pkt, 30);
      if (l4) {
        v ^= load16(pkt, 34) ^ load16(pkt, 36) ^ pkt[23];
      }
      const gate_idx_t legacy = gates[bess::dataplane::RangeSelect(bess::arch::Crc32c(v, 0u), 8)];
      const Bytes reply = Tcp4("10.1.0.1", src, 80, static_cast<uint16_t>(1000 + i));
      EXPECT_EQ((std::vector<gate_idx_t>{legacy, legacy}), Gates(h, lb, {pkt, reply}))
          << (l4 ? "l4 " : "l3 ") << i;
    }
  }
}

// The hash L3 (`l4` false) and L4 must produce, from ParseFrame alone.
bool ReferenceHash(std::span<const uint8_t> frame, size_t total_len, bool l4, uint32_t *hash) {
  using bess::packet::ParseStatus;
  bess::packet::ParsedFrame p;
  const ParseStatus s = bess::packet::ParseFrame(frame, p, total_len);
  if (s != ParseStatus::kOk && s != ParseStatus::kFragment) {
    return false;
  }
  uint32_t words[8];
  std::memcpy(words, p.src.data(), 16);
  std::memcpy(words + 4, p.dst.data(), 16);
  uint32_t v = 0;
  for (const uint32_t w : words) {
    v ^= w;
  }
  if (l4 && s == ParseStatus::kOk && !p.first_fragment) {
    v ^= uint32_t{__builtin_bswap16(p.src_port)} ^ uint32_t{__builtin_bswap16(p.dst_port)} ^
         p.protocol;
  }
  *hash = bess::arch::Crc32c(v, 0u);
  return true;
}

// Valid, truncated and corrupted frames, tagged and not, IPv4 and IPv6, and
// random bytes: both modes, which read the common frame at fixed offsets
// after CheckFastIpv4, drop and hash every one exactly as through the full
// parse.
TEST(HashLbParsedTest, TheFastReadHashesEveryFrameAsTheParse) {
  size_t fast = 0, fast_hashed = 0;
  for (const auto &[bytes, total_len] : bess::testing::ParseCorpus()) {
    const std::span<const uint8_t> frame(bytes);
    for (const bool l4 : {false, true}) {
      uint32_t want = 0, got = 0;
      const bool hashed = ReferenceHash(frame, total_len, l4, &want);
      ASSERT_EQ(hashed, HashLB::FrameHash(frame, total_len, l4, &got))
          << ::testing::PrintToString(bytes) << " total " << total_len << " l4 " << l4;
      if (hashed) {
        EXPECT_EQ(want, got) << ::testing::PrintToString(bytes) << " l4 " << l4;
      }
      if (!l4 && bess::packet::CheckFastIpv4(frame, total_len).taken) {
        fast++;
        fast_hashed += hashed;
      }
    }
  }
  EXPECT_GT(fast_hashed, 500u);
  EXPECT_GT(fast - fast_hashed, 500u);
}

}  // namespace
