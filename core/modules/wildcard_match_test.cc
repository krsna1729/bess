// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// K3.6 module-level test for the migrated WildcardMatch. The packet path is
// exercised end to end by bessctl/module_tests/wildcard_match.py against a live
// daemon; this test covers what that harness cannot reach directly: generation
// construction, rule canonicalization, the module's own limits, introspection
// byte parity, and the safety cases the legacy implementation had no defined
// behavior for.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "module.h"
#include "module_graph.h"
#include "modules/wildcard_match.h"
#include "pb/module_msg.pb.h"

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
  // The ninth distinct mask is rejected.
  EXPECT_NE(0, Run(m, "add", Pack(Rule(0, 0xabcd, 9, kGateA))).error().code());

  // clear() drops the rules *and* their masks, so capacity is genuinely free.
  bess::pb::EmptyArg empty;
  ASSERT_EQ(0, Run(m, "clear", Pack(empty)).error().code());
  for (int i = 0; i < 8; i++) {
    const uint64_t mask = 0x00ffu | (static_cast<uint64_t>(i) << 8);
    ASSERT_EQ(0, Run(m, "add", Pack(Rule(0, mask, i, kGateB))).error().code());
  }
  EXPECT_NE(0, Run(m, "add", Pack(Rule(0, 0x1111, 9, kGateB))).error().code());
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

}  // namespace
