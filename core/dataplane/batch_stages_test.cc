// SPDX-License-Identifier: BSD-3-Clause

// K4.6: RunStages is stage-major -- every element through stage 1, then every
// element through stage 2 -- and adds nothing else.

#include "dataplane/batch_stages.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace {

using bess::dataplane::CacheGeometry;
using bess::dataplane::LookupBody;
using bess::dataplane::LookupShape;
using bess::dataplane::ResolveLookupBody;
using bess::dataplane::RunBatch;
using bess::dataplane::RunStages;

TEST(BatchStagesTest, StageMajorOrder) {
  std::vector<std::pair<int, size_t>> trace;
  RunStages(
      3, [&](size_t i) { trace.emplace_back(1, i); },
      [&](size_t i) { trace.emplace_back(2, i); },
      [&](size_t i) { trace.emplace_back(3, i); });
  const std::vector<std::pair<int, size_t>> expected = {
      {1, 0}, {1, 1}, {1, 2}, {2, 0}, {2, 1}, {2, 2}, {3, 0}, {3, 1}, {3, 2}};
  EXPECT_EQ(expected, trace);
}

TEST(BatchStagesTest, EmptyBatchRunsNothing) {
  int calls = 0;
  RunStages(0, [&](size_t) { calls++; }, [&](size_t) { calls++; });
  EXPECT_EQ(0, calls);
}

TEST(BatchStagesTest, LaterStagesSeeEarlierResults) {
  int squares[8];
  int sum = 0;
  RunStages(
      8, [&](size_t i) { squares[i] = static_cast<int>(i * i); },
      [&](size_t i) {
        bess::dataplane::Prefetch(&squares[i]);
        sum += squares[i];
      });
  EXPECT_EQ(140, sum);
}

TEST(BatchStagesTest, RunBatchPlainIsElementMajor) {
  std::vector<std::pair<int, size_t>> trace;
  RunBatch(
      LookupBody::kPlain, 2, [&](size_t i) { trace.emplace_back(1, i); },
      [&](size_t i) { trace.emplace_back(2, i); });
  const std::vector<std::pair<int, size_t>> expected = {
      {1, 0}, {2, 0}, {1, 1}, {2, 1}};
  EXPECT_EQ(expected, trace);

  trace.clear();
  RunBatch(
      LookupBody::kStaged, 2, [&](size_t i) { trace.emplace_back(1, i); },
      [&](size_t i) { trace.emplace_back(2, i); });
  const std::vector<std::pair<int, size_t>> staged = {
      {1, 0}, {1, 1}, {2, 0}, {2, 1}};
  EXPECT_EQ(staged, trace);
}

// The measured rule (K4.6): staged only for tables beyond L1d whose lookups
// walk dependent lines or branch on loaded data. (Runs without
// BESS_LOOKUP_BODY set.)
TEST(BatchTuningTest, ResolveRule) {
  CacheGeometry cache;
  cache.l1d_bytes = 48 * 1024;
  const auto resolve = [&](size_t bytes, unsigned lines, bool branches) {
    return ResolveLookupBody(
        LookupBody::kAuto,
        LookupShape{.table_bytes = bytes,
                    .dependent_lines = lines,
                    .branches_on_loaded_data = branches},
        cache);
  };
  EXPECT_EQ(LookupBody::kPlain, resolve(8 * 1024, 2, true));    // in L1d
  EXPECT_EQ(LookupBody::kStaged, resolve(1 << 20, 2, true));   // cuckoo
  EXPECT_EQ(LookupBody::kStaged, resolve(1 << 20, 1, true));   // L2Forward
  EXPECT_EQ(LookupBody::kPlain, resolve(64 << 20, 1, false));  // rte_lpm
  EXPECT_EQ(LookupBody::kPlain,
            ResolveLookupBody(LookupBody::kPlain,
                              LookupShape{.table_bytes = 1 << 30,
                                          .dependent_lines = 2},
                              cache))
      << "an explicit choice passes through";
}

TEST(BatchTuningTest, HostGeometryIsSane) {
  const CacheGeometry &g = CacheGeometry::Smallest();
  EXPECT_GE(g.line_bytes, 32u);
  EXPECT_GE(g.l1d_bytes, 8u * 1024);
  EXPECT_GE(g.l2_bytes, g.l1d_bytes);
  EXPECT_GE(g.l3_bytes, g.l2_bytes);
  const CacheGeometry cpu0 = CacheGeometry::ForCpu(0);
  EXPECT_GE(cpu0.l1d_bytes, g.l1d_bytes);
}

}  // namespace
