// SPDX-License-Identifier: BSD-3-Clause

// Measure's commands as control_v2 runs them (D-129): both are actions
// (they read and clear counters, not configuration); get_summary answers a
// MeasureCommandGetSummaryResponse.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <vector>

#include "module.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;

// Packets pass through and are counted; with no timestamp attribute writer
// and no timestamp in the payload none is sampled for latency.
TEST(MeasureTest, GetSummaryAndClearAreActions) {
  ModuleHarness h;
  Module *measure = h.Create("Measure", bess::pb::MeasureArg());
  ASSERT_NE(nullptr, measure);

  const std::vector<Bytes> in(5, Bytes(64, 0));
  EXPECT_EQ(in.size(), h.Run(measure, 0, in, {0})[0].size());

  CommandResponse r =
      h.Action(measure, "get_summary", bess::pb::MeasureCommandGetSummaryArg());
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  bess::pb::MeasureCommandGetSummaryResponse summary;
  ASSERT_TRUE(r.data().UnpackTo(&summary));
  EXPECT_EQ(5u, summary.packets());
  EXPECT_EQ(0u, summary.latency().count());

  r = h.Action(measure, "clear", bess::pb::EmptyArg());
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  r = h.Action(measure, "get_summary", bess::pb::MeasureCommandGetSummaryArg());
  ASSERT_EQ(0, r.error().code()) << r.error().errmsg();
  ASSERT_TRUE(r.data().UnpackTo(&summary));
  EXPECT_EQ(0u, summary.packets());

  // An invalid argument is still refused through the action surface.
  bess::pb::MeasureCommandGetSummaryArg bad;
  bad.add_latency_percentiles(101.0);
  EXPECT_EQ(EINVAL, h.Action(measure, "get_summary", bad).error().code());
}

}  // namespace
