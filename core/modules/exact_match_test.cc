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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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


// G1.2a mode C: ExactMatch's command path against the in-place concurrent
// table -- add/delete/clear semantics, growth past the initial capacity, and
// restore -- through the module's own API. Packet classification is covered
// by bessctl/module_tests/exactmatch.py against a live daemon and the table's
// concurrency by classifier/concurrent_exact_test.cc.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include "module.h"
#include "module_graph.h"
#include "control/runtime_state.h"
#include "modules/exact_match.h"
#include "pb/module_msg.pb.h"

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
  bess::rcu::RcuDomain &domain = bess::control::runtime().rcu();
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

}  // namespace
