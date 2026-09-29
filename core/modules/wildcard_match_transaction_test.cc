// SPDX-License-Identifier: BSD-3-Clause

// WildcardMatch as a transactional resource (Decision D-024): masked rules
// prepared invisibly -- a rule being prepared never outranks an existing
// match and alone reads as a miss -- committed in place, changed together
// with other modules' tables, and looked up through the module's packet path
// (ClassifyBatch) while transactions run.

#include <gtest/gtest.h>

#include <any>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "control/runtime_state.h"
#include "dataplane/transaction_engine.h"
#include "module.h"
#include "module_graph.h"
#include "modules/exact_match.h"
#include "modules/wildcard_match.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_domain.h"

namespace {

using bess::classifier::MaskedRuleResource;
using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;
using Outcome = TransactionEngine::Outcome;

TransactionEngine &Engine() { return bess::control::runtime().transactions(); }

// Both modules classify on 4 bytes at offset 26 and 2 bytes at offset 34.
template <typename M, typename Arg>
M *CreateModule(const char *cls, const std::string &name) {
  Arg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  f = arg.add_fields();
  f->set_offset(34);
  f->set_num_bytes(2);
  google::protobuf::Any packed;
  EXPECT_TRUE(packed.PackFrom(arg));
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(
      ModuleBuilder::all_module_builders().at(cls), name, packed, &perr);
  EXPECT_EQ(0, perr.code()) << perr.errmsg();
  return static_cast<M *>(m);
}

WildcardMatch *CreateWm(const std::string &name) {
  return CreateModule<WildcardMatch, bess::pb::WildcardMatchArg>(
      "WildcardMatch", name);
}

std::string Bytes(uint32_t a, uint16_t b) {
  std::string s(6, '\0');
  std::memcpy(s.data(), &a, 4);
  std::memcpy(s.data() + 4, &b, 2);
  return s;
}

// A rule matching (a & mask_a, b & mask_b).
Op Add(const std::string &module, uint32_t mask_a, uint16_t mask_b,
       uint32_t a, uint16_t b, int64_t priority, uint16_t gate) {
  return Op::Upsert(module + "/rules",
                    Bytes(mask_a, mask_b) + Bytes(a & mask_a, b & mask_b),
                    std::any(MaskedRuleResource::Value{priority, gate}));
}

Op Remove(const std::string &module, uint32_t mask_a, uint16_t mask_b,
          uint32_t a, uint16_t b) {
  return Op::Erase(module + "/rules",
                   Bytes(mask_a, mask_b) + Bytes(a & mask_a, b & mask_b));
}

TransactionEngine::Result Apply(std::vector<Op> ops) {
  return Engine().Apply(ops);
}

size_t RuleCount(WildcardMatch *wm) {
  bess::pb::WildcardMatchConfig config;
  CommandResponse r = wm->GetRuntimeConfig(bess::pb::EmptyArg());
  EXPECT_FALSE(r.has_error());
  EXPECT_TRUE(r.data().UnpackTo(&config));
  return static_cast<size_t>(config.rules_size());
}

class Packets {
 public:
  explicit Packets(size_t n) : pool_(n + 64) {
    for (size_t i = 0; i < n; i++) {
      bess::PacketHandle pkt = pool_.Alloc(60);
      EXPECT_NE(pkt, nullptr);
      std::memset(bess::PacketRef(pkt).head_data<uint8_t *>(), 0, 60);
      handles_.push_back(pkt);
    }
  }
  ~Packets() {
    for (bess::PacketHandle pkt : handles_) {
      bess::PacketFree(pkt);
    }
  }
  void Set(size_t i, uint32_t a, uint16_t b) {
    uint8_t *p = bess::PacketRef(handles_[i]).head_data<uint8_t *>();
    std::memcpy(p + 26, &a, 4);
    std::memcpy(p + 34, &b, 2);
  }
  template <typename M>
  std::vector<gate_idx_t> Classify(const M *m, size_t first, size_t n) const {
    bess::PacketBatch batch;
    batch.clear();
    for (size_t i = 0; i < n; i++) {
      batch.add(bess::PacketRef(handles_[first + i]));
    }
    std::vector<gate_idx_t> gates(n);
    m->ClassifyBatch(&batch, gates.data());
    return gates;
  }

 private:
  bess::PlainPacketPool pool_;
  std::vector<bess::PacketHandle> handles_;
};

constexpr uint32_t kAll = 0xffffffff;
constexpr uint16_t kPort = 0xffff;

class WildcardMatchTransactionTest : public ::testing::Test {
 protected:
  void TearDown() override {
    ModuleGraph::DestroyAllModules();
    while (Engine().ReclaimRetired() != 0) {
    }
  }
};

TEST_F(WildcardMatchTransactionTest, RulesAreAResourceWhileTheModuleExists) {
  WildcardMatch *wm = CreateWm("wm");
  ASSERT_EQ(Apply({Add("wm", kAll, 0, 10, 0, 1, 5)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(RuleCount(wm), 1u);
  Packets packets(2);
  packets.Set(0, 10, 99);
  packets.Set(1, 11, 99);
  EXPECT_EQ(packets.Classify(wm, 0, 2),
            (std::vector<gate_idx_t>{5, DROP_GATE}));
  // A value with bits outside its mask, a wrong key size, an invalid gate.
  auto r = Apply({Op::Upsert("wm/rules", Bytes(0xff, 0) + Bytes(0x100, 0),
                             std::any(MaskedRuleResource::Value{1, 1}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("outside its mask"), std::string::npos);
  r = Apply({Op::Upsert("wm/rules", Bytes(0, 0),
                        std::any(MaskedRuleResource::Value{1, 1}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  r = Apply({Add("wm", kAll, 0, 12, 0, 1, MAX_GATES + 1)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("invalid gate"), std::string::npos);

  // Destroyed with its rules and a freed id still in the removal cascade.
  ASSERT_EQ(Apply({Add("wm", kAll, 0, 10, 0, 2, 6)}).outcome,
            Outcome::kApplied);
  ModuleGraph::DestroyModule(wm);
  EXPECT_EQ(Apply({Add("wm", kAll, 0, 10, 0, 1, 5)}).outcome,
            Outcome::kRejected);
  WildcardMatch *again = CreateWm("wm");
  EXPECT_EQ(RuleCount(again), 0u);
}

// The invariant a pending rule must keep: inside the publication window
// (every rule prepared, none committed), no packet's answer has changed.
WildcardMatch *g_wm = nullptr;
Packets *g_packets = nullptr;
std::vector<gate_idx_t> g_seen;

void ClassifyInWindow(bool entering) {
  if (entering) {
    g_seen = g_packets->Classify(g_wm, 0, 4);
  }
}

TEST_F(WildcardMatchTransactionTest, RulesBeingPreparedChangeNoAnswer) {
  WildcardMatch *wm = CreateWm("wm");
  // A wide, low-priority rule (source address only).
  ASSERT_EQ(Apply({Add("wm", kAll, 0, 10, 0, 1, 3)}).outcome,
            Outcome::kApplied);
  Packets packets(4);
  packets.Set(0, 10, 80);  // the wide rule; a narrower, higher one coming
  packets.Set(1, 20, 80);  // nothing; a rule in a brand-new mask coming
  packets.Set(2, 10, 81);  // the wide rule; stays so
  packets.Set(3, 30, 7);   // nothing; a rule in the wide rule's mask coming
  g_wm = wm;
  g_packets = &packets;
  bess::dataplane::internal::g_publish_window_hook = ClassifyInWindow;
  const auto r = Apply({Add("wm", kAll, kPort, 10, 80, 10, 5),
                        Add("wm", 0, kPort, 0, 80, 7, 6),
                        Add("wm", kAll, 0, 30, 0, 2, 8),
                        Add("wm", kAll, 0, 10, 0, 1, 4)});  // re-gate
  bess::dataplane::internal::g_publish_window_hook = nullptr;
  ASSERT_EQ(r.outcome, Outcome::kApplied);
  EXPECT_EQ(g_seen,
            (std::vector<gate_idx_t>{3, DROP_GATE, 3, DROP_GATE}));
  EXPECT_EQ(packets.Classify(wm, 0, 4),
            (std::vector<gate_idx_t>{5, 6, 4, 8}));
}

TEST_F(WildcardMatchTransactionTest, RejectionLeavesNothingBehind) {
  WildcardMatch *wm = CreateWm("wm");
  ASSERT_EQ(Apply({Add("wm", kAll, 0, 10, 0, 1, 3)}).outcome,
            Outcome::kApplied);
  Packets packets(3);
  packets.Set(0, 10, 80);
  packets.Set(1, 20, 80);
  packets.Set(2, 30, 1);
  const auto before = packets.Classify(wm, 0, 3);
  // New masks, a replacement and a new rule, then an invalid gate.
  auto r = Apply({Add("wm", kAll, kPort, 10, 80, 10, 5),
                  Add("wm", 0, kPort, 0, 80, 7, 6),
                  Add("wm", kAll, 0, 10, 0, 9, 9),
                  Add("wm", kAll, 0, 30, 0, 2, MAX_GATES + 3)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(packets.Classify(wm, 0, 3), before);
  EXPECT_EQ(RuleCount(wm), 1u);
  // The masks it created are dropped by the next change, command or
  // transaction.
  ASSERT_EQ(Apply({Add("wm", kAll, 0, 31, 0, 1, 2)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(RuleCount(wm), 2u);
  EXPECT_EQ(packets.Classify(wm, 0, 3), before);
}

// One transaction across two module kinds: an exact rule and a wildcard rule
// both apply, or neither.
TEST_F(WildcardMatchTransactionTest, OneTransactionAcrossModuleKinds) {
  WildcardMatch *wm = CreateWm("wm");
  auto *em = CreateModule<ExactMatch, bess::pb::ExactMatchArg>("ExactMatch",
                                                              "em");
  Packets packets(1);
  packets.Set(0, 40, 4);
  auto exact = [](uint64_t gate) {
    return Op::Upsert("em/rules", Bytes(40, 4), std::any(gate));
  };
  auto r = Apply({exact(7), Add("wm", kAll, kPort, 40, 4, 1, MAX_GATES + 1)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(packets.Classify(em, 0, 1)[0], DROP_GATE);
  EXPECT_EQ(packets.Classify(wm, 0, 1)[0], DROP_GATE);
  ASSERT_EQ(Apply({exact(7), Add("wm", kAll, kPort, 40, 4, 1, 8)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(packets.Classify(em, 0, 1)[0], 7);
  EXPECT_EQ(packets.Classify(wm, 0, 1)[0], 8);
}

// Commands and transactions write the same rules; a tuple table grows inside
// a transaction; freed rule ids come back once the cascade runs.
TEST_F(WildcardMatchTransactionTest, CommandsGrowthAndIdReuse) {
  WildcardMatch *wm = CreateWm("wm");
  bess::pb::WildcardMatchCommandAddArg add;
  add.set_gate(3);
  add.set_priority(1);
  const std::string v = Bytes(50, 0), m = Bytes(kAll, 0);
  add.add_values()->set_value_bin(v.substr(0, 4));
  add.add_values()->set_value_bin(v.substr(4, 2));
  add.add_masks()->set_value_bin(m.substr(0, 4));
  add.add_masks()->set_value_bin(m.substr(4, 2));
  ASSERT_FALSE(wm->CommandAdd(add).has_error());
  ASSERT_EQ(Apply({Remove("wm", kAll, 0, 50, 0)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(RuleCount(wm), 0u);

  // 3000 rules in one mask, from an empty tuple: several growths.
  std::vector<Op> ops;
  for (uint32_t a = 0; a < 3000; a++) {
    ops.push_back(Add("wm", kAll, kPort, a, 1, 1, 1 + a % 60));
  }
  ASSERT_EQ(Engine().Apply(ops).outcome, Outcome::kApplied);
  EXPECT_EQ(RuleCount(wm), 3000u);
  Packets packets(32);
  for (uint32_t first = 0; first < 3000; first += 32) {
    for (uint32_t i = 0; i < 32; i++) {
      packets.Set(i, first + i, 1);
    }
    const auto gates = packets.Classify(wm, 0, 32);
    for (uint32_t i = 0; i < 32 && first + i < 3000; i++) {
      ASSERT_EQ(gates[i], 1 + (first + i) % 60);
    }
  }
  // Re-gating one rule 5000 times through the engine: each commit frees an
  // id the cascade returns (table-level accounting: ConcurrentMaskedTable
  // test PrepareCommitCancelRecycle); the rule keeps answering correctly.
  for (int round = 0; round < 5000; round++) {
    const auto gate = static_cast<uint16_t>(1 + round % 50);
    ASSERT_EQ(Apply({Add("wm", kAll, kPort, 7, 1, 1, gate)}).outcome,
              Outcome::kApplied);
    if (round % 1000 == 999) {
      packets.Set(0, 7, 1);
      ASSERT_EQ(packets.Classify(wm, 0, 1)[0], gate);
    }
  }
  EXPECT_EQ(RuleCount(wm), 3000u);
}

// The adapter returns every id a commit frees, through the engine's removal
// cascade: re-gating a rule thousands of times leaves the ids in use flat.
TEST_F(WildcardMatchTransactionTest, FreedIdsReturnThroughTheCascade) {
  std::shared_ptr<bess::classifier::ConcurrentMaskedTable> table =
      *bess::classifier::ConcurrentMaskedTable::Create(
          6, 8, bess::control::runtime().rcu());
  MaskedRuleResource res("masked",
                         MaskedRuleResource::Hooks{.table = [&] { return table; }});
  TransactionEngine engine(bess::control::runtime().rcu());
  ASSERT_TRUE(engine.Register(&res));
  auto regate = [&](uint16_t gate) {
    return engine.Apply(std::vector<Op>{Op::Upsert(
        "masked", Bytes(kAll, 0) + Bytes(9, 0),
        std::any(MaskedRuleResource::Value{1, gate}))});
  };
  ASSERT_EQ(regate(1).outcome, Outcome::kApplied);
  const size_t steady = table->ids_in_use();
  for (int i = 0; i < 3000; i++) {
    ASSERT_EQ(regate(static_cast<uint16_t>(2 + i % 50)).outcome,
              Outcome::kApplied);
  }
  while (engine.ReclaimRetired() != 0) {
  }
  EXPECT_LE(table->ids_in_use(), steady + 1);
  ASSERT_EQ(engine.Apply(std::vector<Op>{Op::Erase(
                             "masked", Bytes(kAll, 0) + Bytes(9, 0))})
                .outcome,
            Outcome::kApplied);
  while (engine.ReclaimRetired() != 0) {
  }
  EXPECT_EQ(table->tuple_count(), 0u);  // the emptied mask went too
  ASSERT_TRUE(engine.Unregister("masked"));
}

// A registered reader classifies through the module while transactions add
// rules (new masks included) and remove them; a packet only ever gets its
// own rule's gate or the default.
TEST_F(WildcardMatchTransactionTest, LookupsWhileTransactionsRun) {
  WildcardMatch *wm = CreateWm("wm");
  constexpr uint32_t kSessions = 1024;
  constexpr uint32_t kLive = 200;
  constexpr uint32_t kSteps = 8000;
  // Session s: exact address s + 1 and port s % 7 under one of four masks
  // (port exact or not, address exact), gate 1 + s % 60, priority s % 3.
  auto mask_b = [](uint32_t s) -> uint16_t { return s % 2 ? kPort : 0; };
  auto establish = [&](uint32_t s) {
    return std::vector<Op>{Add("wm", kAll, mask_b(s), s + 1,
                               static_cast<uint16_t>(s % 7),
                               static_cast<int64_t>(s % 3),
                               static_cast<uint16_t>(1 + s % 60))};
  };
  auto release = [&](uint32_t s) {
    return std::vector<Op>{
        Remove("wm", kAll, mask_b(s), s + 1, static_cast<uint16_t>(s % 7))};
  };
  bess::rcu::RcuDomain &domain = bess::control::runtime().rcu();
  constexpr bess::rcu::ReaderId kReader = 26;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> hits{0}, wrong{0};
  Packets packets(32);
  std::thread reader([&] {
    domain.Online(kReader);
    std::mt19937 rng(11);
    uint32_t session[32];
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 32; i++) {
        session[i] = rng() % kSessions;
        packets.Set(static_cast<size_t>(i), session[i] + 1,
                    static_cast<uint16_t>(session[i] % 7));
      }
      const auto gates = packets.Classify(wm, 0, 32);
      for (int i = 0; i < 32; i++) {
        if (gates[i] == DROP_GATE) {
          continue;
        }
        (gates[i] == 1 + session[i] % 60 ? hits : wrong)++;
      }
      domain.Quiescent(kReader);
    }
    domain.Offline(kReader);
  });
  for (uint32_t step = 0; step < kSteps; step++) {
    ASSERT_EQ(Apply(establish(step % kSessions)).outcome, Outcome::kApplied);
    if (step >= kLive) {
      ASSERT_EQ(Apply(release((step - kLive) % kSessions)).outcome,
                Outcome::kApplied);
    }
  }
  stop = true;
  reader.join();
  domain.Unregister(kReader);
  EXPECT_EQ(wrong.load(), 0u);
  EXPECT_GT(hits.load(), 0u);
  EXPECT_EQ(RuleCount(wm), kLive);
}

}  // namespace
