// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "classifier/classifier_api.h"

TEST(ClassifierApiTest, UmbrellaHeaderExposesBothFrontends) {
  bess::classifier::RuntimeClassifierSchema runtime;
  runtime.key_size = 1;
  EXPECT_EQ(1u, runtime.key_size);

  bess::classifier::ByteKey<4> typed{};
  EXPECT_EQ(4u, typed.size());
}
