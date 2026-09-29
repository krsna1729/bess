// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "shared_obj.h"

#include <gtest/gtest.h>

namespace bess {

int num_constructed;
int num_destructed;

class Test : public ::testing::Test {
 protected:
  virtual void SetUp() {
    num_constructed = 0;
    num_destructed = 0;
  }
  virtual void TearDown() { ASSERT_EQ(num_constructed, num_destructed); }
};

class FooType final {
 public:
  FooType() : a_(1) { num_constructed++; }
  FooType(int x, int y) : a_(x + y) { num_constructed++; }
  ~FooType() { num_destructed++; }
  int a_;
};

class BarType final {
 public:
  BarType() : b_(2) { num_constructed++; }
  ~BarType() { num_destructed++; }
  int b_;
};

TEST_F(Test, Basic) {
  std::shared_ptr<FooType> ref_a = shared_objects.Get<FooType>("foo");
  ASSERT_EQ(num_constructed, 1);

  std::shared_ptr<FooType> ref_b = shared_objects.Get<FooType>("foo");
  ASSERT_EQ(num_constructed, 1);
  ASSERT_EQ(ref_a, ref_b);
  ASSERT_EQ(num_destructed, 0);

  ref_a.reset();
  ASSERT_EQ(num_constructed, 1);
  ASSERT_EQ(num_destructed, 0);  // ref_b is still holding a reference
  ASSERT_NE(ref_a, ref_b);

  ref_b.reset();
  ASSERT_EQ(num_destructed, 1);
  ASSERT_EQ(ref_a, ref_b);
}

TEST_F(Test, MultipleObjects) {
  std::shared_ptr<FooType> ref_a = shared_objects.Get<FooType>("foo1");
  ASSERT_EQ(num_constructed, 1);

  std::shared_ptr<FooType> ref_b = shared_objects.Get<FooType>("foo2");
  ASSERT_EQ(num_constructed, 2);
  ASSERT_NE(ref_a, ref_b);
  ASSERT_EQ(num_destructed, 0);

  ref_a.reset();
  ASSERT_EQ(num_destructed, 1);
  ASSERT_NE(ref_a, ref_b);

  ref_b.reset();
  ASSERT_EQ(num_destructed, 2);
  ASSERT_EQ(ref_a, ref_b);
}

TEST_F(Test, TypeIsolation) {
  {
    std::shared_ptr<FooType> ref_a = shared_objects.Get<FooType>("foo");
    ASSERT_EQ(num_constructed, 1);
    ASSERT_EQ(ref_a->a_, 1);
    ref_a->a_ = 3;

    {
      std::shared_ptr<BarType> ref_b = shared_objects.Get<BarType>("foo");
      ASSERT_EQ(num_constructed, 2);
      ASSERT_EQ(ref_b->b_, 2);
      ASSERT_EQ(num_destructed, 0);
    }

    ASSERT_EQ(num_destructed, 1);
  }

  ASSERT_EQ(num_destructed, 2);

  // This is a newly created object with the same name as the previous one
  std::shared_ptr<FooType> ref_c = shared_objects.Get<FooType>("foo");
  ASSERT_EQ(num_constructed, 3);
  ASSERT_EQ(ref_c->a_, 1);
}

TEST_F(Test, Lookup) {
  std::shared_ptr<FooType> ref_a = shared_objects.Lookup<FooType>("foo");
  ASSERT_EQ(num_constructed, 0);
  ASSERT_EQ(static_cast<bool>(ref_a), false);
  ASSERT_EQ(ref_a.use_count(), 0);

  std::shared_ptr<FooType> ref_b = shared_objects.Get<FooType>("foo");
  ASSERT_EQ(num_constructed, 1);
  ASSERT_EQ(ref_b.use_count(), 1);

  std::shared_ptr<FooType> ref_c = shared_objects.Get<FooType>("foo");
  ASSERT_EQ(num_constructed, 1);
  ASSERT_EQ(ref_b.use_count(), 2);
  ASSERT_EQ(ref_c.use_count(), 2);
}

static int deferred_arg() {
  return 2;
}

TEST_F(Test, CustomConstructor) {
  int u = 40;
  std::shared_ptr<FooType> ref_a =
      shared_objects.Get<FooType>("foo", [&]() -> std::shared_ptr<FooType> {
        return std::make_shared<FooType>(u, deferred_arg());
      });
  ASSERT_EQ(num_constructed, 1);
  ASSERT_EQ(ref_a->a_, 42);
}

}  // namespace bess
