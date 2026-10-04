// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_MCSLOCK_H_
#define BESS_UTILS_MCSLOCK_H_

// MCS queue lock: each waiter spins on its own node. The orders below make it
// correct on weakly ordered CPUs; on x86 they compile to the same XCHG,
// CMPXCHG and plain loads/stores as the original volatile/__sync version.

#include <atomic>

#include "arch/cpu.h"

struct mcslock_node {
  std::atomic<mcslock_node *> next;
  std::atomic<bool> locked;
};

typedef struct mcslock_node mcslock_node_t;

struct alignas(bess::arch::kCacheLineSize) mcslock {
  std::atomic<mcslock_node_t *> tail;
};

typedef struct mcslock mcslock_t;

static inline void mcs_lock_init(mcslock_t *lock) {
  lock->tail.store(nullptr, std::memory_order_relaxed);
}

static inline void mcs_lock(mcslock_t *lock, mcslock_node_t *mynode) {
  mynode->next.store(nullptr, std::memory_order_relaxed);
  mynode->locked.store(true, std::memory_order_relaxed);

  // acquire: the previous holder's critical section (published by its
  // release CAS on tail); release: the successor that swaps after us sees
  // mynode->next == nullptr before it links itself in.
  mcslock_node_t *pre = lock->tail.exchange(mynode, std::memory_order_acq_rel);
  if (pre == nullptr) {
    return;
  }

  /* it's held by others. queue up and spin on the node of myself */
  pre->next.store(mynode, std::memory_order_release);

  // acquire pairs with the predecessor's release in mcs_unlock().
  while (mynode->locked.load(std::memory_order_acquire)) {
    bess::arch::CpuRelax();
  }
}

static inline void mcs_unlock(mcslock_t *lock, mcslock_node_t *mynode) {
  mcslock_node_t *next = mynode->next.load(std::memory_order_acquire);
  if (next == nullptr) {
    mcslock_node_t *expected = mynode;
    if (lock->tail.compare_exchange_strong(expected, nullptr,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
      return;
    }

    // A successor swapped itself in but has not linked yet.
    while ((next = mynode->next.load(std::memory_order_acquire)) == nullptr) {
      bess::arch::CpuRelax();
    }
  }

  next->locked.store(false, std::memory_order_release);
}

static inline int mcs_trylock(mcslock_t *lock, mcslock_node_t *mynode) {
  mynode->next.store(nullptr, std::memory_order_relaxed);
  mynode->locked.store(true, std::memory_order_relaxed);
  mcslock_node_t *expected = nullptr;
  return lock->tail.compare_exchange_strong(expected, mynode,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed);
}

static inline int mcs_is_locked(mcslock_t *lock) {
  return lock->tail.load(std::memory_order_relaxed) != nullptr;
}

#endif  // BESS_UTILS_MCSLOCK_H_
