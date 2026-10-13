// SPDX-License-Identifier: BSD-3-Clause

// K3.6 module-level test for the migrated WildcardMatch: generation
// construction, rule canonicalization, the module's own limits, introspection
// byte parity, and the safety cases the legacy implementation had no defined
// behavior for; then packets through the module (WildcardMatchPacketTest, was
// bessctl/module_tests/wildcard_match.py) in process. Commands while a worker
// forwards traffic are tests/live/match_modules_test.cc.

#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "modules/wildcard_match.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::pb::Field;
using bess::pb::FieldData;
using bess::pb::WildcardMatchArg;
using bess::pb::WildcardMatchCommandAddArg;
using bess::pb::WildcardMatchCommandDeleteArg;
using bess::pb::WildcardMatchConfig;

constexpr gate_idx_t kGateA = 1;
constexpr gate_idx_t kGateB = 2;

class WildcardMatchTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }

  // Creates a module with `fields`; returns nullptr when Init rejects them.
  WildcardMatch *Create(const WildcardMatchArg &arg) {
    const auto it = ModuleBuilder::all_module_builders().find("WildcardMatch");
    if (it == ModuleBuilder::all_module_builders().end()) {
      return nullptr;
    }
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(arg));
    pb_error_t perr;
    Module *m = ModuleGraph::CreateModule(it->second, "wm", packed, &perr);
    if (m == nullptr || perr.code() != 0) {
      return nullptr;
    }
    return static_cast<WildcardMatch *>(m);
  }

  static Field OffsetField(int offset, int bytes) {
    Field f;
    f.set_offset(offset);
    f.set_num_bytes(bytes);
    return f;
  }

  static FieldData Bin(const std::vector<uint8_t> &bytes) {
    FieldData d;
    d.set_value_bin(std::string(bytes.begin(), bytes.end()));
    return d;
  }

  static FieldData Int(uint64_t value) {
    FieldData d;
    d.set_value_int(value);
    return d;
  }

  CommandResponse Run(WildcardMatch *m, const char *cmd,
                      const google::protobuf::Any &arg) {
    return m->RunCommand(cmd, arg);
  }

  template <typename T>
  static google::protobuf::Any Pack(const T &arg) {
    google::protobuf::Any packed;
    CHECK(packed.PackFrom(arg));
    return packed;
  }

  // One rule: one field of 2 bytes, so masks are easy to vary.
  WildcardMatchCommandAddArg Rule(uint64_t value, uint64_t mask,
                                  int64_t priority, uint64_t gate) {
    WildcardMatchCommandAddArg arg;
    arg.set_gate(gate);
    arg.set_priority(priority);
    std::vector<uint8_t> v(2);
    std::vector<uint8_t> m(2);
    for (int i = 0; i < 2; i++) {
      v[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
      m[i] = static_cast<uint8_t>((mask >> (8 * i)) & 0xff);
    }
    *arg.add_values() = Bin(v);
    *arg.add_masks() = Bin(m);
    return arg;
  }

  WildcardMatchArg TwoByteField() {
    WildcardMatchArg arg;
    *arg.add_fields() = OffsetField(10, 2);
    return arg;
  }
};

TEST_F(WildcardMatchTest, InitRejectsZeroAndTooManyFields) {
  EXPECT_EQ(nullptr, Create(WildcardMatchArg{}));

  WildcardMatchArg too_many;
  for (int i = 0; i < 9; i++) {
    *too_many.add_fields() = OffsetField(i * 8, 8);
  }
  EXPECT_EQ(nullptr, Create(too_many));

  WildcardMatchArg max_fields;
  for (int i = 0; i < 8; i++) {
    *max_fields.add_fields() = OffsetField(i * 8, 8);
  }
  EXPECT_NE(nullptr, Create(max_fields));
}

TEST_F(WildcardMatchTest, AddOverwritesSameMaskAndValueRegardlessOfPriority) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  // Higher priority first, then a lower-priority command with the same
  // (mask, value): the later command wins, which is what inserting the same
  // key into the live tuple table did.
  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x1234, 0xffff, 100, kGateA)))
                   .error()
                   .code());
  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x1234, 0xffff, 1, kGateB)))
                   .error()
                   .code());

  bess::pb::EmptyArg empty;
  CommandResponse resp = Run(m, "get_runtime_config", Pack(empty));
  ASSERT_EQ(0, resp.error().code());
  WildcardMatchConfig config;
  ASSERT_TRUE(resp.data().UnpackTo(&config));
  ASSERT_EQ(1, config.rules_size());
  EXPECT_EQ(kGateB, config.rules(0).gate());
  EXPECT_EQ(1, config.rules(0).priority());
}

TEST_F(WildcardMatchTest, TupleCeilingCountsActiveMasksAndClearRestoresIt) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  // Eight distinct masks fit.
  for (int i = 0; i < 8; i++) {
    const uint64_t mask = 0xff00u | static_cast<uint64_t>(i);
    ASSERT_EQ(0, Run(m, "add", Pack(Rule(0, mask, i, kGateA))).error().code());
  }
  // The ninth distinct mask is rejected with master's ENOSPC.
  EXPECT_EQ(ENOSPC, Run(m, "add", Pack(Rule(0, 0xabcd, 9, kGateA))).error().code());

  // clear() drops the rules *and* their masks, so capacity is genuinely free.
  bess::pb::EmptyArg empty;
  ASSERT_EQ(0, Run(m, "clear", Pack(empty)).error().code());
  for (int i = 0; i < 8; i++) {
    const uint64_t mask = 0x00ffu | (static_cast<uint64_t>(i) << 8);
    ASSERT_EQ(0, Run(m, "add", Pack(Rule(0, mask, i, kGateB))).error().code());
  }
  EXPECT_NE(0, Run(m, "add", Pack(Rule(0, 0x1111, 9, kGateB))).error().code());
}

TEST_F(WildcardMatchTest, MaxTuplesSetsTheCeilingAndRoundTrips) {
  WildcardMatchArg arg = TwoByteField();
  arg.set_max_tuples(WildcardMatch::kMaxTuplesLimit + 1);
  EXPECT_EQ(nullptr, Create(arg)) << "beyond the upper bound";

  arg.set_max_tuples(12);
  WildcardMatch *m = Create(arg);
  ASSERT_NE(nullptr, m);
  for (int i = 0; i < 12; i++) {
    const uint64_t mask = 0xff00u | static_cast<uint64_t>(i);
    ASSERT_EQ(0, Run(m, "add", Pack(Rule(0, mask, i, kGateA))).error().code())
        << i;
  }
  EXPECT_EQ(ENOSPC,
            Run(m, "add", Pack(Rule(0, 0xabcd, 13, kGateA))).error().code());

  bess::pb::EmptyArg empty;
  CommandResponse resp = Run(m, "get_initial_arg", Pack(empty));
  ASSERT_EQ(0, resp.error().code());
  WildcardMatchArg out;
  ASSERT_TRUE(resp.data().UnpackTo(&out));
  EXPECT_EQ(12u, out.max_tuples());
}

// The default gate may be given at creation; get_initial_arg reproduces it,
// and an invalid one refuses the module.
TEST_F(WildcardMatchTest, DefaultGateAtCreation) {
  WildcardMatchArg arg = TwoByteField();
  arg.set_default_gate(uint64_t{1} << 20);
  EXPECT_EQ(nullptr, Create(arg)) << "an invalid gate";

  arg.set_default_gate(kGateB);
  WildcardMatch *m = Create(arg);
  ASSERT_NE(nullptr, m);
  bess::pb::EmptyArg empty;
  CommandResponse config = Run(m, "get_runtime_config", Pack(empty));
  ASSERT_EQ(0, config.error().code());
  WildcardMatchConfig runtime;
  ASSERT_TRUE(config.data().UnpackTo(&runtime));
  EXPECT_EQ(kGateB, runtime.default_gate());
  CommandResponse initial = Run(m, "get_initial_arg", Pack(empty));
  ASSERT_EQ(0, initial.error().code());
  WildcardMatchArg out;
  ASSERT_TRUE(initial.data().UnpackTo(&out));
  ASSERT_TRUE(out.has_default_gate());
  EXPECT_EQ(kGateB, out.default_gate());
}

TEST_F(WildcardMatchTest, DeleteExistingAndMissing) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x1234, 0xffff, 1, kGateA)))
                   .error()
                   .code());

  WildcardMatchCommandDeleteArg del;
  *del.add_values() = Bin({0x34, 0x12});
  *del.add_masks() = Bin({0xff, 0xff});
  EXPECT_EQ(0, Run(m, "delete", Pack(del)).error().code());
  // Deleting again reports not-found rather than succeeding.
  EXPECT_EQ(ENOENT, Run(m, "delete", Pack(del)).error().code());

  // A missing key inside an existing mask is also not-found.
  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x1234, 0xffff, 1, kGateA)))
                   .error()
                   .code());
  WildcardMatchCommandDeleteArg other;
  *other.add_values() = Bin({0x99, 0x99});
  *other.add_masks() = Bin({0xff, 0xff});
  EXPECT_EQ(ENOENT, Run(m, "delete", Pack(other)).error().code());
}

TEST_F(WildcardMatchTest, BinaryLengthRulesAreCompatibleButBounded) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  // Shorter than the field: accepted and zero-padded, as the legacy
  // stack-local decode effectively did.
  WildcardMatchCommandAddArg short_value;
  short_value.set_gate(kGateA);
  short_value.set_priority(1);
  *short_value.add_values() = Bin({0x34});
  *short_value.add_masks() = Bin({0xff});
  EXPECT_EQ(0, Run(m, "add", Pack(short_value)).error().code());

  // Longer than the field: rejected instead of copied past the field word.
  WildcardMatchCommandAddArg long_value;
  long_value.set_gate(kGateA);
  long_value.set_priority(1);
  *long_value.add_values() = Bin({0x01, 0x02, 0x03});
  *long_value.add_masks() = Bin({0xff, 0xff});
  EXPECT_NE(0, Run(m, "add", Pack(long_value)).error().code());
}

TEST_F(WildcardMatchTest, NonCanonicalRuleIsRejected) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  // value has bits set outside the mask.
  EXPECT_NE(0, Run(m, "add", Pack(Rule(0xffff, 0x00ff, 1, kGateA)))
                   .error()
                   .code());
}

TEST_F(WildcardMatchTest, FullInt64PrioritySurvivesRoundTrip) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  const int64_t big = int64_t{1} << 40;
  const int64_t negative = -1234567890123ll;
  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x0001, 0xffff, big, kGateA)))
                   .error()
                   .code());
  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x0002, 0xffff, negative, kGateB)))
                   .error()
                   .code());

  bess::pb::EmptyArg empty;
  CommandResponse resp = Run(m, "get_runtime_config", Pack(empty));
  ASSERT_EQ(0, resp.error().code());
  WildcardMatchConfig config;
  ASSERT_TRUE(resp.data().UnpackTo(&config));
  ASSERT_EQ(2, config.rules_size());
  EXPECT_EQ(negative, config.rules(0).priority());
  EXPECT_EQ(big, config.rules(1).priority());
}

TEST_F(WildcardMatchTest, FailedSetRuntimeConfigLeavesStateUnchanged) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x1234, 0xffff, 5, kGateA)))
                   .error()
                   .code());

  // Second rule is invalid (value outside its mask), so the whole
  // configuration must be rejected and the installed one left serving.
  WildcardMatchConfig bad;
  bad.set_default_gate(kGateB);
  *bad.add_rules() = Rule(0x1234, 0xffff, 5, kGateB);
  *bad.add_rules() = Rule(0xffff, 0x00ff, 5, kGateB);
  EXPECT_NE(0, Run(m, "set_runtime_config", Pack(bad)).error().code());

  bess::pb::EmptyArg empty;
  CommandResponse resp = Run(m, "get_runtime_config", Pack(empty));
  ASSERT_EQ(0, resp.error().code());
  WildcardMatchConfig config;
  ASSERT_TRUE(resp.data().UnpackTo(&config));
  ASSERT_EQ(1, config.rules_size());
  EXPECT_EQ(kGateA, config.rules(0).gate());
  // The rejected configuration's default gate must not have been applied.
  EXPECT_EQ(DROP_GATE, config.default_gate());
}

TEST_F(WildcardMatchTest, RuntimeConfigRoundTripsByteIdentically) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  // Non-prefix masks, so the round trip has to preserve arbitrary bit patterns.
  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x0a0b, 0x0f0f, 1, kGateA)))
                   .error()
                   .code());
  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x0000, 0xf0f0, 2, kGateB)))
                   .error()
                   .code());

  bess::pb::EmptyArg empty;
  CommandResponse first = Run(m, "get_runtime_config", Pack(empty));
  ASSERT_EQ(0, first.error().code());
  WildcardMatchConfig config;
  ASSERT_TRUE(first.data().UnpackTo(&config));
  config.set_default_gate(kGateA);

  // Re-applying the exported configuration must reproduce it exactly.
  ASSERT_EQ(0, Run(m, "set_runtime_config", Pack(config)).error().code());
  CommandResponse second = Run(m, "get_runtime_config", Pack(empty));
  ASSERT_EQ(0, second.error().code());
  WildcardMatchConfig again;
  ASSERT_TRUE(second.data().UnpackTo(&again));

  EXPECT_EQ(config.default_gate(), again.default_gate());
  ASSERT_EQ(config.rules_size(), again.rules_size());
  for (int i = 0; i < config.rules_size(); i++) {
    EXPECT_EQ(config.rules(i).priority(), again.rules(i).priority());
    EXPECT_EQ(config.rules(i).gate(), again.rules(i).gate());
    ASSERT_EQ(config.rules(i).masks_size(), again.rules(i).masks_size());
    for (int f = 0; f < config.rules(i).masks_size(); f++) {
      EXPECT_EQ(config.rules(i).masks(f).value_bin(),
                again.rules(i).masks(f).value_bin());
      EXPECT_EQ(config.rules(i).values(f).value_bin(),
                again.rules(i).values(f).value_bin());
    }
  }
}

TEST_F(WildcardMatchTest, GetInitialArgReproducesFieldConfiguration) {
  WildcardMatchArg arg;
  *arg.add_fields() = OffsetField(23, 1);
  *arg.add_fields() = OffsetField(2, 2);
  WildcardMatch *m = Create(arg);
  ASSERT_NE(nullptr, m);

  bess::pb::EmptyArg empty;
  CommandResponse resp = Run(m, "get_initial_arg", Pack(empty));
  ASSERT_EQ(0, resp.error().code());
  WildcardMatchArg out;
  ASSERT_TRUE(resp.data().UnpackTo(&out));
  ASSERT_EQ(2, out.fields_size());
  EXPECT_EQ(23, out.fields(0).offset());
  EXPECT_EQ(1, out.fields(0).num_bytes());
  EXPECT_EQ(2, out.fields(1).offset());
  EXPECT_EQ(2, out.fields(1).num_bytes());
}

TEST_F(WildcardMatchTest, IntegerEncodedRulesAreAccepted) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  WildcardMatchCommandAddArg arg;
  arg.set_gate(kGateA);
  arg.set_priority(1);
  *arg.add_values() = Int(0x1234);
  *arg.add_masks() = Int(0xffff);
  EXPECT_EQ(0, Run(m, "add", Pack(arg)).error().code());

  bess::pb::EmptyArg empty;
  CommandResponse resp = Run(m, "get_runtime_config", Pack(empty));
  ASSERT_EQ(0, resp.error().code());
  WildcardMatchConfig config;
  ASSERT_TRUE(resp.data().UnpackTo(&config));
  ASSERT_EQ(1, config.rules_size());
  // value_int decodes big-endian into the field bytes (legacy uint64_to_bin
  // with big_endian=true), while value_bin is verbatim.
  EXPECT_EQ(std::string("\x12\x34", 2), config.rules(0).values(0).value_bin());
}

TEST_F(WildcardMatchTest, RejectsWideGateBeforeNarrowing) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  // Protobuf stores gate as uint64; 65536 would wrap to gate 0 if checked
  // only after narrowing to gate_idx_t.
  EXPECT_NE(0, Run(m, "add", Pack(Rule(0x1234, 0xffff, 1, 65536)))
                   .error()
                   .code());

  bess::pb::WildcardMatchCommandSetDefaultGateArg default_gate;
  default_gate.set_gate(65536);
  EXPECT_NE(0, Run(m, "set_default_gate", Pack(default_gate))
                   .error()
                   .code());
  WildcardMatchConfig config;
  config.set_default_gate(65536);
  EXPECT_NE(0, Run(m, "set_runtime_config", Pack(config)).error().code());
}

// Packets through the module (was bessctl/module_tests/wildcard_match.py).

using bess::testing::Bytes;
using bess::testing::Ip4;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::TcpPacket;
using google::protobuf::util::MessageDifferencer;
using Octets = std::vector<uint8_t>;

Field WmField(uint32_t offset, uint32_t bytes) {
  Field f;
  f.set_offset(offset);
  f.set_num_bytes(bytes);
  return f;
}

FieldData BinData(const Octets &bytes) {
  FieldData d;
  d.set_value_bin(std::string(bytes.begin(), bytes.end()));
  return d;
}

Octets IpOctets(const char *text) {
  const auto a = Ip4(text);
  return Octets(a.begin(), a.end());
}

WildcardMatchCommandAddArg WmRule(uint64_t gate, int64_t priority, const std::vector<Octets> &masks,
                                  const std::vector<Octets> &values) {
  WildcardMatchCommandAddArg arg;
  arg.set_gate(gate);
  arg.set_priority(priority);
  for (const Octets &m : masks) {
    *arg.add_masks() = BinData(m);
  }
  for (const Octets &v : values) {
    *arg.add_values() = BinData(v);
  }
  return arg;
}

WildcardMatchCommandDeleteArg WmKey(const std::vector<Octets> &masks,
                                    const std::vector<Octets> &values) {
  WildcardMatchCommandDeleteArg arg;
  for (const Octets &m : masks) {
    *arg.add_masks() = BinData(m);
  }
  for (const Octets &v : values) {
    *arg.add_values() = BinData(v);
  }
  return arg;
}

bess::pb::WildcardMatchCommandSetDefaultGateArg WmDefault(uint64_t gate) {
  bess::pb::WildcardMatchCommandSetDefaultGateArg arg;
  arg.set_gate(gate);
  return arg;
}

Module *CreateWm(ModuleHarness &h, const std::vector<Field> &fields) {
  WildcardMatchArg arg;
  for (const Field &f : fields) {
    *arg.add_fields() = f;
  }
  return h.Create("WildcardMatch", arg);
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

using GateCounts = std::map<gate_idx_t, size_t>;

const Octets kFull4 = {0xff, 0xff, 0xff, 0xff};

// Rules on (IPv4 src, IPv4 dst) send each packet, unchanged, out its rule's
// gate; a packet no rule matches takes the default gate. Re-adding a (mask,
// value) with another gate and priority replaces the rule.
TEST(WildcardMatchPacketTest, IpPairRulesPickTheGateAndMissesTakeTheDefault) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4), WmField(30, 4)});
  ASSERT_NE(nullptr, wm);
  const Octets dip = IpOctets("12.34.56.78");
  const std::vector<Octets> s1d = {IpOctets("65.43.21.0"), dip};
  const std::vector<Octets> s2d = {IpOctets("0.12.34.56"), dip};
  h.MustCommand(wm, "add", WmRule(0, 0, {kFull4, kFull4}, s1d));
  h.MustCommand(wm, "add", WmRule(1, 1, {kFull4, kFull4}, s1d));
  h.MustCommand(wm, "add", WmRule(0, 0, {kFull4, kFull4}, s2d));
  h.MustCommand(wm, "add", WmRule(2, 1, {kFull4, kFull4}, s2d));
  h.MustCommand(wm, "set_default_gate", WmDefault(3));

  std::mt19937 rng(1);
  const Bytes pkt1 = TcpPacket({.sip = "65.43.21.0", .dip = "12.34.56.78"}, rng);
  const Bytes pkt2 = TcpPacket({.sip = "0.12.34.56", .dip = "12.34.56.78"}, rng);
  const Bytes miss = TcpPacket({.sip = "0.12.33.56", .dip = "12.34.56.78"}, rng);

  EXPECT_TRUE(Counts(h.Run(wm, 0, {}, {0, 1, 2, 3})).empty());
  auto out = h.Run(wm, 0, {pkt1}, {0, 1, 2, 3});
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(out));
  EXPECT_EQ((std::vector<Bytes>{pkt1}), out[1]);
  out = h.Run(wm, 0, {pkt2}, {0, 1, 2, 3});
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(out));
  EXPECT_EQ((std::vector<Bytes>{pkt2}), out[2]);
  out = h.Run(wm, 0, {miss}, {0, 1, 2, 3});
  EXPECT_EQ((GateCounts{{3, 1}}), Counts(out));
  EXPECT_EQ((std::vector<Bytes>{miss}), out[3]);
}

// Masks need not be prefixes: arbitrary bits inside each byte are matched,
// the others are wildcards.
TEST(WildcardMatchPacketTest, NonPrefixMasksMatchOnlyTheMaskedBits) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, wm);
  h.MustCommand(wm, "add", WmRule(1, 1, {{0x0f, 0xf0, 0x00, 0xff}}, {{0x0a, 0x30, 0x00, 0x44}}));
  h.MustCommand(wm, "set_default_gate", WmDefault(2));

  std::mt19937 rng(2);
  // 26.51.119.68 = 1a 33 77 44: the masked bits match, the wildcard bits
  // differ from the rule's.
  const Bytes hit = TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.1"}, rng);
  const Bytes miss = TcpPacket({.sip = "26.51.119.69", .dip = "10.0.0.1"}, rng);
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(wm, 0, {hit}, {0, 1, 2})));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {miss}, {0, 1, 2})));
}

// 1-, 3- and 7-byte fields: the key is not a multiple of eight bytes and no
// field is word-sized; every byte of every field counts.
TEST(WildcardMatchPacketTest, MixedFieldSizesFormTheKey) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 1), WmField(27, 3), WmField(30, 7)});
  ASSERT_NE(nullptr, wm);
  std::mt19937 rng(3);
  const Bytes pkt =
      TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.1", .sport = 1000, .dport = 2000}, rng);
  h.MustCommand(wm, "add",
                WmRule(1, 1, {Octets(1, 0xff), Octets(3, 0xff), Octets(7, 0xff)},
                       {Octets(pkt.begin() + 26, pkt.begin() + 27),
                        Octets(pkt.begin() + 27, pkt.begin() + 30),
                        Octets(pkt.begin() + 30, pkt.begin() + 37)}));
  h.MustCommand(wm, "set_default_gate", WmDefault(2));
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2})));

  // Only the last byte of the destination address (inside the 7-byte field)
  // differs.
  const Bytes other =
      TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.9", .sport = 1000, .dport = 2000}, rng);
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {other}, {0, 1, 2})));
}

// Two different masks match the same packet: the higher priority wins
// whichever was added first; at equal priority the later command wins.
TEST(WildcardMatchPacketTest, PriorityDecidesAcrossMasksAndTiesGoToTheLaterCommand) {
  ModuleHarness h;
  const Octets wild_mask = {0xff, 0x00, 0x00, 0x00};
  const Octets wild = {0x1a, 0x00, 0x00, 0x00};
  const Octets exact = {0x1a, 0x33, 0x77, 0x44};
  std::mt19937 rng(4);
  const Bytes pkt = TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.1"}, rng);

  Module *wm = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, wm);
  h.MustCommand(wm, "add", WmRule(1, 1, {wild_mask}, {wild}));
  h.MustCommand(wm, "add", WmRule(2, 9, {kFull4}, {exact}));
  h.MustCommand(wm, "set_default_gate", WmDefault(3));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2, 3})));

  Module *tie = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, tie);
  h.MustCommand(tie, "add", WmRule(1, 5, {wild_mask}, {wild}));
  h.MustCommand(tie, "add", WmRule(2, 5, {kFull4}, {exact}));
  h.MustCommand(tie, "set_default_gate", WmDefault(3));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(tie, 0, {pkt}, {0, 1, 2, 3})));

  Module *reversed = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, reversed);
  h.MustCommand(reversed, "add", WmRule(2, 5, {kFull4}, {exact}));
  h.MustCommand(reversed, "add", WmRule(1, 5, {wild_mask}, {wild}));
  h.MustCommand(reversed, "set_default_gate", WmDefault(3));
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(reversed, 0, {pkt}, {0, 1, 2, 3})));
}

// A deleted rule no longer matches; clear() frees the masks' capacity: eight
// new masks fit afterwards, and they are the ones packets now see (the last
// matches the probe).
TEST(WildcardMatchPacketTest, DeleteAndClearChangeWhatPacketsMatch) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, wm);
  const Octets addr = IpOctets("26.51.119.68");
  h.MustCommand(wm, "add", WmRule(1, 1, {kFull4}, {addr}));
  h.MustCommand(wm, "set_default_gate", WmDefault(2));
  std::mt19937 rng(5);
  const Bytes pkt = TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.1"}, rng);
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2})));

  h.MustCommand(wm, "delete", WmKey({kFull4}, {addr}));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2})));

  for (uint8_t i = 0; i < 8; i++) {
    h.MustCommand(wm, "add", WmRule(1, 1, {{0xff, 0, 0, i}}, {{0x1a, 0, 0, i}}));
  }
  h.MustCommand(wm, "clear", bess::pb::EmptyArg());
  for (uint8_t i = 0; i < 8; i++) {
    h.MustCommand(wm, "add", WmRule(1, 1, {{0, 0xff, i, 0}}, {{0, 0x33, i, 0}}));
  }
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2})));
}

// A field reaching past the end of the packet is not read: the packet takes
// the default gate (an all-zero read would have matched the rule).
TEST(WildcardMatchPacketTest, ShortPacketTakesTheDefaultGate) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(30, 4)});
  ASSERT_NE(nullptr, wm);
  h.MustCommand(wm, "add", WmRule(1, 1, {kFull4}, {{0, 0, 0, 0}}));
  h.MustCommand(wm, "set_default_gate", WmDefault(2));
  const Bytes short_pkt(20, 0);
  auto out = h.Run(wm, 0, {short_pkt}, {0, 1, 2});
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(out));
  EXPECT_EQ((std::vector<Bytes>{short_pkt}), out[2]);
}

// A field may be a metadata attribute: three SetMetadata writers feed one
// WildcardMatch through a Merge, and each packet goes where its attribute's
// value sends it (no rule: the default gate).
TEST(WildcardMatchPacketTest, MatchesOnAMetadataAttribute) {
  ModuleHarness h;
  Field attr;
  attr.set_attr_name("sangjin");
  attr.set_num_bytes(2);
  Module *wm = CreateWm(h, {attr});
  ASSERT_NE(nullptr, wm);
  h.MustCommand(wm, "add", WmRule(1, 0, {{0xff, 0xff}}, {{0x88, 0x80}}));
  h.MustCommand(wm, "add", WmRule(2, 0, {{0xff, 0xff}}, {{0x77, 0x70}}));
  h.MustCommand(wm, "set_default_gate", WmDefault(0));

  Module *merge = h.Create("Merge", bess::pb::MergeArg());
  ASSERT_NE(nullptr, merge);
  // No default gate hooks: Track counts per worker, and the harness runs
  // off any worker.
  ASSERT_EQ(0, ModuleGraph::ConnectModules(merge, 0, wm, 0, true));
  std::vector<Module *> tags;
  for (const char *value : {"\x77\x90", "\x88\x80", "\x77\x70"}) {
    bess::pb::SetMetadataArg tag;
    auto *a = tag.add_attrs();
    a->set_name("sangjin");
    a->set_size(2);
    a->set_value_bin(std::string(value, 2));
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
    auto out = h.RunPipeline(tags[i], 0, {pkt}, wm, {0, 1, 2});
    EXPECT_EQ((GateCounts{{i, 1}}), Counts(out)) << "tag " << i;
    EXPECT_EQ((std::vector<Bytes>{pkt}), out[i]) << "tag " << i;
  }
}

// get_initial_arg reproduces the creation argument (a metadata and a packet
// field); get_runtime_config the default gate and the rules, sorted by
// priority.
TEST(WildcardMatchPacketTest, InitialArgAndRuntimeConfigReproduceTheModule) {
  ModuleHarness h;
  WildcardMatchArg arg;
  Field attr;
  attr.set_attr_name("babylon5");
  attr.set_num_bytes(2);
  *arg.add_fields() = attr;
  *arg.add_fields() = WmField(10, 1);
  Module *wm = h.Create("WildcardMatch", arg);
  ASSERT_NE(nullptr, wm);
  const WildcardMatchCommandAddArg r1 = WmRule(1, 1, {{0xff, 0xf0}, {0x7f}}, {{0x88, 0x80}, {0x03}});
  const WildcardMatchCommandAddArg r2 = WmRule(2, 2, {{0xf0, 0xff}, {0x3f}}, {{0x70, 0x70}, {0x05}});
  h.MustCommand(wm, "add", r2);  // added out of order: the answer is sorted
  h.MustCommand(wm, "add", r1);
  h.MustCommand(wm, "set_default_gate", WmDefault(3));

  WildcardMatchConfig expect;
  expect.set_default_gate(3);
  *expect.add_rules() = r1;
  *expect.add_rules() = r2;

  WildcardMatchArg initial;
  ASSERT_TRUE(h.MustCommand(wm, "get_initial_arg", bess::pb::EmptyArg()).UnpackTo(&initial));
  EXPECT_TRUE(MessageDifferencer::Equals(arg, initial)) << initial.DebugString();
  WildcardMatchConfig config;
  ASSERT_TRUE(h.MustCommand(wm, "get_runtime_config", bess::pb::EmptyArg()).UnpackTo(&config));
  EXPECT_TRUE(MessageDifferencer::Equals(expect, config)) << config.DebugString();
}

// D-129: the rules and the default gate as resources.

FieldData IntData(uint64_t value) {
  FieldData d;
  d.set_value_int(value);
  return d;
}

bess::pb::WildcardMatchRuleKey WmRuleKey(const std::vector<FieldData> &masks,
                                         const std::vector<FieldData> &values) {
  bess::pb::WildcardMatchRuleKey key;
  for (const FieldData &m : masks) {
    *key.add_masks() = m;
  }
  for (const FieldData &v : values) {
    *key.add_values() = v;
  }
  return key;
}

bess::pb::WildcardMatchRuleValue WmRuleValue(int64_t priority, uint64_t gate) {
  bess::pb::WildcardMatchRuleValue value;
  value.set_priority(priority);
  value.set_gate(gate);
  return value;
}

bess::pb::WildcardMatchSettings WmSettings(uint64_t default_gate) {
  bess::pb::WildcardMatchSettings settings;
  settings.set_default_gate(default_gate);
  return settings;
}

// Rules of different masks and priorities upserted as "<module>/rules"
// classify packets as the add command's would, read back as the same
// entries (an integer-encoded key reads back as bytes and still compares
// equal through the codec), and an erase takes one out.
TEST(WildcardMatchResourceTest, RulesResourceClassifiesAndRoundTrips) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4), WmField(30, 4)});  // sip, dip
  ASSERT_NE(nullptr, wm);
  const std::string rules = wm->name() + "/rules";
  const Octets zero4 = {0, 0, 0, 0};

  // Source exact, destination wild: priority 10, gate 1.
  const auto key_a = WmRuleKey({BinData(kFull4), BinData(zero4)},
                               {BinData(IpOctets("26.51.119.68")), BinData(zero4)});
  const auto val_a = WmRuleValue(10, 1);
  // Source 26.0.0.0/8 (integer-encoded), destination wild: priority 5, gate 2.
  const auto key_b = WmRuleKey({IntData(0xff000000), BinData(zero4)},
                               {IntData(0x1a000000), BinData(zero4)});
  const auto val_b = WmRuleValue(5, 2);
  // Source wild, destination exact: priority 20, gate 3.
  const auto key_c = WmRuleKey({BinData(zero4), BinData(kFull4)},
                               {BinData(zero4), BinData(IpOctets("10.0.0.1"))});
  const auto val_c = WmRuleValue(20, 3);
  ASSERT_EQ("", h.UpsertResource(rules, key_a, val_a));
  ASSERT_EQ("", h.UpsertResource(rules, key_b, val_b));
  ASSERT_EQ("", h.UpsertResource(rules, key_c, val_c));
  ASSERT_EQ("", h.SetResource(wm->name() + "/settings", WmSettings(4)));

  std::mt19937 rng(7);
  const Bytes all = TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.1"}, rng);
  const Bytes a_b = TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.2"}, rng);
  const Bytes b = TcpPacket({.sip = "26.1.1.1", .dip = "10.0.0.2"}, rng);
  const Bytes none = TcpPacket({.sip = "27.0.0.1", .dip = "10.0.0.2"}, rng);
  const std::vector<gate_idx_t> gates = {0, 1, 2, 3, 4};
  EXPECT_EQ((GateCounts{{3, 1}}), Counts(h.Run(wm, 0, {all}, gates)));
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(wm, 0, {a_b}, gates)));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {b}, gates)));
  EXPECT_EQ((GateCounts{{4, 1}}), Counts(h.Run(wm, 0, {none}, gates)));

  EXPECT_EQ("", h.ReadBackDiffers(rules, {{&key_a, &val_a}, {&key_b, &val_b}, {&key_c, &val_c}}));
  EXPECT_EQ(3u, h.ReadResource(rules).size());

  ASSERT_EQ("", h.EraseResource(rules, key_b));
  EXPECT_EQ((GateCounts{{4, 1}}), Counts(h.Run(wm, 0, {b}, gates)));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{&key_a, &val_a}, {&key_c, &val_c}}));

  // A rule the legacy command adds is read back too.
  h.MustCommand(wm, "add", WmRule(2, 7, {{0xff, 0, 0, 0}, zero4}, {{0x1b, 0, 0, 0}, zero4}));
  const auto key_d = WmRuleKey({BinData({0xff, 0, 0, 0}), BinData(zero4)},
                               {BinData({0x1b, 0, 0, 0}), BinData(zero4)});
  const auto val_d = WmRuleValue(7, 2);
  EXPECT_EQ("", h.ReadBackDiffers(rules, {{&key_a, &val_a}, {&key_c, &val_c}, {&key_d, &val_d}}));
}

// A rule with a bad gate, a wrong field count or a non-canonical value/mask
// pair is refused, and nothing changes.
TEST(WildcardMatchResourceTest, RulesResourceRefusesBadRules) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, wm);
  const std::string rules = wm->name() + "/rules";
  const auto key = WmRuleKey({BinData(kFull4)}, {BinData(IpOctets("26.51.119.68"))});
  EXPECT_NE("", h.UpsertResource(rules, key, WmRuleValue(1, uint64_t{1} << 20)));
  EXPECT_NE("", h.UpsertResource(rules, WmRuleKey({BinData(kFull4), BinData(kFull4)},
                                                  {BinData(kFull4), BinData(kFull4)}),
                                 WmRuleValue(1, 1)));
  EXPECT_NE("", h.UpsertResource(rules, WmRuleKey({BinData({0xff, 0, 0, 0})}, {BinData(kFull4)}),
                                 WmRuleValue(1, 1)));
  EXPECT_EQ("", h.ReadBackDiffers(rules, {}));
}

// "<module>/settings" sets the default gate (what set_default_gate does),
// reads back as set, and refuses a gate out of range, keeping the old one.
TEST(WildcardMatchResourceTest, SettingsSetTheDefaultGate) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, wm);
  const std::string settings = wm->name() + "/settings";
  h.MustCommand(wm, "add", WmRule(1, 1, {kFull4}, {IpOctets("26.51.119.68")}));
  std::mt19937 rng(8);
  const Bytes hit = TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.1"}, rng);
  const Bytes miss = TcpPacket({.sip = "26.51.119.69", .dip = "10.0.0.1"}, rng);
  EXPECT_TRUE(Counts(h.Run(wm, 0, {miss}, {0, 1, 2, 3})).empty()) << "dropped by default";

  const auto two = WmSettings(2);
  ASSERT_EQ("", h.SetResource(settings, two));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {miss}, {0, 1, 2, 3})));
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(wm, 0, {hit}, {0, 1, 2, 3})));
  EXPECT_EQ("", h.ReadBackDiffers(settings, {{nullptr, &two}}));

  EXPECT_NE("", h.SetResource(settings, WmSettings(uint64_t{1} << 20)));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {miss}, {0, 1, 2, 3})));
  EXPECT_EQ("", h.ReadBackDiffers(settings, {{nullptr, &two}}));

  // The legacy setter and the resource are the same state.
  h.MustCommand(wm, "set_default_gate", WmDefault(3));
  const auto three = WmSettings(3);
  EXPECT_EQ("", h.ReadBackDiffers(settings, {{nullptr, &three}}));
  EXPECT_EQ((GateCounts{{3, 1}}), Counts(h.Run(wm, 0, {miss}, {0, 1, 2, 3})));

  // Gate 0 is the proto3 default: it reads back as an empty message.
  const auto zero = WmSettings(0);
  ASSERT_EQ("", h.SetResource(settings, zero));
  EXPECT_EQ((GateCounts{{0, 1}}), Counts(h.Run(wm, 0, {miss}, {0, 1, 2, 3})));
  EXPECT_EQ("", h.ReadBackDiffers(settings, {{nullptr, &zero}}));
}

// Every WildcardMatch command edits or reads back resource state: control_v2
// does not run any of them as an action, while the legacy command still does.
TEST(WildcardMatchResourceTest, CommandsAreLegacyOnly) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, wm);
  const auto rule = WmRule(1, 1, {kFull4}, {IpOctets("26.51.119.68")});
  const bess::pb::EmptyArg empty;
  EXPECT_EQ(ENOTSUP, h.Action(wm, "add", rule).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(wm, "delete", WmKey({kFull4}, {IpOctets("26.51.119.68")}))
                         .error()
                         .code());
  EXPECT_EQ(ENOTSUP, h.Action(wm, "clear", empty).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(wm, "set_default_gate", WmDefault(1)).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(wm, "get_initial_arg", empty).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(wm, "get_runtime_config", empty).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(wm, "set_runtime_config", WildcardMatchConfig()).error().code());
  EXPECT_EQ("", h.ReadBackDiffers(wm->name() + "/rules", {})) << "the refused add did nothing";

  EXPECT_EQ(0, h.Command(wm, "add", rule).error().code());
  EXPECT_EQ(1u, h.ReadResource(wm->name() + "/rules").size());
}

}  // namespace
