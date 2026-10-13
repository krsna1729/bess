// SPDX-License-Identifier: BSD-3-Clause

// G1.2a mode C: ExactMatch's command path against the in-place concurrent
// table -- add/delete/clear semantics, growth past the initial capacity, and
// restore -- through the module's own API; then packets through the module
// (ExactMatchPacketTest, was bessctl/module_tests/exact_match.py) in process.
// Commands while a worker forwards traffic are tests/live/match_modules_test.cc
// and the table's concurrency classifier/concurrent_exact_test.cc.

#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "runtime/runtime_state.h"
#include "packet_pool.h"
#include "modules/exact_match.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::pb::ExactMatchArg;
using bess::pb::ExactMatchCommandAddArg;
using bess::pb::ExactMatchCommandDeleteArg;
using bess::pb::ExactMatchConfig;

class ExactMatchTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }

  // Two fields: 4 bytes at offset 26 (IPv4 src), 2 bytes at offset 34.
  ExactMatch *Create() {
    ExactMatchArg arg;
    auto *f = arg.add_fields();
    f->set_offset(26);
    f->set_num_bytes(4);
    f = arg.add_fields();
    f->set_offset(34);
    f->set_num_bytes(2);
    const auto &builders = ModuleBuilder::all_module_builders();
    const auto it = builders.find("ExactMatch");
    EXPECT_NE(it, builders.end());
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(arg));
    pb_error_t perr;
    Module *m = ModuleGraph::CreateModule(it->second, "em", packed, &perr);
    EXPECT_EQ(0, perr.code()) << perr.errmsg();
    return static_cast<ExactMatch *>(m);
  }

  static ExactMatchCommandAddArg Rule(uint32_t a, uint16_t b, uint64_t gate) {
    ExactMatchCommandAddArg arg;
    arg.set_gate(gate);
    arg.add_fields()->set_value_int(a);
    arg.add_fields()->set_value_int(b);
    return arg;
  }

  static ExactMatchCommandDeleteArg Key(uint32_t a, uint16_t b) {
    ExactMatchCommandDeleteArg arg;
    arg.add_fields()->set_value_int(a);
    arg.add_fields()->set_value_int(b);
    return arg;
  }

  static ExactMatchConfig Config(ExactMatch *m) {
    ExactMatchConfig config;
    CommandResponse r = m->GetRuntimeConfig(bess::pb::EmptyArg());
    EXPECT_FALSE(r.has_error());
    EXPECT_TRUE(r.data().UnpackTo(&config));
    return config;
  }
};

TEST_F(ExactMatchTest, AddDeleteClearAndDefaultGate) {
  ExactMatch *m = Create();
  ASSERT_NE(nullptr, m);
  ASSERT_FALSE(m->CommandAdd(Rule(1, 10, 3)).has_error());
  ASSERT_FALSE(m->CommandAdd(Rule(2, 20, 1)).has_error());
  ASSERT_FALSE(m->CommandAdd(Rule(2, 20, 4)).has_error());  // update
  EXPECT_EQ("2 fields, 2 rules", m->GetDesc());

  ExactMatchConfig config = Config(m);
  ASSERT_EQ(2, config.rules_size());
  EXPECT_EQ(3u, config.rules(0).gate());  // sorted by gate
  EXPECT_EQ(4u, config.rules(1).gate());
  EXPECT_EQ(std::string("\x02\x00\x00\x00", 4),
            config.rules(1).fields(0).value_bin());
  EXPECT_EQ(std::string("\x14\x00", 2), config.rules(1).fields(1).value_bin());

  bess::pb::ExactMatchCommandSetDefaultGateArg gate;
  gate.set_gate(7);
  ASSERT_FALSE(m->CommandSetDefaultGate(gate).has_error());
  config = Config(m);
  EXPECT_EQ(7u, config.default_gate());
  EXPECT_EQ(2, config.rules_size()) << "rules survive a default-gate change";

  ASSERT_FALSE(m->CommandDelete(Key(1, 10)).has_error());
  CommandResponse r = m->CommandDelete(Key(1, 10));
  EXPECT_EQ(ENOENT, r.error().code());

  ASSERT_FALSE(m->CommandClear(bess::pb::EmptyArg()).has_error());
  config = Config(m);
  EXPECT_EQ(0, config.rules_size());
  EXPECT_EQ(7u, config.default_gate()) << "clear keeps the default gate";
}

// The default gate may be given at creation (manifests have no commands); an
// invalid one refuses the module.
TEST_F(ExactMatchTest, DefaultGateAtCreation) {
  const auto &builders = ModuleBuilder::all_module_builders();
  const auto it = builders.find("ExactMatch");
  ASSERT_NE(it, builders.end());
  for (const uint64_t gate : {uint64_t{9}, uint64_t{1} << 20}) {
    ExactMatchArg arg;
    auto *f = arg.add_fields();
    f->set_offset(26);
    f->set_num_bytes(4);
    arg.set_default_gate(gate);
    google::protobuf::Any packed;
    ASSERT_TRUE(packed.PackFrom(arg));
    pb_error_t perr;
    Module *m = ModuleGraph::CreateModule(it->second, "em_dg", packed, &perr);
    if (gate == 9) {
      ASSERT_NE(nullptr, m) << perr.errmsg();
      EXPECT_EQ(9u, Config(static_cast<ExactMatch *>(m)).default_gate());
      ExactMatchArg initial;
      CommandResponse r =
          static_cast<ExactMatch *>(m)->GetInitialArg(bess::pb::EmptyArg());
      ASSERT_TRUE(r.data().UnpackTo(&initial));
      EXPECT_EQ(9u, initial.default_gate()) << "get_initial_arg reproduces it";
      ModuleGraph::DestroyAllModules();
    } else {
      EXPECT_EQ(nullptr, m);
      EXPECT_EQ(EINVAL, perr.code()) << perr.errmsg();
    }
  }
}

TEST_F(ExactMatchTest, RejectsWrongFieldCountAndSize) {
  ExactMatch *m = Create();
  ASSERT_NE(nullptr, m);
  ExactMatchCommandAddArg one_field;
  one_field.set_gate(1);
  one_field.add_fields()->set_value_int(1);
  EXPECT_EQ(EINVAL, m->CommandAdd(one_field).error().code());

  ExactMatchCommandAddArg wide = Rule(1, 2, 1);
  wide.mutable_fields(1)->set_value_bin(std::string(3, 'x'));
  EXPECT_EQ(EINVAL, m->CommandAdd(wide).error().code());
  EXPECT_EQ(0, Config(m).rules_size());
}

// Past the initial 1K-slot table the module grows by doubling (a copy and a
// generation publish); nothing is lost across several growths, and deletes
// interleaved with growth stay exact.
TEST_F(ExactMatchTest, GrowsWithoutLosingRules) {
  ExactMatch *m = Create();
  ASSERT_NE(nullptr, m);
  constexpr uint32_t kRules = 20000;
  for (uint32_t i = 0; i < kRules; i++) {
    ASSERT_FALSE(m->CommandAdd(Rule(i, static_cast<uint16_t>(i * 7),
                                    i % 1000)).has_error())
        << i;
    if (i % 5 == 4) {
      ASSERT_FALSE(m->CommandDelete(Key(i - 2, static_cast<uint16_t>(
                                                  (i - 2) * 7)))
                       .has_error());
    }
  }
  const ExactMatchConfig config = Config(m);
  EXPECT_EQ(static_cast<int>(kRules - kRules / 5), config.rules_size());
  EXPECT_EQ("2 fields, 16000 rules", m->GetDesc());
  for (const auto &rule : config.rules()) {
    uint32_t a;
    memcpy(&a, rule.fields(0).value_bin().data(), 4);
    EXPECT_NE(2u, a % 5) << "deleted rule " << a << " came back";
    EXPECT_EQ(a % 1000, rule.gate());
  }
}

// Deleted slots are held until readers pass a grace period. A worker that
// is slow to report quiescence must not turn adds into ENOSPC while the
// table is below its load limit: the module grows instead.
TEST_F(ExactMatchTest, ChurnDuringLongGracePeriodGrowsInsteadOfFailing) {
  ExactMatch *m = Create();
  ASSERT_NE(nullptr, m);
  bess::rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  constexpr uint32_t kReader = 21;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  domain.Online(kReader);  // and never quiescent during the churn

  for (uint32_t i = 0; i < 700; i++) {
    ASSERT_FALSE(m->CommandAdd(Rule(i, 1, 1)).has_error()) << i;
  }
  for (uint32_t i = 0; i < 2000; i++) {
    const uint32_t id = 100000 + i;
    CommandResponse r = m->CommandAdd(Rule(id, 1, 2));
    ASSERT_FALSE(r.has_error()) << i << ": " << r.error().errmsg();
    ASSERT_FALSE(m->CommandDelete(Key(id, 1)).has_error());
  }
  domain.Offline(kReader);
  domain.Unregister(kReader);
  EXPECT_EQ(700, Config(m).rules_size());
}

TEST_F(ExactMatchTest, RestoreReplacesRulesAndCollapsesDuplicates) {
  ExactMatch *m = Create();
  ASSERT_NE(nullptr, m);
  ASSERT_FALSE(m->CommandAdd(Rule(9, 9, 9)).has_error());

  ExactMatchConfig restore;
  restore.set_default_gate(5);
  *restore.add_rules() = Rule(1, 1, 1);
  *restore.add_rules() = Rule(2, 2, 2);
  *restore.add_rules() = Rule(1, 1, 3);  // last one wins
  ASSERT_FALSE(m->SetRuntimeConfig(restore).has_error());

  const ExactMatchConfig config = Config(m);
  EXPECT_EQ(5u, config.default_gate());
  ASSERT_EQ(2, config.rules_size());
  EXPECT_EQ(2u, config.rules(0).gate());
  EXPECT_EQ(3u, config.rules(1).gate());

  // A bad restore leaves the running configuration alone.
  ExactMatchConfig bad = restore;
  bad.mutable_rules(0)->mutable_fields(0)->set_value_bin("toolong!!");
  EXPECT_TRUE(m->SetRuntimeConfig(bad).has_error());
  EXPECT_EQ(2, Config(m).rules_size());
}

// Packets through the module (was bessctl/module_tests/exact_match.py).

using bess::testing::Bytes;
using bess::testing::Ip4;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::TcpPacket;
using google::protobuf::util::MessageDifferencer;

bess::pb::Field OffsetField(uint32_t offset, uint32_t bytes) {
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

bess::pb::FieldData Bin(const std::string &bytes) {
  bess::pb::FieldData d;
  d.set_value_bin(bytes);
  return d;
}

std::string Ip(const char *text) {
  const auto a = Ip4(text);
  return std::string(a.begin(), a.end());
}

ExactMatchCommandAddArg BinRule(const std::vector<std::string> &values, uint64_t gate) {
  ExactMatchCommandAddArg arg;
  arg.set_gate(gate);
  for (const std::string &v : values) {
    *arg.add_fields() = Bin(v);
  }
  return arg;
}

ExactMatchCommandDeleteArg BinKey(const std::vector<std::string> &values) {
  ExactMatchCommandDeleteArg arg;
  for (const std::string &v : values) {
    *arg.add_fields() = Bin(v);
  }
  return arg;
}

bess::pb::ExactMatchCommandSetDefaultGateArg DefaultGate(uint64_t gate) {
  bess::pb::ExactMatchCommandSetDefaultGateArg arg;
  arg.set_gate(gate);
  return arg;
}

template <typename T>
T Unpack(const google::protobuf::Any &any) {
  T out;
  EXPECT_TRUE(any.UnpackTo(&out)) << any.type_url();
  return out;
}

// The gates `out` holds a packet on, each with how many.
std::map<gate_idx_t, size_t> Counts(const std::map<gate_idx_t, std::vector<Bytes>> &out) {
  std::map<gate_idx_t, size_t> counts;
  for (const auto &[gate, packets] : out) {
    if (!packets.empty()) {
      counts[gate] = packets.size();
    }
  }
  return counts;
}

// Rules on (IPv4 src, IPv4 dst) send each packet, unchanged, out its rule's
// gate; a packet no rule matches takes the default gate.
TEST(ExactMatchPacketTest, IpPairRulesPickTheGateAndMissesTakeTheDefault) {
  ModuleHarness h;
  ExactMatchArg arg;
  *arg.add_fields() = OffsetField(26, 4);
  *arg.add_fields() = OffsetField(30, 4);
  Module *em = h.Create("ExactMatch", arg);
  ASSERT_NE(nullptr, em);
  h.MustCommand(em, "add", BinRule({Ip("65.43.21.0"), Ip("12.34.56.78")}, 1));
  h.MustCommand(em, "add", BinRule({Ip("0.12.34.56"), Ip("12.34.56.78")}, 2));
  h.MustCommand(em, "set_default_gate", DefaultGate(3));

  std::mt19937 rng(1);
  const Bytes pkt1 = TcpPacket({.sip = "65.43.21.0", .dip = "12.34.56.78"}, rng);
  const Bytes pkt2 = TcpPacket({.sip = "0.12.34.56", .dip = "12.34.56.78"}, rng);
  const Bytes miss = TcpPacket({.sip = "0.12.33.56", .dip = "12.34.56.78"}, rng);

  EXPECT_TRUE(Counts(h.Run(em, 0, {}, {0, 1, 2, 3})).empty());
  auto out = h.Run(em, 0, {pkt1}, {0, 1, 2, 3});
  EXPECT_EQ((std::map<gate_idx_t, size_t>{{1, 1}}), Counts(out));
  EXPECT_EQ((std::vector<Bytes>{pkt1}), out[1]);
  out = h.Run(em, 0, {pkt2}, {0, 1, 2, 3});
  EXPECT_EQ((std::map<gate_idx_t, size_t>{{2, 1}}), Counts(out));
  EXPECT_EQ((std::vector<Bytes>{pkt2}), out[2]);
  out = h.Run(em, 0, {miss}, {0, 1, 2, 3});
  EXPECT_EQ((std::map<gate_idx_t, size_t>{{3, 1}}), Counts(out));
  EXPECT_EQ((std::vector<Bytes>{miss}), out[3]);
}

// A field may be a metadata attribute: three SetMetadata writers feed one
// ExactMatch through a Merge, and each packet goes where its attribute's
// value sends it (no rule: the default gate).
TEST(ExactMatchPacketTest, MatchesOnAMetadataAttribute) {
  ModuleHarness h;
  ExactMatchArg arg;
  *arg.add_fields() = AttrField("sangjin", 2);
  *arg.add_masks() = Bin("\xff\xff");
  Module *em = h.Create("ExactMatch", arg);
  ASSERT_NE(nullptr, em);
  h.MustCommand(em, "add", BinRule({"\x88\x80"}, 1));
  h.MustCommand(em, "add", BinRule({"\x77\x70"}, 2));
  h.MustCommand(em, "set_default_gate", DefaultGate(0));

  Module *merge = h.Create("Merge", bess::pb::MergeArg());
  ASSERT_NE(nullptr, merge);
  // No default gate hooks: Track counts per worker, and the harness runs
  // off any worker.
  ASSERT_EQ(0, ModuleGraph::ConnectModules(merge, 0, em, 0, true));
  std::vector<Module *> tags;
  for (const char *value : {"\x77\x90", "\x88\x80", "\x77\x70"}) {
    bess::pb::SetMetadataArg tag;
    auto *attr = tag.add_attrs();
    attr->set_name("sangjin");
    attr->set_size(2);
    attr->set_value_bin(std::string(value, 2));
    Module *m = h.Create("SetMetadata", tag);
    ASSERT_NE(nullptr, m);
    ASSERT_EQ(0, ModuleGraph::ConnectModules(m, 0, merge, 0, true));
    tags.push_back(m);
  }

  // An IPv4/UDP packet without an Ethernet header, as the Python test sent.
  PacketBuilder b;
  b.IPv4("1.2.3.4", "2.3.4.5").Udp(10001, 10002).Payload("helloworld");
  b.Last<PacketBuilder::IPv4Hdr>().ttl = 98;
  const Bytes pkt = b.Build();
  for (gate_idx_t i = 0; i < 3; i++) {
    auto out = h.RunPipeline(tags[i], 0, {pkt}, em, {0, 1, 2});
    EXPECT_EQ((std::map<gate_idx_t, size_t>{{i, 1}}), Counts(out)) << "tag " << i;
    EXPECT_EQ((std::vector<Bytes>{pkt}), out[i]) << "tag " << i;
  }
}

// get_initial_arg reproduces the creation argument (a metadata and a packet
// field, each with its mask); get_runtime_config the default gate and the
// rules, sorted by gate.
TEST(ExactMatchPacketTest, InitialArgAndRuntimeConfigReproduceTheModule) {
  ModuleHarness h;
  ExactMatchArg arg;
  *arg.add_fields() = AttrField("babylon5", 2);
  *arg.add_fields() = OffsetField(10, 1);
  *arg.add_masks() = Bin("\xff\xf0");
  *arg.add_masks() = Bin("\x7f");
  Module *em = h.Create("ExactMatch", arg);
  ASSERT_NE(nullptr, em);
  // Added in reverse gate order: the answer is sorted anyway.
  h.MustCommand(em, "add", BinRule({"\x77\x70", "\x05"}, 2));
  h.MustCommand(em, "add", BinRule({std::string("\x88\x80", 2), "\x03"}, 1));
  h.MustCommand(em, "set_default_gate", DefaultGate(3));

  ExactMatchConfig expect;
  expect.set_default_gate(3);
  *expect.add_rules() = BinRule({std::string("\x88\x80", 2), "\x03"}, 1);
  *expect.add_rules() = BinRule({"\x77\x70", "\x05"}, 2);

  const auto initial =
      Unpack<ExactMatchArg>(h.MustCommand(em, "get_initial_arg", bess::pb::EmptyArg()));
  EXPECT_TRUE(MessageDifferencer::Equals(arg, initial)) << initial.DebugString();
  const auto config =
      Unpack<ExactMatchConfig>(h.MustCommand(em, "get_runtime_config", bess::pb::EmptyArg()));
  EXPECT_TRUE(MessageDifferencer::Equals(expect, config)) << config.DebugString();
}

// Every mutating command rebuilds the table; a metadata field must survive
// each rebuild (a regression: re-registering the attribute failed with
// EEXIST, so the first command after Init failed).
TEST(ExactMatchPacketTest, MetadataFieldSurvivesTableRebuilds) {
  ModuleHarness h;
  ExactMatchArg arg;
  *arg.add_fields() = AttrField("phase_j_meta", 2);
  Module *em = h.Create("ExactMatch", arg);
  ASSERT_NE(nullptr, em);
  const std::string key("\x88\x80", 2);

  h.MustCommand(em, "add", BinRule({key}, 1));
  h.MustCommand(em, "add", BinRule({key}, 2));  // an update: one rule
  h.MustCommand(em, "set_default_gate", DefaultGate(3));
  ExactMatchConfig restore;
  restore.set_default_gate(2);
  *restore.add_rules() = BinRule({key}, 1);
  h.MustCommand(em, "set_runtime_config", restore);
  h.MustCommand(em, "delete", BinKey({key}));
  h.MustCommand(em, "clear", bess::pb::EmptyArg());

  const auto config =
      Unpack<ExactMatchConfig>(h.MustCommand(em, "get_runtime_config", bess::pb::EmptyArg()));
  EXPECT_EQ(2u, config.default_gate());
  EXPECT_EQ(0, config.rules_size());
}

// Duplicates in set_runtime_config collapse to one rule, the last
// (RestoreReplacesRulesAndCollapsesDuplicates); the survivor is one real
// table entry: packets match it, and delete removes it exactly once.
TEST(ExactMatchPacketTest, CollapsedDuplicateIsOneTableEntry) {
  ModuleHarness h;
  ExactMatchArg arg;
  *arg.add_fields() = OffsetField(26, 4);
  Module *em = h.Create("ExactMatch", arg);
  ASSERT_NE(nullptr, em);
  ExactMatchConfig restore;
  restore.set_default_gate(0);
  *restore.add_rules() = BinRule({Ip("1.2.3.4")}, 1);
  *restore.add_rules() = BinRule({Ip("1.2.3.4")}, 2);
  h.MustCommand(em, "set_runtime_config", restore);

  auto config =
      Unpack<ExactMatchConfig>(h.MustCommand(em, "get_runtime_config", bess::pb::EmptyArg()));
  ASSERT_EQ(1, config.rules_size());
  EXPECT_EQ(2u, config.rules(0).gate());

  std::mt19937 rng(2);
  const Bytes pkt = TcpPacket({.sip = "1.2.3.4", .dip = "5.6.7.8"}, rng);
  EXPECT_EQ((std::map<gate_idx_t, size_t>{{2, 1}}), Counts(h.Run(em, 0, {pkt}, {0, 1, 2})));

  h.MustCommand(em, "delete", BinKey({Ip("1.2.3.4")}));
  config = Unpack<ExactMatchConfig>(h.MustCommand(em, "get_runtime_config", bess::pb::EmptyArg()));
  EXPECT_EQ(0, config.rules_size());
  EXPECT_EQ(ENOENT, h.Command(em, "delete", BinKey({Ip("1.2.3.4")})).error().code());
  EXPECT_EQ((std::map<gate_idx_t, size_t>{{0, 1}}), Counts(h.Run(em, 0, {pkt}, {0, 1, 2})));
}

// Rule bytes are stored as given; packet bytes are masked. With a 2-byte
// field on the first two IPv4 source bytes whose mask keeps only the first
// byte, a rule with a nonzero second byte never matches, and one with a zero
// second byte matches whatever the packet has there. The mask is reported
// in the converted (big-endian) order.
TEST(ExactMatchPacketTest, MaskedOffRuleBitsStayUnmatchable) {
  ModuleHarness h;
  ExactMatchArg arg;
  *arg.add_fields() = OffsetField(26, 2);
  *arg.add_masks() = Bin(std::string("\x00\xff", 2));
  Module *em = h.Create("ExactMatch", arg);
  ASSERT_NE(nullptr, em);
  h.MustCommand(em, "add", BinRule({"A\xbb"}, 1));
  h.MustCommand(em, "add", BinRule({std::string("A\x00", 2)}, 2));
  h.MustCommand(em, "set_default_gate", DefaultGate(3));

  const auto initial =
      Unpack<ExactMatchArg>(h.MustCommand(em, "get_initial_arg", bess::pb::EmptyArg()));
  ASSERT_EQ(1, initial.masks_size());
  EXPECT_EQ(std::string("\xff\x00", 2), initial.masks(0).value_bin());

  std::mt19937 rng(3);
  const Bytes hit = TcpPacket({.sip = "65.43.21.0", .dip = "12.34.56.78"}, rng);
  const Bytes hit_any_second = TcpPacket({.sip = "65.99.99.99", .dip = "12.34.56.78"}, rng);
  const Bytes miss = TcpPacket({.sip = "66.43.21.0", .dip = "12.34.56.78"}, rng);
  EXPECT_EQ((std::map<gate_idx_t, size_t>{{2, 1}}), Counts(h.Run(em, 0, {hit}, {0, 1, 2, 3})));
  EXPECT_EQ((std::map<gate_idx_t, size_t>{{2, 1}}),
            Counts(h.Run(em, 0, {hit_any_second}, {0, 1, 2, 3})));
  EXPECT_EQ((std::map<gate_idx_t, size_t>{{3, 1}}), Counts(h.Run(em, 0, {miss}, {0, 1, 2, 3})));
}

// The fused kernel (D-121) against the generic path, both in ClassifyBatch
// (the code ProcessBatch runs) on one module and its rules.

// Packets of random bytes and lengths, batched 32 at a time.
class RandomPackets {
 public:
  static constexpr size_t kRoom = 64;

  explicit RandomPackets(size_t n) : pool_(n + 64) {
    for (size_t i = 0; i < n; i++) {
      bess::PacketHandle pkt = pool_.Alloc(kRoom);
      EXPECT_NE(pkt, nullptr);
      handles_.push_back(pkt);
    }
  }
  ~RandomPackets() {
    for (bess::PacketHandle pkt : handles_) {
      bess::PacketFree(pkt);
    }
  }
  RandomPackets(const RandomPackets &) = delete;
  RandomPackets &operator=(const RandomPackets &) = delete;

  size_t size() const { return handles_.size(); }
  uint8_t *data(size_t i) const {
    return bess::PacketRef(handles_[i]).head_data<uint8_t *>();
  }
  void SetLength(size_t i, size_t len) {
    bess::PacketRef(handles_[i]).set_data_len(static_cast<uint16_t>(len));
  }

  // The gates `em` sends every packet to, 32 at a time.
  std::vector<gate_idx_t> Classify(const ExactMatch *em) const {
    std::vector<gate_idx_t> gates(handles_.size());
    for (size_t first = 0; first < handles_.size();
         first += bess::PacketBatch::kMaxBurst) {
      const size_t n =
          std::min(bess::PacketBatch::kMaxBurst, handles_.size() - first);
      bess::PacketBatch batch;
      batch.clear();
      for (size_t i = 0; i < n; i++) {
        batch.add(bess::PacketRef(handles_[first + i]));
      }
      em->ClassifyBatch(&batch, gates.data() + first);
    }
    return gates;
  }

 private:
  bess::PlainPacketPool pool_;
  std::vector<bess::PacketHandle> handles_;
};

// Every width the fused kernel takes, at odd and even offsets, unmasked and
// masked: hits (some with masked-off bits set), misses, packets one byte
// too short for the field and packets exactly long enough. The fused and the
// generic path send every packet to the same gate.
TEST(ExactMatchPacketTest, FusedKernelClassifiesAsTheGenericPath) {
  std::mt19937 rng(121);
  for (const size_t width : {size_t{1}, size_t{2}, size_t{4}, size_t{8}}) {
    for (const size_t offset : {size_t{0}, size_t{1}, size_t{26}, size_t{51}}) {
      for (const bool masked : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "width " << width << " offset "
                                          << offset << " masked " << masked);
        ExactMatchArg arg;
        *arg.add_fields() = OffsetField(static_cast<uint32_t>(offset),
                                        static_cast<uint32_t>(width));
        // A mask of the same byte everywhere does not depend on byte order.
        const unsigned mask = masked ? 0xf0u : 0xffu;
        if (masked) {
          *arg.add_masks() = Bin(std::string(width, '\xf0'));
        }
        arg.set_default_gate(0);
        ModuleHarness h;
        auto *em = static_cast<ExactMatch *>(h.Create("ExactMatch", arg));
        ASSERT_NE(nullptr, em);
        EXPECT_EQ(width, em->fused_key_width());

        // Rule keys hold only bits the mask keeps, so masked packets match.
        std::vector<std::string> keys;
        for (size_t r = 0; r < 64; r++) {
          std::string key(width, '\0');
          for (char &c : key) {
            c = static_cast<char>(rng() & mask);
          }
          keys.push_back(key);
          h.MustCommand(em, "add", BinRule({key}, 1 + r % 15));
        }

        RandomPackets packets(256);
        const size_t need = offset + width;
        for (size_t i = 0; i < packets.size(); i++) {
          uint8_t *p = packets.data(i);
          for (size_t k = 0; k < RandomPackets::kRoom; k++) {
            p[k] = static_cast<uint8_t>(rng());
          }
          if (i % 4 != 3) {  // a rule's key, with masked-off bits noise
            const std::string &key = keys[rng() % keys.size()];
            for (size_t k = 0; k < width; k++) {
              p[offset + k] = static_cast<uint8_t>(
                  static_cast<uint8_t>(key[k]) | (rng() & (0xffu ^ mask)));
            }
          }
          switch (i % 5) {
            case 0:
              packets.SetLength(i, need - 1);
              break;
            case 1:
              packets.SetLength(i, need);
              break;
            default:
              packets.SetLength(i, need + rng() % (RandomPackets::kRoom - need + 1));
              break;
          }
        }

        const std::vector<gate_idx_t> fused = packets.Classify(em);
        Error err;
        ASSERT_TRUE(em->SetFusedKernelEnabled(false, &err)) << err.second;
        EXPECT_EQ(0u, em->fused_key_width());
        const std::vector<gate_idx_t> generic = packets.Classify(em);
        EXPECT_EQ(generic, fused);

        size_t hits = 0;
        for (size_t i = 0; i < packets.size(); i++) {
          hits += fused[i] != 0;
          if (i % 5 == 0) {
            EXPECT_EQ(gate_idx_t{0}, fused[i])
                << "packet " << i << " is too short";
          }
        }
        EXPECT_GT(hits, packets.size() / 3);
      }
    }
  }
}

// The fused kernel takes exactly a key that is one packet field of 1, 2, 4
// or 8 bytes (after coalescing adjacent fields); every other layout runs the
// generic path.
TEST(ExactMatchPacketTest, FusedKernelOnlyForOnePacketFieldOf1248Bytes) {
  struct Case {
    std::vector<bess::pb::Field> fields;
    size_t fused;
  };
  const std::vector<Case> cases = {
      {{OffsetField(26, 1)}, 1},
      {{OffsetField(27, 2)}, 2},
      {{OffsetField(26, 4)}, 4},
      {{OffsetField(30, 8)}, 8},
      {{OffsetField(26, 2), OffsetField(28, 2)}, 4},  // coalesced: one field
      {{OffsetField(26, 4), OffsetField(30, 4)}, 8},
      {{OffsetField(26, 3)}, 0},
      {{OffsetField(26, 6)}, 0},
      {{OffsetField(26, 4), OffsetField(34, 2)}, 0},
      {{OffsetField(28, 2), OffsetField(26, 2)}, 0},  // key order != packet
      {{AttrField("fused_kernel_attr", 4)}, 0},
  };
  for (size_t c = 0; c < cases.size(); c++) {
    ModuleHarness h;
    ExactMatchArg arg;
    for (const bess::pb::Field &f : cases[c].fields) {
      *arg.add_fields() = f;
    }
    auto *em = static_cast<ExactMatch *>(h.Create("ExactMatch", arg));
    ASSERT_NE(nullptr, em) << "case " << c;
    EXPECT_EQ(cases[c].fused, em->fused_key_width()) << "case " << c;
  }
}

}  // namespace
