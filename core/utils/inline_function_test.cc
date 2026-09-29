// SPDX-License-Identifier: BSD-3-Clause

#include "utils/inline_function.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <utility>

namespace bess::utils {
namespace {

using Fn = InlineFunction<int(int), 48>;

struct Counted {
  static inline int live = 0;
  Counted() { live++; }
  Counted(Counted &&) noexcept { live++; }
  ~Counted() { live--; }
};

TEST(InlineFunctionTest, CallsWhatItHolds) {
  int base = 40;
  Fn add([&base](int x) { return base + x; });
  ASSERT_TRUE(add);
  EXPECT_EQ(add(2), 42);
  base = 1;
  EXPECT_EQ(add(2), 3);
  EXPECT_FALSE(Fn());
}

TEST(InlineFunctionTest, HoldsMoveOnlyCallablesAndMovesThem) {
  auto owned = std::make_unique<int>(7);
  Fn f([p = std::move(owned)](int x) { return *p * x; });
  Fn g(std::move(f));
  EXPECT_FALSE(f);  // NOLINT: moved-from is empty, by contract
  EXPECT_EQ(g(6), 42);
  Fn h;
  h = std::move(g);
  EXPECT_FALSE(g);  // NOLINT
  EXPECT_EQ(h(2), 14);
}

TEST(InlineFunctionTest, DestroysExactlyOnce) {
  {
    Fn f([c = Counted()](int x) { return x; });
    EXPECT_EQ(Counted::live, 1);
    Fn g(std::move(f));
    EXPECT_EQ(Counted::live, 1);  // moved, the source destroyed
    g = Fn([](int x) { return x; });
    EXPECT_EQ(Counted::live, 0);  // replaced
    Fn h([c = Counted()](int x) { return x; });
    EXPECT_EQ(Counted::live, 1);
  }
  EXPECT_EQ(Counted::live, 0);
}

TEST(InlineFunctionTest, FillsItsWholeCapacity) {
  std::array<char, 48> bytes{};
  bytes[47] = 5;
  Fn f([bytes](int x) { return bytes[47] + x; });
  EXPECT_EQ(f(1), 6);
  // A 49-byte capture does not compile ("callable too large for its inline
  // storage"); checked once by hand, as a compile failure cannot be a test.
}

}  // namespace
}  // namespace bess::utils
