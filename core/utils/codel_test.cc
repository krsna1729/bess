// Copyright (c) 2017, Joshua Stone.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "codel.h"

#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

using bess::utils::Codel;
using bess::utils::Queue;

struct ManualClock {
  const uint64_t *now;
  uint64_t operator()() const noexcept { return *now; }
};

void integer_drop(int* ptr) {
  delete ptr;
}

// Simple test to make sure one can get back out object
TEST(CodelTest, SingleInputOutput) {
  Codel<int*> c(&integer_drop, 8, 5000000, 100000000);
  int* val = new int();
  ASSERT_NE(val, nullptr);
  ASSERT_FALSE(c.Push(val));
  ASSERT_EQ(c.Size(), 1);
  int* output;
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, val);
  ASSERT_EQ(c.Size(), 0);
  EXPECT_EQ(output, val);
  delete val;
}

// Simple test to make sure one can get back out non-pointer object
TEST(CodelTest, NonPointerType) {
  Codel<int> c(NULL, 8, 5000000, 100000000);
  int val = 3439148;
  ASSERT_FALSE(c.Push(val));
  ASSERT_EQ(c.Size(), 1);
  int output;
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, val);
  ASSERT_EQ(c.Size(), 0);
  EXPECT_EQ(output, val);
}

// Tests to make sure that low delay entries won't be dropped
TEST(CodelTest, NoDropTest) {
  uint64_t now = 1'000'000'000;
  Codel<int*, ManualClock> c(&integer_drop, 8, 5000000, 100000000,
                             ManualClock{&now});

  constexpr int n = 4;
  int* vals[n];
  for (int i = 0; i < n; i++) {
    vals[i] = new int();
  }
  for (int i = 0; i < n; i++) {
    ASSERT_FALSE(c.Push(vals[i]));
  }
  EXPECT_EQ(c.Size(), n);

  now += 3'000'000;  // Low delay must not cause drops.
  int* output = nullptr;
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[0]);

  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[1]);

  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[2]);

  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[3]);
  EXPECT_EQ(c.Size(), 0);

  for (int i = 0; i < n; i++) {
    delete vals[i];
  }
}

// Tests to make sure that codel enters drop state at the proper time and
// properly drops at the correct time.
TEST(CodelTest, DropTest) {
  uint64_t now = 1'000'000'000;
  Codel<int*, ManualClock> c(&integer_drop, 16, 5000000, 100000000,
                             ManualClock{&now});

  constexpr int n = 10;
  int* vals[n];
  for (int i = 0; i < n; i++) {
    vals[i] = new int();
  }
  for (int i = 0; i < n; i++) {
    ASSERT_FALSE(c.Push(vals[i]));
  }
  EXPECT_EQ(c.Size(), n);

  now += 10'000'000;  // Every entry is above the 5 ms target.
  int* output = nullptr;

  // should mark above target after dequeuing this entry
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[0]);

  // still shouldn't drop yet because hasn't been interval since
  // being above target
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[1]);
  now += 100'000'000;  // Reach the exact above-target window boundary.

  // Should be above target for window.
  // should drop vals[2], enter drop state, and return 4th instead.
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[3]);

  // should now in be drop state and the next drop should be 100ms.
  now += 72'000'000;
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[4]);  // shouldn't be a drop yet only has been 72ms.

  now += 30'000'000;
  ASSERT_FALSE(c.Pop(output));
  // vals[5] should be dropped now has been over 100ms.
  ASSERT_EQ(output, vals[6]);
  // the next drop should be in approximately 70ms(100/sqrt(2))

  now += 40'000'000;
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[7]);  // it has only been 40ms should not be a drop yet

  now += 34'000'000;
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[9]);  // vals[8] should have been dropped

  // codel queue should be empty and return nullptr
  ASSERT_TRUE(c.Pop(output));
  EXPECT_EQ(c.Size(), 0);

  delete vals[0];
  delete vals[1];
  delete vals[3];
  delete vals[4];
  delete vals[6];
  delete vals[7];
  delete vals[9];
}

// Makes sure that codel will exit drop state at the proper time both when the
// front entry is low delay and when the object that returns after dropping the
// front entry is low delay.
TEST(CodelTest, ChangeStateTest) {
  uint64_t now = 1'000'000'000;
  Codel<int*, ManualClock> c(&integer_drop, 16, 5000000, 100000000,
                             ManualClock{&now});
  constexpr int n = 10;
  int* vals[n];
  for (int i = 0; i < n; i++) {
    vals[i] = new int();
  }

  for (int i = 0; i < 4; i++) {
    ASSERT_FALSE(c.Push(vals[i]));
  }
  EXPECT_EQ(c.Size(), 4);

  now += 10'000'000;  // Every entry is above the 5 ms target.
  int* output;

  // should change into mark above target after dequeuing this entry
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[0]);

  now += 105'000'000;  // Advance beyond the above-target window.

  // Should be above target for window.
  // should drop vals[1], enter drop state, and return 3rd instead.
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[2]);

  // should now in be drop state and the next drop should be 100ms.
  now += 104'000'000;
  ASSERT_FALSE(c.Push(vals[4]));
  ASSERT_FALSE(c.Pop(output));
  // vals[3] should be dropped now has been over 100ms.
  ASSERT_EQ(output, vals[4]);
  // val4 had a delay below target so should have left drop state.

  for (int i = 5; i < 8; i++) {
    ASSERT_FALSE(c.Push(vals[i]));
  }
  now += 72'000'000;  // Remaining in drop state would cause another drop.
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[5]);  // should now be marked as above target

  now += 205'000'000;  // Advance beyond two windows.
  // Should be above target for window.
  // should drop vals[6], enter drop state, and return 8th instead.
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[7]);

  ASSERT_FALSE(c.Push(vals[8]));
  ASSERT_FALSE(c.Push(vals[9]));
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[8]);
  // should have exited drop state as delay below target

  now += 72'000'000;  // Remaining in drop state would cause another drop.
  ASSERT_FALSE(c.Pop(output));
  ASSERT_EQ(output, vals[9]);

  // codel queue should be empty and return nullptr
  ASSERT_TRUE(c.Pop(output));
  EXPECT_EQ(c.Size(), 0);

  delete vals[0];
  delete vals[2];
  delete vals[4];
  delete vals[5];
  delete vals[7];
  delete vals[8];
  delete vals[9];
}

// simple test to make sure that multiple objects can be enqueued and dequeued
// at the same time
TEST(CodelTest, MultiPushPop) {
  uint64_t now = 1'000'000'000;
  Codel<int*, ManualClock> c(&integer_drop, 16, 5000000, 100000000,
                             ManualClock{&now});
  constexpr int n = 6;
  int* vals[n];
  for (int i = 0; i < n; i++) {
    vals[i] = new int();
  }
  ASSERT_EQ(c.Push(vals, n), n);
  EXPECT_EQ(c.Size(), n);

  int** output = new int*[n];
  ASSERT_EQ(c.Pop(output, n), n);
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(output[i], vals[i]);
  }

  for (int i = 0; i < n; i++) {
    delete vals[i];
  }
  delete[] output;
}

}  // namespace
