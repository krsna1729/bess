// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "histogram.h"

#include <gtest/gtest.h>

namespace {

TEST(HistogramTest, U32Quartiles) {
  // 1002 is out of range, thus will be floored to 1000
  const std::vector<uint32_t> values = {1, 2, 3, 4, 5, 1002};

  Histogram<uint32_t> hist(1000, 1);
  for (uint32_t x : values) {
    hist.Insert(x);
  }

  auto ret = hist.Summarize({25.0, 50.0, 75.0, 100.0});

  EXPECT_EQ(1, ret.above_range);
  EXPECT_EQ(1, ret.min);
  EXPECT_EQ(1000, ret.max);
  EXPECT_EQ(169, ret.avg);
  EXPECT_EQ(6, ret.count);
  EXPECT_EQ(1015, ret.total);
  EXPECT_EQ(2, ret.percentile_values[0]);     // 25th percentile
  EXPECT_EQ(4, ret.percentile_values[1]);     // 50th percentile
  EXPECT_EQ(5, ret.percentile_values[2]);     // 75th percentile
  EXPECT_EQ(1000, ret.percentile_values[3]);  // 100th percentile
}

TEST(HistogramTest, DoubleQuartiles) {
  const std::vector<double> values = {1.0, 1.0, 2.0, 2.0, 4.0, 6.0};

  Histogram<double> hist(1000, 0.5);
  for (double x : values) {
    hist.Insert(x);
  }

  auto ret = hist.Summarize({25.0, 50.0, 75.0, 100.0});

  EXPECT_EQ(0, ret.above_range);
  EXPECT_DOUBLE_EQ(1.0, ret.min);
  EXPECT_DOUBLE_EQ(6.0, ret.max);
  EXPECT_DOUBLE_EQ(16.0 / 6, ret.avg);
  EXPECT_EQ(6, ret.count);
  EXPECT_DOUBLE_EQ(16.0, ret.total);
  EXPECT_DOUBLE_EQ(1.0, ret.percentile_values[0]);  // 25th percentile
  EXPECT_DOUBLE_EQ(2.0, ret.percentile_values[1]);  // 50th percentile
  EXPECT_DOUBLE_EQ(4.0, ret.percentile_values[2]);  // 75th percentile
  EXPECT_DOUBLE_EQ(6.0, ret.percentile_values[3]);  // 100th percentile
}

}  // namespace (unnamed)
