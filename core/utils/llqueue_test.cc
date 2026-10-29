// Copyright (c) 2017, Joshua Stone.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "lock_less_queue.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "framework/memory_scope.h"

namespace {

using bess::utils::LockLessQueue;
using bess::utils::Queue;

// Simple test to make sure one can get back out object
TEST(LLQueueTest, SingleInputOutput) {
  LockLessQueue<int*> q(8);
  int* val = new int();
  ASSERT_NE(val, nullptr);
  ASSERT_FALSE(q.Push(val));
  ASSERT_EQ(q.Size(), 1);
  int* output;
  ASSERT_FALSE(q.Pop(output));
  ASSERT_EQ(output, val);
  ASSERT_EQ(q.Size(), 0);
  EXPECT_EQ(output, val);
  delete val;
}

// Tests to make sure that one can get back out multiple objects
TEST(LLQueueTest, MultiInputOutput) {
  LockLessQueue<int*> q(8);
  constexpr int n = 4;
  int* vals[n];
  for (int i = 0; i < n; i++) {
    vals[i] = new int();
  }
  for (int i = 0; i < n; i++) {
    ASSERT_FALSE(q.Push(vals[i]));
  }
  EXPECT_EQ(q.Size(), n);

  int* output = nullptr;
  ASSERT_FALSE(q.Pop(output));
  ASSERT_EQ(output, vals[0]);

  ASSERT_FALSE(q.Pop(output));
  ASSERT_EQ(output, vals[1]);

  ASSERT_FALSE(q.Pop(output));
  ASSERT_EQ(output, vals[2]);

  ASSERT_FALSE(q.Pop(output));
  ASSERT_EQ(output, vals[3]);
  EXPECT_EQ(q.Size(), 0);

  for (int i = 0; i < n; i++) {
    delete vals[i];
  }
}

// simple test to make sure that the queue is resized properly
TEST(LLQueueTest, Resize) {
  LockLessQueue<int*> q(8);

  constexpr int n = 6;
  int* vals1[n];
  int* vals2[n];
  for (int i = 0; i < n; i++) {
    vals1[i] = new int();
    vals2[i] = new int();
  }
  ASSERT_EQ(q.Push(vals1, n), n);
  EXPECT_EQ(q.Size(), n);

  ASSERT_EQ(q.Resize(16), 0);
  ASSERT_EQ(q.Capacity(), 16);
  ASSERT_EQ(q.Push(vals2, n), n);
  ASSERT_EQ(q.Size(), 2 * n);

  int** output = new int*[2*n];
  ASSERT_EQ(q.Pop(output, 2 * n), 2*n);
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(output[i], vals1[i]);
    ASSERT_EQ(output[i + n], vals2[i]);
  }

  for (int i = 0; i < n; i++) {
    delete vals1[i];
    delete vals2[i];
  }
  delete[] output;
}

// simple test to make sure that multiple objects can be enqueued and dequeued
// at the same time
TEST(LLQueueTest, MultiPushPop) {
  LockLessQueue<int*> q(16);
  constexpr int n = 6;
  int* vals[n];
  for (int i = 0; i < n; i++) {
    vals[i] = new int();
  }
  ASSERT_EQ(q.Push(vals, n), n);
  EXPECT_EQ(q.Size(), n);

  int** output = new int*[n];
  ASSERT_EQ(q.Pop(output, n), n);
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(output[i], vals[i]);
  }

  for (int i = 0; i < n; i++) {
    delete vals[i];
  }
  delete[] output;
}

// What the constructor allocates (the ring, glibc) is what the queue reports
// and what RingBytes says AllocRingMem takes (D-198).
TEST(LLQueueTest, BytesMatchesTheConstructor) {
  using Q = LockLessQueue<int*>;
  for (size_t capacity : {size_t{8}, size_t{256}, size_t{4096}}) {
    Q q(capacity);
    EXPECT_EQ(bess::framework::MemoryCost<Q>::Bytes(capacity), q.bytes());
    EXPECT_EQ(bess::framework::MemoryCost<Q>::Bytes(capacity),
              bess::framework::MemoryCost<Q>::GlibcBytes(capacity));
    const ssize_t raw = rte_ring_get_memsize(static_cast<unsigned>(capacity));
    ASSERT_GT(raw, 0);
    EXPECT_EQ(bess::utils::RingMemBytes(static_cast<size_t>(raw)), q.bytes());
    EXPECT_GE(q.bytes(), capacity * sizeof(void*));
  }
  Q fallback;
  EXPECT_EQ(Q::Bytes(), fallback.bytes());
  EXPECT_EQ(0u, bess::utils::RingBytes(size_t{1} << 40));
}

// Records the growth a queue asks (D-198); refuses it when `refuse` is set.
struct RecordingGrowth final : bess::utils::MemoryGrowth {
  std::vector<std::pair<size_t, size_t>> grown, shrunk;
  bool refuse = false;
  void Grow(size_t bytes, size_t glibc_bytes) override {
    if (refuse) {
      throw bess::utils::MemoryRefused(
          bess::utils::MemoryRefused::Reason::kNormalLimit, bytes, "refused");
    }
    grown.emplace_back(bytes, glibc_bytes);
  }
  void Shrink(size_t bytes, size_t glibc_bytes) noexcept override {
    shrunk.emplace_back(bytes, glibc_bytes);
  }
};

// Resize admits the new ring first and gives back the old one after.
TEST(LLQueueTest, ResizeAdmitsTheNewRingFirst) {
  LockLessQueue<int*> q(8);
  RecordingGrowth growth;
  q.set_memory_growth(&growth);
  const size_t small = q.bytes();
  const size_t big = bess::utils::RingBytes(64);
  ASSERT_EQ(0, q.Resize(64));
  EXPECT_EQ((std::vector<std::pair<size_t, size_t>>{{big, big}}), growth.grown);
  EXPECT_EQ((std::vector<std::pair<size_t, size_t>>{{small, small}}),
            growth.shrunk);
  EXPECT_EQ(big, q.bytes());
}

// A refused Resize leaves the queue as it was, items included.
TEST(LLQueueTest, ARefusedResizeChangesNothing) {
  LockLessQueue<int*> q(8);
  int value = 0;
  ASSERT_FALSE(q.Push(&value));
  RecordingGrowth growth;
  growth.refuse = true;
  q.set_memory_growth(&growth);
  const size_t before = q.bytes();
  EXPECT_THROW(q.Resize(64), bess::utils::MemoryRefused);
  EXPECT_EQ(8u, q.Capacity());
  EXPECT_EQ(before, q.bytes());
  EXPECT_EQ(1u, q.Size());
  EXPECT_TRUE(growth.shrunk.empty());
  int* out = nullptr;
  ASSERT_FALSE(q.Pop(out));
  EXPECT_EQ(&value, out);
}

}  // namespace
