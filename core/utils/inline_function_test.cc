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
