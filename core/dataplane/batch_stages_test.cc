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
