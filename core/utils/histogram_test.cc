// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "histogram.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "framework/memory_scope.h"

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

// What the constructor allocates (glibc) is what the histogram reports
// (D-198).
TEST(HistogramTest, BytesMatchesTheConstructor) {
  using H = Histogram<uint64_t>;
  for (size_t n : {size_t{1}, size_t{100}, size_t{100000}}) {
    H h(n, 10);
    EXPECT_EQ(bess::framework::MemoryCost<H>::Bytes(n, uint64_t{10}),
              h.bytes());
    EXPECT_EQ(bess::framework::MemoryCost<H>::Bytes(n, uint64_t{10}),
              bess::framework::MemoryCost<H>::GlibcBytes(n, uint64_t{10}));
  }
  Histogram<double> d(7, 0.5);
  EXPECT_EQ(Histogram<double>::Bytes(7, 0.5), d.bytes());
}

// Records the growth a histogram asks (D-198); refuses it when `refuse` is
// set.
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

// Resize admits the new buckets first and gives back the old ones after; a
// refusal changes nothing.
TEST(HistogramTest, ResizeAdmitsTheNewBucketsFirst) {
  Histogram<uint64_t> h(10, 1);
  RecordingGrowth growth;
  h.set_memory_growth(&growth);
  const size_t small = h.bytes();
  const size_t big = Histogram<uint64_t>::Bytes(1000, 2);
  h.Resize(1000, 2);
  EXPECT_EQ((std::vector<std::pair<size_t, size_t>>{{big, big}}), growth.grown);
  EXPECT_EQ((std::vector<std::pair<size_t, size_t>>{{small, small}}),
            growth.shrunk);
  EXPECT_EQ(big, h.bytes());
  EXPECT_EQ(2u, h.bucket_width());

  growth.refuse = true;
  EXPECT_THROW(h.Resize(5000, 3), bess::utils::MemoryRefused);
  EXPECT_EQ(big, h.bytes());
  EXPECT_EQ(2u, h.bucket_width());
  EXPECT_EQ(1u, growth.shrunk.size());
}

}  // namespace (unnamed)
