// SPDX-License-Identifier: BSD-3-Clause

// ExactMatch as a transactional resource (Decision D-022): rules of several
// modules changed in one transaction, next to the module's own commands, and
// looked up through the module's packet path (ClassifyBatch runs the code
// ProcessBatch runs) while transactions are in progress.

#include <gtest/gtest.h>

#include <any>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "runtime/runtime_state.h"
#include "dataplane/transaction_engine.h"
#include "module.h"
#include "module_graph.h"
#include "modules/exact_match.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_domain.h"

namespace {

using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;
using Outcome = TransactionEngine::Outcome;

TransactionEngine &Engine() { return bess::runtime::runtime().transactions(); }

// Fields: 4 bytes at offset 26, 2 bytes at offset 34. The packed rule key is
// the fields' bytes in order: 6 bytes.
ExactMatch *CreateEm(const std::string &name) {
  bess::pb::ExactMatchArg arg;
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
      ModuleBuilder::all_module_builders().at("ExactMatch"), name, packed,
      &perr);
  EXPECT_EQ(0, perr.code()) << perr.errmsg();
  return static_cast<ExactMatch *>(m);
}

std::string Key(uint32_t a, uint16_t b) {
  std::string key(6, '\0');
  std::memcpy(key.data(), &a, sizeof(a));
  std::memcpy(key.data() + 4, &b, sizeof(b));
  return key;
}

Op Add(const std::string &module, uint32_t a, uint16_t b, uint64_t gate) {
  return Op::Upsert(module + "/rules", Key(a, b), std::any(gate));
}

Op Remove(const std::string &module, uint32_t a, uint16_t b) {
  return Op::Erase(module + "/rules", Key(a, b));
}

TransactionEngine::Result Apply(std::vector<Op> ops) {
  return Engine().Apply(ops);
}

size_t RuleCount(ExactMatch *em) {
  bess::pb::ExactMatchConfig config;
  CommandResponse r = em->GetRuntimeConfig(bess::pb::EmptyArg());
  EXPECT_FALSE(r.has_error());
  EXPECT_TRUE(r.data().UnpackTo(&config));
  return static_cast<size_t>(config.rules_size());
}

// Packets whose keys the test sets, batched 32 at a time.
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
    std::memcpy(p + 26, &a, sizeof(a));
    std::memcpy(p + 34, &b, sizeof(b));
  }

  // The gates `em` sends packets [first, first + n) to (n <= 32).
  std::vector<gate_idx_t> Classify(const ExactMatch *em, size_t first,
                                   size_t n) const {
    bess::PacketBatch batch;
    batch.clear();
    for (size_t i = 0; i < n; i++) {
      batch.add(bess::PacketRef(handles_[first + i]));
    }
    std::vector<gate_idx_t> gates(n);
    em->ClassifyBatch(&batch, gates.data());
    return gates;
  }

 private:
  bess::PlainPacketPool pool_;
  std::vector<bess::PacketHandle> handles_;
};

// The gate for key (a, b) in these tests.
gate_idx_t GateOf(uint32_t a, uint16_t b) {
  return static_cast<gate_idx_t>(1 + (a * 7 + b) % 63);
}

class ExactMatchTransactionTest : public ::testing::Test {
 protected:
  void TearDown() override { ModuleGraph::DestroyAllModules(); }
};

TEST_F(ExactMatchTransactionTest, RulesAreAResourceWhileTheModuleExists) {
  ExactMatch *em = CreateEm("em");
  auto r = Apply({Add("em", 1, 2, 5)});
  ASSERT_EQ(r.outcome, Outcome::kApplied);
  EXPECT_EQ(RuleCount(em), 1u);
  Packets packets(2);
  packets.Set(0, 1, 2);
  packets.Set(1, 1, 3);
  EXPECT_EQ(packets.Classify(em, 0, 2),
            (std::vector<gate_idx_t>{5, DROP_GATE}));

  // Wrong key width and invalid gates are refused by the resource.
  r = Engine().Apply(std::vector<Op>{
      Op::Upsert("em/rules", std::string(4, 'x'), std::any(uint64_t{1}))});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("6"), std::string::npos);
  r = Apply({Add("em", 9, 9, MAX_GATES + 1)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_NE(r.ops[0].error.find("invalid gate"), std::string::npos);
  EXPECT_EQ(Apply({Add("em", 9, 9, DROP_GATE)}).outcome, Outcome::kApplied);

  // Destroyed with its rules: the resource goes with the module, and a new
  // module may take the name.
  ModuleGraph::DestroyModule(em);
  r = Apply({Add("em", 1, 2, 5)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  ExactMatch *again = CreateEm("em");
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(RuleCount(again), 0u);
  EXPECT_EQ(Apply({Add("em", 1, 2, 5)}).outcome, Outcome::kApplied);
}

TEST_F(ExactMatchTransactionTest, OneTransactionChangesSeveralModules) {
  ExactMatch *ul = CreateEm("ul");
  ExactMatch *dl = CreateEm("dl");
  Packets packets(2);
  packets.Set(0, 10, 1);
  packets.Set(1, 10, 2);

  ASSERT_EQ(Apply({Add("ul", 10, 1, 3), Add("dl", 10, 2, 4)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(packets.Classify(ul, 0, 1)[0], 3);
  EXPECT_EQ(packets.Classify(dl, 1, 1)[0], 4);

  // One bad operation: nothing of the transaction happens in either module.
  auto r = Apply({Remove("ul", 10, 1), Add("dl", 10, 2, 9),
                  Add("dl", 11, 2, MAX_GATES + 7)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[2].status, TransactionEngine::OpStatus::kFailed);
  EXPECT_EQ(packets.Classify(ul, 0, 1)[0], 3);
  EXPECT_EQ(packets.Classify(dl, 1, 1)[0], 4);
  EXPECT_EQ(RuleCount(ul), 1u);
  EXPECT_EQ(RuleCount(dl), 1u);

  ASSERT_EQ(Apply({Remove("ul", 10, 1), Remove("dl", 10, 2)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(packets.Classify(ul, 0, 1)[0], DROP_GATE);
  EXPECT_EQ(packets.Classify(dl, 1, 1)[0], DROP_GATE);
}

TEST_F(ExactMatchTransactionTest, CommandsAndTransactionsShareTheRules) {
  ExactMatch *em = CreateEm("em");
  bess::pb::ExactMatchCommandAddArg add;
  add.set_gate(5);
  add.add_fields()->set_value_int(1);
  add.add_fields()->set_value_int(1);
  ASSERT_FALSE(em->CommandAdd(add).has_error());

  // A transaction sees the command's rule, and the command the
  // transaction's.
  ASSERT_EQ(Apply({Add("em", 1, 1, 6), Add("em", 2, 2, 7)}).outcome,
            Outcome::kApplied);
  EXPECT_EQ(RuleCount(em), 2u);
  bess::pb::ExactMatchCommandDeleteArg del;
  del.add_fields()->set_value_int(2);
  del.add_fields()->set_value_int(2);
  ASSERT_FALSE(em->CommandDelete(del).has_error());
  auto r = Apply({Remove("em", 2, 2)});
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops[0].error, "not found");
  ASSERT_EQ(Apply({Remove("em", 1, 1)}).outcome, Outcome::kApplied);
  EXPECT_EQ(RuleCount(em), 0u);

  // clear empties the resource too.
  ASSERT_EQ(Apply({Add("em", 3, 3, 1)}).outcome, Outcome::kApplied);
  ASSERT_FALSE(em->CommandClear(bess::pb::EmptyArg()).has_error());
  EXPECT_EQ(Apply({Remove("em", 3, 3)}).outcome, Outcome::kRejected);
}

// The table grows inside a transaction (a larger copy, published as a new
// generation): keys already placed by the transaction move with it, and a
// rejection after growth leaves nothing behind.
TEST_F(ExactMatchTransactionTest, GrowsWhileATransactionIsPrepared) {
  ExactMatch *em = CreateEm("em");
  constexpr uint32_t kRules = 5000;  // several growths from an empty module
  auto adds = [](uint32_t first, uint32_t n) {
    std::vector<Op> ops;
    for (uint32_t i = first; i < first + n; i++) {
      ops.push_back(Add("em", i, 1, GateOf(i, 1)));
    }
    return ops;
  };
  Packets packets(32);
  auto expect_rules = [&](uint32_t n) {
    EXPECT_EQ(RuleCount(em), n);
    for (uint32_t first = 0; first < 2 * kRules; first += 32) {
      for (size_t i = 0; i < 32; i++) {
        packets.Set(i, first + static_cast<uint32_t>(i), 1);
      }
      const auto gates = packets.Classify(em, 0, 32);
      for (size_t i = 0; i < 32; i++) {
        const uint32_t a = first + static_cast<uint32_t>(i);
        ASSERT_EQ(gates[i], a < n ? GateOf(a, 1) : DROP_GATE) << a;
      }
    }
  };

  // Applied: keys placed before each growth are published into the table
  // that replaced the one they were placed in.
  ASSERT_EQ(Engine().Apply(adds(0, kRules)).outcome, Outcome::kApplied);
  expect_rules(kRules);

  // Rejected at the last operation, after growing again: every key it
  // placed is gone, the committed ones are untouched.
  std::vector<Op> ops = adds(kRules, kRules);
  ops.push_back(Add("em", 2 * kRules, 1, MAX_GATES + 1));
  auto r = Engine().Apply(ops);
  ASSERT_EQ(r.outcome, Outcome::kRejected);
  EXPECT_EQ(r.ops.back().status, TransactionEngine::OpStatus::kFailed);
  expect_rules(kRules);
}

// Keys a transaction is preparing are in the table with the placeholder
// value; the packet path must treat them as misses.
ExactMatch *g_watched = nullptr;
Packets *g_watch_packets = nullptr;
std::vector<gate_idx_t> g_seen_in_window;

void ClassifyInWindow(bool entering) {
  if (entering) {
    g_seen_in_window = g_watch_packets->Classify(g_watched, 0, 2);
  }
}

TEST_F(ExactMatchTransactionTest, KeysBeingPreparedAreMissesForPackets) {
  ExactMatch *em = CreateEm("em");
  ASSERT_EQ(Apply({Add("em", 1, 1, 4)}).outcome, Outcome::kApplied);
  Packets packets(2);
  packets.Set(0, 1, 1);  // committed
  packets.Set(1, 2, 2);  // placed by the transaction below, not yet published
  g_watched = em;
  g_watch_packets = &packets;
  bess::dataplane::internal::g_publish_window_hook = ClassifyInWindow;
  const auto r = Apply({Add("em", 2, 2, 5), Add("em", 1, 1, 6)});
  bess::dataplane::internal::g_publish_window_hook = nullptr;
  ASSERT_EQ(r.outcome, Outcome::kApplied);
  // Just before publication: the new key is present but pending, the
  // updated one still has its old gate.
  EXPECT_EQ(g_seen_in_window, (std::vector<gate_idx_t>{4, DROP_GATE}));
  EXPECT_EQ(packets.Classify(em, 0, 2), (std::vector<gate_idx_t>{6, 5}));
}

// Real lookups through two modules on a registered reader thread while one
// transaction per session adds, and another removes, a rule in each module.
// The tables grow several times under the reader. A packet only ever sees
// its rule's gate or a miss -- never a placeholder, a freed table, or another
// key's gate.
TEST_F(ExactMatchTransactionTest, LookupsWhileTransactionsRun) {
  ExactMatch *ul = CreateEm("ul");
  ExactMatch *dl = CreateEm("dl");
  constexpr uint32_t kSessions = 4096;
  constexpr uint32_t kLive = 1500;
  constexpr uint32_t kSteps = 20000;

  bess::rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  constexpr bess::rcu::ReaderId kReader = 21;
  ASSERT_TRUE(domain.Register(kReader).has_value());
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> hits{0}, misses{0}, wrong{0};
  Packets packets(64);
  std::thread reader([&] {
    domain.Online(kReader);
    std::mt19937 rng(7);
    std::vector<uint32_t> session(32);
    while (!stop.load(std::memory_order_relaxed)) {
      for (size_t i = 0; i < 32; i++) {
        session[i] = rng() % kSessions;
        packets.Set(i, session[i], 1);
        packets.Set(32 + i, session[i], 2);
      }
      const auto up = packets.Classify(ul, 0, 32);
      const auto down = packets.Classify(dl, 32, 32);
      for (size_t i = 0; i < 32; i++) {
        for (const auto &[gate, b] :
             {std::pair{up[i], uint16_t{1}}, std::pair{down[i], uint16_t{2}}}) {
          if (gate == DROP_GATE) {
            misses++;
          } else if (gate == GateOf(session[i], b)) {
            hits++;
          } else {
            wrong++;
          }
        }
      }
      domain.Quiescent(kReader);
    }
    domain.Offline(kReader);
  });

  auto establish = [](uint32_t s) {
    return std::vector<Op>{Add("ul", s, 1, GateOf(s, 1)),
                           Add("dl", s, 2, GateOf(s, 2))};
  };
  auto release = [](uint32_t s) {
    return std::vector<Op>{Remove("ul", s, 1), Remove("dl", s, 2)};
  };
  uint64_t busy = 0;
  for (uint32_t step = 0; step < kSteps; step++) {
    while (Apply(establish(step % kSessions)).outcome == Outcome::kBusy) {
      busy++;
    }
    if (step >= kLive) {
      while (Apply(release((step - kLive) % kSessions)).outcome ==
             Outcome::kBusy) {
        busy++;
      }
    }
    bess::runtime::runtime().rcu().ReclaimReady();
  }
  stop = true;
  reader.join();
  domain.Unregister(kReader);

  EXPECT_EQ(wrong.load(), 0u);
  EXPECT_GT(hits.load(), 0u);
  EXPECT_GT(misses.load(), 0u);
  EXPECT_EQ(RuleCount(ul), kLive);
  EXPECT_EQ(RuleCount(dl), kLive);
  // The live window is exactly the last kLive sessions.
  for (uint32_t k = 0; k < 64; k++) {
    const uint32_t s = (kSteps - 1 - k * 47) % kSessions;
    const bool live = (kSteps - 1 - k * 47) >= kSteps - kLive;
    packets.Set(0, s, 1);
    EXPECT_EQ(packets.Classify(ul, 0, 1)[0], live ? GateOf(s, 1) : DROP_GATE);
  }
  std::printf("[lookups] hits %llu misses %llu, busy retries %llu\n",
              static_cast<unsigned long long>(hits.load()),
              static_cast<unsigned long long>(misses.load()),
              static_cast<unsigned long long>(busy));
}

}  // namespace
