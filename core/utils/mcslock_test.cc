// SPDX-License-Identifier: BSD-3-Clause

#include "utils/mcslock.h"

#include <gtest/gtest.h>

#include <sched.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// Mutual exclusion and hand-off under contention: every increment of a plain
// counter happens inside the lock, so none may be lost, and every waiter must
// be woken (a lost hand-off hangs the test). An MCS lock hands over in strict
// FIFO order, so with more spinning threads than CPUs each hand-off can wait
// for a scheduler time slice: the thread count follows the CPUs this process
// may use (at least 2, for contention) and the iteration count stays small.
TEST(McsLockTest, ContendedIncrementsAreNeverLost) {
  mcslock_t lock;
  mcs_lock_init(&lock);
  uint64_t counter = 0;
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  ASSERT_EQ(0, sched_getaffinity(0, sizeof(cpus), &cpus));
  const int kThreads = std::clamp(CPU_COUNT(&cpus), 2, 4);
  constexpr int kIterations = 2000;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&] {
      for (int i = 0; i < kIterations; i++) {
        mcslock_node_t node;
        mcs_lock(&lock, &node);
        counter = counter + 1;
        mcs_unlock(&lock, &node);
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  EXPECT_EQ(static_cast<uint64_t>(kThreads) * kIterations, counter);
  EXPECT_FALSE(mcs_is_locked(&lock));
}

TEST(McsLockTest, TrylockFailsWhileHeldAndItsNodeUnlocksCleanly) {
  mcslock_t lock;
  mcs_lock_init(&lock);

  // A node with stale contents, as a stack node often has: trylock must
  // initialize it, or the unlock below follows a garbage `next`.
  mcslock_node_t node;
  std::memset(static_cast<void *>(&node), 0xa5, sizeof(node));
  ASSERT_TRUE(mcs_trylock(&lock, &node));
  EXPECT_TRUE(mcs_is_locked(&lock));

  mcslock_node_t other;
  EXPECT_FALSE(mcs_trylock(&lock, &other));

  mcs_unlock(&lock, &node);
  EXPECT_FALSE(mcs_is_locked(&lock));
  ASSERT_TRUE(mcs_trylock(&lock, &other));
  mcs_unlock(&lock, &other);
  EXPECT_FALSE(mcs_is_locked(&lock));
}

}  // namespace
