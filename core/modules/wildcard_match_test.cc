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
#include "utils/logging.h"

namespace {
// A refused Init must return no module; ModuleHarness treats a refusal as a
// test failure, so negative cases go through the graph directly.
bool CreateRefused(const std::string &class_name,
                   const google::protobuf::Message &arg) {
  const auto &builders = ModuleBuilder::all_module_builders();
  const auto it = builders.find(class_name);
  if (it == builders.end()) {
    return false;
  }
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) {
    return false;
  }
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(it->second, "refused", packed, &perr);
  EXPECT_EQ(nullptr, m) << perr.errmsg();
  return m == nullptr && perr.code() != 0;
}
}  // namespace

#include "testing/rng.h"
#include "testing/packet_builder.h"

namespace {

using bess::pb::Field;
using bess::pb::FieldData;
using bess::pb::WildcardMatchArg;
using bess::pb::WildcardMatchCommandAddArg;

constexpr gate_idx_t kGateA = 1;
constexpr gate_idx_t kGateB = 2;

// The shape the legacy WildcardMatchConfig wire message carried (rules and a
// default gate). That message went with the legacy service (D-140); the tests
// compare configurations, so they compare this.
struct WmRuleSpec {
  bess::pb::WildcardMatchRuleKey key;  // masks and values
  int64_t priority_value = 0;
  uint64_t gate_value = 0;
  int64_t priority() const { return priority_value; }
  uint64_t gate() const { return gate_value; }
  void set_priority(int64_t p) { priority_value = p; }
  void set_gate(uint64_t g) { gate_value = g; }
  int masks_size() const { return key.masks_size(); }
  int values_size() const { return key.values_size(); }
  const FieldData &masks(int i) const { return key.masks(i); }
  const FieldData &values(int i) const { return key.values(i); }
  FieldData *add_masks() { return key.add_masks(); }
  FieldData *add_values() { return key.add_values(); }

  WmRuleSpec() = default;
  // A rule as the legacy `add` command built it: the live argument shape
  // carries the same masks, values, priority and gate.
  explicit WmRuleSpec(const bess::pb::WildcardMatchCommandAddArg &arg) {
    *key.mutable_values() = arg.values();
    *key.mutable_masks() = arg.masks();
    priority_value = arg.priority();
    gate_value = arg.gate();
  }
};

struct WmConfig {
  std::vector<WmRuleSpec> rule_list;
  uint64_t gate_value = 0;
  int rules_size() const { return static_cast<int>(rule_list.size()); }
  const WmRuleSpec &rules(int i) const { return rule_list[i]; }
  const std::vector<WmRuleSpec> &rules() const { return rule_list; }
  std::vector<WmRuleSpec> *mutable_rules() { return &rule_list; }
  WmRuleSpec *mutable_rules(int i) { return &rule_list[i]; }
  WmRuleSpec *add_rules() {
    rule_list.emplace_back();
    return &rule_list.back();
  }
  uint64_t default_gate() const { return gate_value; }
  void set_default_gate(uint64_t g) { gate_value = g; }
  std::string DebugString() const {
    std::string out = "default_gate: " + std::to_string(gate_value);
    for (const WmRuleSpec &r : rule_list) {
      out += "\nrule: priority " + std::to_string(r.priority()) + " gate " +
             std::to_string(r.gate());
    }
    return out;
  }
  bool operator==(const WmConfig &o) const {
    if (gate_value != o.gate_value || rule_list.size() != o.rule_list.size()) {
      return false;
    }
    for (size_t i = 0; i < rule_list.size(); i++) {
      const WmRuleSpec &a = rule_list[i];
      const WmRuleSpec &b = o.rule_list[i];
      if (a.priority() != b.priority() || a.gate() != b.gate() ||
          a.masks_size() != b.masks_size() ||
          a.values_size() != b.values_size()) {
        return false;
      }
      for (int f = 0; f < a.masks_size(); f++) {
        if (a.masks(f).SerializeAsString() != b.masks(f).SerializeAsString() ||
            a.values(f).SerializeAsString() !=
                b.values(f).SerializeAsString()) {
          return false;
        }
      }
    }
    return true;
  }
};

// The rules and the settings of `m` as its resources read them, sorted with
// SortRules; defined with the other resource helpers below.
WmConfig ReadConfig(bess::testing::ModuleHarness &h, Module *m);

// The rules of `config` in a stable order (priority, gate, masks, values). The
// rule table's own iteration order is not part of what the module promises, so
// a test that compares two whole configurations sorts both sides with this.
void SortRules(WmConfig *config) {
  std::sort(config->mutable_rules()->begin(), config->mutable_rules()->end(),
            [](const WmRuleSpec &a, const WmRuleSpec &b) {
              if (a.priority() != b.priority()) {
                return a.priority() < b.priority();
              }
              if (a.gate() != b.gate()) {
                return a.gate() < b.gate();
              }
              for (int i = 0; i < a.masks_size(); i++) {
                if (a.masks(i).value_bin() != b.masks(i).value_bin()) {
                  return a.masks(i).value_bin() < b.masks(i).value_bin();
                }
              }
              for (int i = 0; i < a.values_size(); i++) {
                if (a.values(i).value_bin() != b.values(i).value_bin()) {
                  return a.values(i).value_bin() < b.values(i).value_bin();
                }
              }
              return false;
            });
}

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

  // The legacy commands as a client runs them now (D-140): "add" and "delete"
  // write "<name>/rules", "clear" erases them, "set_default_gate" writes
  // "<name>/settings", each through the process's transaction engine. A
  // refusal carries EINVAL and the reason; the resource layer reports no errno.
  CommandResponse Run(WildcardMatch *m, const char *cmd,
                      const google::protobuf::Any &arg) {
    const std::string rules = std::string(m->name()) + "/rules";
    const std::string_view command(cmd);
    if (command == "add") {
      bess::pb::WildcardMatchCommandAddArg add;
      if (!arg.UnpackTo(&add)) {
        return CommandFailure(EINVAL, "not an add argument");
      }
      bess::pb::WildcardMatchRuleKey key;
      *key.mutable_values() = add.values();
      *key.mutable_masks() = add.masks();
      bess::pb::WildcardMatchRuleValue value;
      value.set_priority(add.priority());
      value.set_gate(add.gate());
      return Report(h_.UpsertResource(rules, key, value));
    }
    if (command == "delete") {
      bess::pb::WildcardMatchRuleKey key;
      if (!arg.UnpackTo(&key)) {
        return CommandFailure(EINVAL, "not a delete argument");
      }
      return Report(h_.EraseResource(rules, key));
    }
    if (command == "clear") {
      for (const auto &entry : h_.ReadResource(rules)) {
        bess::pb::WildcardMatchRuleKey key;
        if (!entry.first.UnpackTo(&key)) {
          return CommandFailure(EINVAL, "a rule does not decode");
        }
        const CommandResponse erased = Report(h_.EraseResource(rules, key));
        if (erased.error().code() != 0) {
          return erased;
        }
      }
      return CommandSuccess();
    }
    if (command == "set_default_gate") {
      bess::pb::WildcardMatchSettings settings;
      if (!arg.UnpackTo(&settings)) {
        return CommandFailure(EINVAL, "not a set_default_gate argument");
      }
      return Report(
          h_.SetResource(std::string(m->name()) + "/settings", settings));
    }
    // No readback command: the resources are the state, and a test reads them
    // with ReadConfig (as the resource tests do).
    return CommandFailure(EINVAL, "no such command: %s", cmd);
  }

  // One op per key, in one transaction: every rule the configuration does not
  // list is erased, every listed rule upserted (duplicates collapse to the
  // last), then the default gate is set. A refusal applies none of it -- the
  // legacy set_runtime_config built the new table in full and swapped it in.
  CommandResponse Restore(WildcardMatch *m, const WmConfig &config) {
    const std::string rules = std::string(m->name()) + "/rules";
    const std::string settings = std::string(m->name()) + "/settings";
    const bess::framework::ResourceCodec *codec = h_.Codec(rules);
    if (codec == nullptr) {
      return CommandFailure(EINVAL, "no resource '%s'", rules.c_str());
    }
    std::map<bess::dataplane::ResourceKey, std::any> desired;
    for (const WmRuleSpec &rule : config.rules()) {
      bess::pb::WildcardMatchRuleKey key = rule.key;
      bess::pb::WildcardMatchRuleValue value;
      value.set_priority(rule.priority());
      value.set_gate(rule.gate());
      google::protobuf::Any packed_key;
      google::protobuf::Any packed_value;
      EXPECT_TRUE(packed_key.PackFrom(key));
      EXPECT_TRUE(packed_value.PackFrom(value));
      auto k = codec->Key(packed_key.type_url(), packed_key.value());
      auto v = codec->Value(packed_value.type_url(), packed_value.value());
      if (!k) {
        return CommandFailure(EINVAL, "%s", k.error().c_str());
      }
      if (!v) {
        return CommandFailure(EINVAL, "%s", v.error().c_str());
      }
      desired[std::move(*k)] = std::move(*v);
    }
    std::vector<bess::dataplane::Op> ops;
    for (const auto &entry : h_.ReadResource(rules)) {
      auto key = codec->Key(entry.first.type_url(), entry.first.value());
      if (!key) {
        return CommandFailure(EINVAL, "%s", key.error().c_str());
      }
      if (desired.contains(*key)) {
        continue;
      }
      ops.push_back(bess::dataplane::Op::Erase(rules, std::move(*key)));
    }
    for (auto &[key, value] : desired) {
      ops.push_back(bess::dataplane::Op::Upsert(rules, key, std::move(value)));
    }
    const std::string error = h_.Apply(ops);
    if (!error.empty()) {
      return CommandFailure(EINVAL, "%s", error.c_str());
    }
    return SetDefaultGate(m, config.default_gate());
  }

  CommandResponse SetDefaultGate(WildcardMatch *m, uint64_t gate) {
    bess::pb::WildcardMatchSettings settings;
    settings.set_default_gate(gate);
    return Report(h_.SetResource(std::string(m->name()) + "/settings", settings));
  }

  // A resource write's refusal: "" applied, else EINVAL with the reason (the
  // resource layer carries no errno).
  static CommandResponse Report(const std::string &error) {
    if (error.empty()) {
      return CommandSuccess();
    }
    return CommandFailure(EINVAL, "%s", error.c_str());
  }

  template <typename T>
  static google::protobuf::Any Pack(const T &arg) {
    google::protobuf::Any packed;
    CHECK(packed.PackFrom(arg));
    return packed;
  }

  bess::testing::ModuleHarness h_;

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

  const WmConfig config = ReadConfig(h_, m);
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
  // The ninth distinct mask is refused (master reported ENOSPC; the resource
  // layer reports the reason, not an errno).
  EXPECT_NE(0, Run(m, "add", Pack(Rule(0, 0xabcd, 9, kGateA))).error().code());
  const WmConfig held_config = ReadConfig(h_, m);
  EXPECT_EQ(8, held_config.rules_size()) << "the refused rule was not added";

  // clear() drops the rules *and* their masks, so capacity is genuinely free.
  bess::pb::EmptyArg empty;
  ASSERT_EQ(0, Run(m, "clear", Pack(empty)).error().code());
  for (int i = 0; i < 8; i++) {
    const uint64_t mask = 0x00ffu | (static_cast<uint64_t>(i) << 8);
    ASSERT_EQ(0, Run(m, "add", Pack(Rule(0, mask, i, kGateB))).error().code());
  }
  EXPECT_NE(0, Run(m, "add", Pack(Rule(0, 0x1111, 9, kGateB))).error().code());
}

TEST_F(WildcardMatchTest, MaxTuplesSetsTheCeiling) {
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
  EXPECT_NE(0, Run(m, "add", Pack(Rule(0, 0xabcd, 13, kGateA))).error().code())
      << "the ceiling is 12 tuples";
}

// The default gate may be given at creation; an invalid one refuses the
// module, and the one given is what "<name>/settings" holds.
TEST_F(WildcardMatchTest, DefaultGateAtCreation) {
  WildcardMatchArg arg = TwoByteField();
  arg.set_default_gate(uint64_t{1} << 20);
  EXPECT_EQ(nullptr, Create(arg)) << "an invalid gate";

  arg.set_default_gate(kGateB);
  WildcardMatch *m = Create(arg);
  ASSERT_NE(nullptr, m);
  const WmConfig runtime = ReadConfig(h_, m);
  EXPECT_EQ(kGateB, runtime.default_gate());
}

TEST_F(WildcardMatchTest, DeleteExistingAndMissing) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x1234, 0xffff, 1, kGateA)))
                   .error()
                   .code());

  bess::pb::WildcardMatchRuleKey del;
  *del.add_values() = Bin({0x34, 0x12});
  *del.add_masks() = Bin({0xff, 0xff});
  EXPECT_EQ(0, Run(m, "delete", Pack(del)).error().code());
  // Deleting again reports not-found rather than succeeding.
  EXPECT_NE(0, Run(m, "delete", Pack(del)).error().code());

  // A missing key inside an existing mask is also not-found.
  ASSERT_EQ(0, Run(m, "add", Pack(Rule(0x1234, 0xffff, 1, kGateA)))
                   .error()
                   .code());
  bess::pb::WildcardMatchRuleKey other;
  *other.add_values() = Bin({0x99, 0x99});
  *other.add_masks() = Bin({0xff, 0xff});
  EXPECT_NE(0, Run(m, "delete", Pack(other)).error().code());
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

  const WmConfig config = ReadConfig(h_, m);
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
  WmConfig bad;
  bad.set_default_gate(kGateB);
  *bad.add_rules() = WmRuleSpec(Rule(0x1234, 0xffff, 5, kGateB));
  *bad.add_rules() = WmRuleSpec(Rule(0xffff, 0x00ff, 5, kGateB));
  EXPECT_NE(0, Restore(m, bad).error().code());

  const WmConfig config = ReadConfig(h_, m);
  ASSERT_EQ(1, config.rules_size());
  EXPECT_EQ(kGateA, config.rules(0).gate());
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

  WmConfig config = ReadConfig(h_, m);
  config.set_default_gate(kGateA);

  // Re-applying the exported configuration must reproduce it exactly.
  ASSERT_EQ(0, Restore(m, config).error().code());
  const WmConfig again = ReadConfig(h_, m);

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

TEST_F(WildcardMatchTest, IntegerEncodedRulesAreAccepted) {
  WildcardMatch *m = Create(TwoByteField());
  ASSERT_NE(nullptr, m);

  WildcardMatchCommandAddArg arg;
  arg.set_gate(kGateA);
  arg.set_priority(1);
  *arg.add_values() = Int(0x1234);
  *arg.add_masks() = Int(0xffff);
  EXPECT_EQ(0, Run(m, "add", Pack(arg)).error().code());

  const WmConfig config = ReadConfig(h_, m);
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

  bess::pb::WildcardMatchSettings default_gate;
  default_gate.set_default_gate(65536);
  EXPECT_NE(0, Run(m, "set_default_gate", Pack(default_gate))
                   .error()
                   .code());
  WmConfig config;
  config.set_default_gate(65536);
  EXPECT_NE(0, Restore(m, config).error().code());
}

// Packets through the module (was bessctl/module_tests/wildcard_match.py).

using bess::testing::Bytes;
using bess::testing::Ip4;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;
using bess::testing::TcpPacket;
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

bess::pb::WildcardMatchRuleKey WmKey(const std::vector<Octets> &masks,
                                     const std::vector<Octets> &values) {
  bess::pb::WildcardMatchRuleKey arg;
  for (const Octets &m : masks) {
    *arg.add_masks() = BinData(m);
  }
  for (const Octets &v : values) {
    *arg.add_values() = BinData(v);
  }
  return arg;
}

bess::pb::WildcardMatchSettings WmDefault(uint64_t gate) {
  bess::pb::WildcardMatchSettings arg;
  arg.set_default_gate(gate);
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

// Rule and setting writes as a client sends them (D-140): "<name>/rules" and
// "<name>/settings" through the process's transaction engine -- the path the
// legacy commands took. "" when it applied, else why not.
std::string AddRule(ModuleHarness &h, Module *m, const WildcardMatchCommandAddArg &rule) {
  bess::pb::WildcardMatchRuleKey key;
  *key.mutable_values() = rule.values();
  *key.mutable_masks() = rule.masks();
  bess::pb::WildcardMatchRuleValue value;
  value.set_priority(rule.priority());
  value.set_gate(rule.gate());
  return h.UpsertResource(std::string(m->name()) + "/rules", key, value);
}

std::string DeleteRule(ModuleHarness &h, Module *m,
                       const bess::pb::WildcardMatchRuleKey &rule) {
  return h.EraseResource(std::string(m->name()) + "/rules", rule);
}

std::string ClearRules(ModuleHarness &h, Module *m) {
  const std::string rules = std::string(m->name()) + "/rules";
  for (const auto &entry : h.ReadResource(rules)) {
    bess::pb::WildcardMatchRuleKey key;
    if (!entry.first.UnpackTo(&key)) {
      return "a rule does not decode";
    }
    const std::string error = h.EraseResource(rules, key);
    if (!error.empty()) {
      return error;
    }
  }
  return "";
}

std::string SetDefaultGate(ModuleHarness &h, Module *m, uint64_t gate) {
  bess::pb::WildcardMatchSettings settings;
  settings.set_default_gate(gate);
  return h.SetResource(std::string(m->name()) + "/settings", settings);
}

// What "<name>/rules" and "<name>/settings" hold, sorted with SortRules: the
// resources are the state, and there is no other readback.
WmConfig ReadConfig(ModuleHarness &h, Module *m) {
  WmConfig config;
  for (const auto &entry : h.ReadResource(std::string(m->name()) + "/rules")) {
    bess::pb::WildcardMatchRuleKey key;
    bess::pb::WildcardMatchRuleValue value;
    EXPECT_TRUE(entry.first.UnpackTo(&key));
    EXPECT_TRUE(entry.second.UnpackTo(&value));
    WmRuleSpec *rule = config.add_rules();
    rule->set_priority(value.priority());
    rule->set_gate(value.gate());
    *rule->key.mutable_values() = key.values();
    *rule->key.mutable_masks() = key.masks();
  }
  SortRules(&config);
  bess::pb::WildcardMatchSettings held;
  const auto entries = h.ReadResource(std::string(m->name()) + "/settings");
  EXPECT_EQ(1u, entries.size());
  EXPECT_TRUE(entries[0].second.UnpackTo(&held));
  config.set_default_gate(held.default_gate());
  return config;
}

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
  EXPECT_EQ("", AddRule(h, wm, WmRule(0, 0, {kFull4, kFull4}, s1d)));
  EXPECT_EQ("", AddRule(h, wm, WmRule(1, 1, {kFull4, kFull4}, s1d)));
  EXPECT_EQ("", AddRule(h, wm, WmRule(0, 0, {kFull4, kFull4}, s2d)));
  EXPECT_EQ("", AddRule(h, wm, WmRule(2, 1, {kFull4, kFull4}, s2d)));
  EXPECT_EQ("", SetDefaultGate(h, wm, 3));

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

// A rule's result record is placed by result_fields; a miss places
// default_result. Bytes placed beyond the rule's value are zero.
TEST(WildcardMatchPacketTest, ResultFieldsPlaceRecordBytes) {
  ModuleHarness h;
  WildcardMatchArg arg;
  *arg.add_fields() = WmField(26, 4);
  auto *field = arg.add_result_fields();
  field->set_offset(40);
  field->set_value_offset(0);
  field->set_num_bytes(2);
  arg.set_default_result(std::string("\xee\xff", 2));
  Module *wm = h.Create("WildcardMatch", arg);
  ASSERT_NE(nullptr, wm);

  const auto rule = [&](const char *dip, uint64_t gate, const std::string &result) {
    bess::pb::WildcardMatchRuleKey key =
        WmKey({kFull4}, {IpOctets(dip)});
    bess::pb::WildcardMatchRuleValue value;
    value.set_priority(1);
    value.set_gate(gate);
    value.set_result(result);
    return h.UpsertResource(std::string(wm->name()) + "/rules", key, value);
  };
  EXPECT_EQ("", rule("10.0.0.1", 1, std::string("\x11\x22", 2)));
  EXPECT_EQ("", rule("10.0.0.2", 2, {}));
  EXPECT_EQ("", SetDefaultGate(h, wm, 3));

  auto rng = bess::testing::SeededRng(1);
  const Bytes hit = TcpPacket({.sip = "10.0.0.1", .dip = "12.34.56.78"}, rng);
  Bytes empty = TcpPacket({.sip = "10.0.0.2", .dip = "12.34.56.78"}, rng);
  empty[40] = 0xAA;  // must be overwritten with the zero-padded record
  empty[41] = 0xAA;
  const Bytes miss = TcpPacket({.sip = "10.0.0.3", .dip = "12.34.56.78"}, rng);

  auto out = h.Run(wm, 0, {hit, empty, miss}, {1, 2, 3});
  ASSERT_EQ(1u, out[1].size());
  EXPECT_EQ((Bytes{0x11, 0x22}), Bytes(out[1][0].begin() + 40, out[1][0].begin() + 42));
  ASSERT_EQ(1u, out[2].size());
  EXPECT_EQ((Bytes{0, 0}), Bytes(out[2][0].begin() + 40, out[2][0].begin() + 42));
  ASSERT_EQ(1u, out[3].size());
  EXPECT_EQ((Bytes{0xee, 0xff}), Bytes(out[3][0].begin() + 40, out[3][0].begin() + 42));
}

// A result record wider than the arena's limit is refused at Init.
TEST(WildcardMatchPacketTest, RejectsAnOversizedResultRecord) {
  ModuleHarness h;
  WildcardMatchArg arg;
  *arg.add_fields() = WmField(26, 4);
  auto *field = arg.add_result_fields();
  field->set_attr_name("oversized_wm_attr");
  field->set_value_offset(4096);
  field->set_num_bytes(1);
  EXPECT_TRUE(CreateRefused("WildcardMatch", arg));
}

// Index of `m`'s metadata attribute `name`, or all_attrs().size().
size_t AttrIndex(const Module *m, const std::string &name) {
  for (size_t i = 0; i < m->all_attrs().size(); i++) {
    if (m->all_attrs()[i].name == name) {
      return i;
    }
  }
  return m->all_attrs().size();
}

// A WildcardMatch on IPv4 src 10.0.0.1 (rule gate `gate`, default gate
// `default_gate`) whose result record carries `fields`: each a `size`-byte
// attribute at the next `size`-byte slice. The rule's record is `record`.
Module *ResultWm(ModuleHarness &h, const std::vector<std::string> &fields,
                 uint32_t size, uint64_t gate, uint64_t default_gate,
                 const std::string &record) {
  WildcardMatchArg arg;
  *arg.add_fields() = WmField(26, 4);
  for (size_t i = 0; i < fields.size(); i++) {
    auto *field = arg.add_result_fields();
    field->set_attr_name(fields[i]);
    field->set_value_offset(static_cast<uint32_t>(i) * size);
    field->set_num_bytes(size);
  }
  arg.set_default_gate(default_gate);
  Module *wm = h.Create("WildcardMatch", arg);
  if (wm == nullptr) {
    return nullptr;
  }
  bess::pb::WildcardMatchRuleValue value;
  value.set_priority(1);
  value.set_gate(gate);
  value.set_result(record);
  EXPECT_EQ("", h.UpsertResource(std::string(wm->name()) + "/rules",
                                 WmKey({kFull4}, {IpOctets("10.0.0.1")}), value));
  return wm;
}

// A metadata result destination with no reader gets kMetadataOffsetNoWrite:
// the field is compiled out of the plan and a hit still takes its rule's
// gate, as the metadata model lets a writer skip what nothing reads.
TEST(WildcardMatchPacketTest, UnreadResultFieldIsSkippedAndTheRuleGateApplies) {
  ModuleHarness h;
  Module *wm = ResultWm(h, {"wm_unread_attr"}, 4, 0, 1, std::string(4, '\x07'));
  ASSERT_NE(nullptr, wm);

  auto rng = bess::testing::SeededRng(1);
  const Bytes pkt = TcpPacket({.sip = "10.0.0.1", .dip = "12.34.56.78"}, rng);
  auto out = h.Run(wm, 0, {pkt}, {0, 1});
  EXPECT_EQ((GateCounts{{0, 1}}), Counts(out));
  EXPECT_EQ((std::vector<Bytes>{pkt}), out[0]) << "nothing is written";
  const size_t unread = AttrIndex(wm, "wm_unread_attr");
  ASSERT_LT(unread, wm->all_attrs().size());
  EXPECT_EQ(bess::metadata::kMetadataOffsetNoWrite, wm->attr_offset(unread));
}

// One field read downstream, one not: the read field is written (a downstream
// WildcardMatch matches on it) and the hit takes its rule's gate.
TEST(WildcardMatchPacketTest, ReadResultFieldIsWrittenBesideAnUnreadOne) {
  ModuleHarness h;
  Module *wm = ResultWm(h, {"wm_read_attr", "wm_unread_attr"}, 4, 1, 2,
                        std::string("\x00\x00\x00\x07\x00\x00\x00\x09", 8));
  ASSERT_NE(nullptr, wm);

  Field attr;
  attr.set_attr_name("wm_read_attr");
  attr.set_num_bytes(4);
  Module *next = CreateWm(h, {attr});
  ASSERT_NE(nullptr, next);
  EXPECT_EQ("", AddRule(h, next, WmRule(1, 0, {kFull4}, {{0, 0, 0, 7}})));
  EXPECT_EQ("", SetDefaultGate(h, next, 0));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(wm, 1, next, 0, true));

  auto rng = bess::testing::SeededRng(1);
  const Bytes pkt = TcpPacket({.sip = "10.0.0.1", .dip = "12.34.56.78"}, rng);
  for (int resume = 0; resume < 2; resume++) {
    auto out = h.RunPipeline(wm, 0, {pkt}, next, {0, 1});
    EXPECT_EQ((GateCounts{{1, 1}}), Counts(out)) << "resume " << resume;
  }
  const size_t read_id = AttrIndex(wm, "wm_read_attr");
  const size_t unread_id = AttrIndex(wm, "wm_unread_attr");
  ASSERT_LT(read_id, wm->all_attrs().size());
  ASSERT_LT(unread_id, wm->all_attrs().size());
  EXPECT_TRUE(bess::metadata::IsValidOffset(wm->attr_offset(read_id)));
  EXPECT_EQ(bess::metadata::kMetadataOffsetNoWrite, wm->attr_offset(unread_id));
}

// A result field with a reader but no metadata space (five 32-byte
// attributes in 128 bytes) has no destination: the first PreResume fails
// closed and the hit takes the default gate.
TEST(WildcardMatchPacketTest, ResultFieldWithoutSpaceFailsClosed) {
  ModuleHarness h;
  constexpr size_t kFields = 5;
  constexpr uint32_t kSize = bess::metadata::kMetadataAttrMaxSize;
  static_assert(kFields * kSize > bess::metadata::kMetadataTotalSize);
  std::vector<std::string> names;
  bess::pb::MetadataTestArg read;
  for (size_t i = 0; i < kFields; i++) {
    names.push_back("wm_nospace" + std::to_string(i));
    (*read.mutable_read())[names.back()] = kSize;
  }
  Module *wm = ResultWm(h, names, kSize, 1, 0,
                        std::string(kFields * kSize, '\x07'));
  ASSERT_NE(nullptr, wm);
  Module *reader = h.Create("MetadataTest", read);
  ASSERT_NE(nullptr, reader);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(wm, 1, reader, 0, true));

  auto rng = bess::testing::SeededRng(1);
  const Bytes pkt = TcpPacket({.sip = "10.0.0.1", .dip = "12.34.56.78"}, rng);
  auto out = h.RunPipeline(wm, 0, {pkt}, reader, {0});
  EXPECT_TRUE(Counts(out).empty())
      << "the hit's gate leads to the reader; the default gate is unwired";
  bool no_space = false;
  for (size_t i = 0; i < wm->all_attrs().size(); i++) {
    no_space = no_space ||
               wm->attr_offset(i) == bess::metadata::kMetadataOffsetNoSpace;
  }
  EXPECT_TRUE(no_space);
}

// Captures the messages glog emits at ERROR or above while it is alive.
class ErrorLogCapture : public google::LogSink {
 public:
  ErrorLogCapture() { google::AddLogSink(this); }
  ~ErrorLogCapture() override { google::RemoveLogSink(this); }
  void send(google::LogSeverity severity, const char *, const char *, int,
            const google::LogMessageTime &, const char *message,
            size_t message_len) override {
    if (severity >= google::GLOG_ERROR) {
      messages.emplace_back(message, message_len);
    }
  }
  std::vector<std::string> messages;
};

// The first PreResume resolves a deferred result plan: no destination moved
// then, nor on a later resume that leaves the offsets alone.
TEST(WildcardMatchPacketTest, DeferredResultPlanIsNotAMove) {
  ModuleHarness h;
  Module *wm = ResultWm(h, {"wm_read_attr"}, 4, 1, 0, std::string("\x00\x00\x00\x07", 4));
  ASSERT_NE(nullptr, wm);
  bess::pb::MetadataTestArg read;
  (*read.mutable_read())["wm_read_attr"] = 4;
  Module *reader = h.Create("MetadataTest", read);
  ASSERT_NE(nullptr, reader);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(wm, 1, reader, 0, true));

  auto rng = bess::testing::SeededRng(1);
  const Bytes pkt = TcpPacket({.sip = "10.0.0.1", .dip = "12.34.56.78"}, rng);
  ErrorLogCapture errors;
  for (int resume = 0; resume < 2; resume++) {
    EXPECT_EQ((GateCounts{{0, 1}}), Counts(h.RunPipeline(wm, 0, {pkt}, reader, {0})))
        << "resume " << resume;
  }
  for (const std::string &message : errors.messages) {
    EXPECT_EQ(std::string::npos, message.find("moved")) << message;
  }
}

// An explicitly empty default_result stays a no-op on a gate-only module; a
// non-empty one needs a result field.
TEST(WildcardMatchPacketTest, EmptyDefaultResultNeedsAFieldOnlyWhenNonEmpty) {
  ModuleHarness h;
  WildcardMatchArg arg;
  *arg.add_fields() = WmField(26, 4);
  arg.set_default_result("");
  EXPECT_NE(nullptr, h.Create("WildcardMatch", arg));

  WildcardMatchArg nonempty;
  *nonempty.add_fields() = WmField(26, 4);
  nonempty.set_default_result("\x01");
  EXPECT_TRUE(CreateRefused("WildcardMatch", nonempty));
}

// Masks need not be prefixes: arbitrary bits inside each byte are matched,
// the others are wildcards.
TEST(WildcardMatchPacketTest, NonPrefixMasksMatchOnlyTheMaskedBits) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, wm);
  EXPECT_EQ("", AddRule(h, wm, WmRule(1, 1, {{0x0f, 0xf0, 0x00, 0xff}}, {{0x0a, 0x30, 0x00, 0x44}})));
  EXPECT_EQ("", SetDefaultGate(h, wm, 2));

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
  EXPECT_EQ("", AddRule(h, wm,
                        WmRule(1, 1, {Octets(1, 0xff), Octets(3, 0xff), Octets(7, 0xff)},
                               {Octets(pkt.begin() + 26, pkt.begin() + 27),
                                Octets(pkt.begin() + 27, pkt.begin() + 30),
                                Octets(pkt.begin() + 30, pkt.begin() + 37)})));
  EXPECT_EQ("", SetDefaultGate(h, wm, 2));
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
  EXPECT_EQ("", AddRule(h, wm, WmRule(1, 1, {wild_mask}, {wild})));
  EXPECT_EQ("", AddRule(h, wm, WmRule(2, 9, {kFull4}, {exact})));
  EXPECT_EQ("", SetDefaultGate(h, wm, 3));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2, 3})));

  Module *tie = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, tie);
  EXPECT_EQ("", AddRule(h, tie, WmRule(1, 5, {wild_mask}, {wild})));
  EXPECT_EQ("", AddRule(h, tie, WmRule(2, 5, {kFull4}, {exact})));
  EXPECT_EQ("", SetDefaultGate(h, tie, 3));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(tie, 0, {pkt}, {0, 1, 2, 3})));

  Module *reversed = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, reversed);
  EXPECT_EQ("", AddRule(h, reversed, WmRule(2, 5, {kFull4}, {exact})));
  EXPECT_EQ("", AddRule(h, reversed, WmRule(1, 5, {wild_mask}, {wild})));
  EXPECT_EQ("", SetDefaultGate(h, reversed, 3));
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
  EXPECT_EQ("", AddRule(h, wm, WmRule(1, 1, {kFull4}, {addr})));
  EXPECT_EQ("", SetDefaultGate(h, wm, 2));
  std::mt19937 rng(5);
  const Bytes pkt = TcpPacket({.sip = "26.51.119.68", .dip = "10.0.0.1"}, rng);
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2})));

  EXPECT_EQ("", DeleteRule(h, wm, WmKey({kFull4}, {addr})));
  EXPECT_EQ((GateCounts{{2, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2})));

  for (uint8_t i = 0; i < 8; i++) {
    EXPECT_EQ("", AddRule(h, wm, WmRule(1, 1, {{0xff, 0, 0, i}}, {{0x1a, 0, 0, i}})));
  }
  EXPECT_EQ("", ClearRules(h, wm));
  for (uint8_t i = 0; i < 8; i++) {
    EXPECT_EQ("", AddRule(h, wm, WmRule(1, 1, {{0, 0xff, i, 0}}, {{0, 0x33, i, 0}})));
  }
  EXPECT_EQ((GateCounts{{1, 1}}), Counts(h.Run(wm, 0, {pkt}, {0, 1, 2})));
}

// A field reaching past the end of the packet is not read: the packet takes
// the default gate (an all-zero read would have matched the rule).
TEST(WildcardMatchPacketTest, ShortPacketTakesTheDefaultGate) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(30, 4)});
  ASSERT_NE(nullptr, wm);
  EXPECT_EQ("", AddRule(h, wm, WmRule(1, 1, {kFull4}, {{0, 0, 0, 0}})));
  EXPECT_EQ("", SetDefaultGate(h, wm, 2));
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
  EXPECT_EQ("", AddRule(h, wm, WmRule(1, 0, {{0xff, 0xff}}, {{0x88, 0x80}})));
  EXPECT_EQ("", AddRule(h, wm, WmRule(2, 0, {{0xff, 0xff}}, {{0x77, 0x70}})));
  EXPECT_EQ("", SetDefaultGate(h, wm, 0));

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

// The rules and the default gate read back as they were written, whatever
// order they were added in.
TEST(WildcardMatchPacketTest, RuntimeConfigReproducesTheModule) {
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
  EXPECT_EQ("", AddRule(h, wm, r2));
  EXPECT_EQ("", AddRule(h, wm, r1));
  EXPECT_EQ("", SetDefaultGate(h, wm, 3));

  WmConfig expect;
  expect.set_default_gate(3);
  *expect.add_rules() = WmRuleSpec(r1);
  *expect.add_rules() = WmRuleSpec(r2);
  SortRules(&expect);

  const WmConfig config = ReadConfig(h, wm);
  EXPECT_EQ(expect, config) << config.DebugString();
}

// D-129: the rules and the default gate as resources (the state the module
// holds; the legacy commands that edited them are gone with D-140).

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
  EXPECT_EQ("", AddRule(h, wm, WmRule(2, 7, {{0xff, 0, 0, 0}, zero4}, {{0x1b, 0, 0, 0}, zero4})));
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
  EXPECT_EQ("", AddRule(h, wm, WmRule(1, 1, {kFull4}, {IpOctets("26.51.119.68")})));
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
  EXPECT_EQ("", SetDefaultGate(h, wm, 3));
  const auto three = WmSettings(3);
  EXPECT_EQ("", h.ReadBackDiffers(settings, {{nullptr, &three}}));
  EXPECT_EQ((GateCounts{{3, 1}}), Counts(h.Run(wm, 0, {miss}, {0, 1, 2, 3})));

  // Gate 0 is the proto3 default: it reads back as an empty message.
  const auto zero = WmSettings(0);
  ASSERT_EQ("", h.SetResource(settings, zero));
  EXPECT_EQ((GateCounts{{0, 1}}), Counts(h.Run(wm, 0, {miss}, {0, 1, 2, 3})));
  EXPECT_EQ("", h.ReadBackDiffers(settings, {{nullptr, &zero}}));
}

// Every state-editing command is gone with the legacy service: InvokeAction
// has no such action (the resources are the way to change the same state).
TEST(WildcardMatchResourceTest, StateCommandsAreGone) {
  ModuleHarness h;
  Module *wm = CreateWm(h, {WmField(26, 4)});
  ASSERT_NE(nullptr, wm);
  const auto rule = WmRule(1, 1, {kFull4}, {IpOctets("26.51.119.68")});
  const bess::pb::EmptyArg empty;
  EXPECT_NE(0, h.Action(wm, "add", rule).error().code());
  EXPECT_NE(0, h.Action(wm, "delete", WmKey({kFull4}, {IpOctets("26.51.119.68")}))
                   .error()
                   .code());
  EXPECT_NE(0, h.Action(wm, "clear", empty).error().code());
  EXPECT_NE(0, h.Action(wm, "set_default_gate", WmDefault(1)).error().code());
  EXPECT_NE(0, h.Action(wm, "get_initial_arg", empty).error().code());
  EXPECT_NE(0, h.Action(wm, "get_runtime_config", empty).error().code());
  EXPECT_NE(0, h.Action(wm, "set_runtime_config", bess::pb::EmptyArg()).error().code());
  EXPECT_EQ("", h.ReadBackDiffers(wm->name() + "/rules", {})) << "the refused add did nothing";

  EXPECT_EQ("", AddRule(h, wm, rule));
  EXPECT_EQ(1u, h.ReadResource(wm->name() + "/rules").size());
}

}  // namespace
