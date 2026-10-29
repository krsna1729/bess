// SPDX-License-Identifier: BSD-3-Clause

// A masked rule whose new tuple table DPDK cannot allocate is refused for
// memory (D-198: RESOURCE_EXHAUSTED at the control boundary), not rejected as
// "rule table is full". Its own binary: it exhausts the process's DPDK heap
// and leaves it so, as freeing the filler would zero (and so commit) every
// page of the 512 MB --no-huge heap.

#include <gtest/gtest.h>

#include <rte_eal.h>
#include <rte_malloc.h>

#include <any>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "classifier/concurrent_masked.h"
#include "classifier/masked_rule_resource.h"
#include "dataplane/transaction_engine.h"
#include "runtime/runtime_state.h"
#include "utils/dpdk_memory.h"

namespace bess::classifier {
namespace {

using dataplane::Op;
using dataplane::TransactionEngine;
using Outcome = TransactionEngine::Outcome;

constexpr size_t kKeyLen = 8;

// A rule for `value` in the first key byte under `mask` there.
Op Upsert(uint8_t mask, uint8_t value) {
  std::string key(2 * kKeyLen, '\0');
  key[0] = static_cast<char>(mask);
  key[kKeyLen] = static_cast<char>(value);
  return Op::Upsert("masked", key,
                    std::any(MaskedRuleResource::Value{1, 1, {}}));
}

TEST(MaskedRuleMemoryTest, ATupleTableDpdkCannotAllocateIsRefusedNotFull) {
  utils::MemoryNode();  // DPDK up
  if (rte_eal_has_hugepages()) {
    GTEST_SKIP() << "fills DPDK's fixed --no-huge heap (BESSD_MEM_BACKING)";
  }
  rcu::RcuDomain &domain = runtime::runtime().rcu();
  auto created = ConcurrentMaskedTable::Create(kKeyLen, 8, domain);
  ASSERT_TRUE(created.has_value()) << created.error();
  std::shared_ptr<ConcurrentMaskedTable> table = std::move(*created);
  MaskedRuleResource resource(
      "masked", MaskedRuleResource::Hooks{.table = [&] { return table; }});
  TransactionEngine engine(domain);
  ASSERT_TRUE(engine.Register(&resource).has_value());
  // A first mask's table and the rule arena, while there is memory.
  ASSERT_EQ(engine.Apply(std::vector<Op>{Upsert(0xff, 1)}).outcome,
            Outcome::kApplied);

  // DPDK's heap full but for `room`: enough for its small bookkeeping
  // (tailq entries, an RCU config), not for a ring. A new mask's rte_hash
  // then goes to the fallback heap, and its RCU defer queue, which DPDK
  // allocates on SOCKET_ID_ANY (never the fallback heap), fails.
  void *room = rte_malloc(nullptr, 2048, 0);
  void *spare = rte_malloc(nullptr, size_t{4} << 20, 0);
  ASSERT_NE(room, nullptr);
  ASSERT_NE(spare, nullptr);
  std::vector<void *> filler;  // never freed (see the top of the file)
  for (size_t size = size_t{1} << 30; size >= 64;) {
    if (void *block = rte_malloc(nullptr, size, 0)) {
      filler.push_back(block);
    } else {
      size /= 2;
    }
  }
  rte_free(room);

  const Op rule = Upsert(0xf0, 0x10);
  try {
    const TransactionEngine::Result result = engine.Apply(std::vector<Op>{rule});
    ADD_FAILURE() << "not refused for memory: outcome "
                  << static_cast<int>(result.outcome) << ", "
                  << result.ops.at(0).error;
  } catch (const utils::MemoryRefused &e) {
    EXPECT_EQ(e.reason(), utils::MemoryRefused::Reason::kNormalLimit) << e.what();
  }
  EXPECT_EQ(table->size(), 1u);
  EXPECT_EQ(table->tuple_count(), 1u);

  // Memory back: the same rule applies beside the first.
  rte_free(spare);
  ASSERT_EQ(engine.Apply(std::vector<Op>{rule}).outcome, Outcome::kApplied);
  EXPECT_EQ(table->size(), 2u);
  EXPECT_EQ(table->tuple_count(), 2u);

  const std::string names[] = {"masked"};
  ASSERT_TRUE(engine.ReleaseForTeardown(names).has_value());
  for (int i = 0; i < 100000 && engine.ReclaimRetired() != 0; i++) {
  }
  domain.Drain();
}

}  // namespace
}  // namespace bess::classifier
