// SPDX-License-Identifier: BSD-3-Clause

// Dataplane transactions (G1.2c, D-025) while a worker forwards traffic (was
// bessctl/module_tests/dataplane_transactions.py): rule churn through the
// SDK is applied (or answered busy) beside the running worker, the rules take
// effect on the traffic, and the daemon stays up. Steering and erasing are
// control_sdk_test.cc's; cross-module all-or-nothing, replay and
// GetTransaction are core/control/dataplane_transactions_test.cc's.

#include <array>
#include <format>
#include <string>

#include "live_daemon.h"
#include "steering.h"

namespace {

using bess::live::LiveTest;
using bess::live::Types;

// 10.(i / 256 % 256).(i % 256).1, host order.
uint32_t Session(int i) {
  return 0x0A000001u | static_cast<uint32_t>(i / 256 % 256) << 16 |
         static_cast<uint32_t>(i % 256) << 8;
}

class DataplaneTransactionsTest : public LiveTest {};

TEST_F(DataplaneTransactionsTest, TransactionsWhileTrafficRuns) {
  Types types(client());
  // src -> upd (10.0.0.1 -> 10.0.0.2) -> rnd (the source's middle two bytes:
  // sessions 0-255) -> em (default gate 0) -> gates 0-3 -> sinks.
  auto src = types.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  auto upd = types.Make("bess.pb.UpdateArg",
                        R"({"fields": [{"offset": 26, "size": 4, "value": 167772161},
                                       {"offset": 30, "size": 4, "value": 167772162}]})");
  auto rnd = types.Make("bess.pb.RandomUpdateArg",
                        R"({"fields": [{"offset": 27, "size": 2, "min": 0, "max": 255}]})");
  auto em = types.Make("bess.pb.ExactMatchArg",
                       R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 30, "num_bytes": 4}],
                           "default_gate": 0})");
  bess::pb::v2::TaskPlacement task;
  task.set_module("src");
  task.set_wid(0);
  bess::sdk::PipelineBuilder b;
  b.Worker(0, 0, "")
      .Module("src", "Source", src.get())
      .Module("upd", "Update", upd.get())
      .Module("rnd", "RandomUpdate", rnd.get())
      .Module("em", "ExactMatch", em.get())
      .Chain({"src", "upd", "rnd", "em"})
      .Task(task);
  for (uint32_t gate = 0; gate < 4; gate++) {
    const std::string sink = "sink" + std::to_string(gate);
    b.Module(sink, "Sink", nullptr).Connect("em", gate, sink, 0);
  }
  auto pipeline = b.Build();
  ASSERT_TRUE(pipeline) << pipeline.error().message;
  auto applied_pipeline = client().ApplyPipeline(*pipeline);
  ASSERT_TRUE(applied_pipeline) << applied_pipeline.error().message;
  auto rules = client().Resource(bess::live::kSteeringResource);
  ASSERT_TRUE(rules) << rules.error().message;

  const auto before = bess::live::EmGateCounts(client());
  ASSERT_TRUE(client().ResumeWorkers());
  int applied = 0;
  int busy = 0;
  // Add a rule for session i (to gate 1 + i % 3), remove session i - 16's.
  for (int i = 0; i < 200; i++) {
    auto tx = client().NewTransaction();
    auto value = types.Make(rules->value_type, std::format(R"({{"gate": {}}})", 1 + i % 3));
    ASSERT_TRUE(tx.Upsert(*rules, *types.Make(rules->key_type,
                                              bess::live::RuleKeyJson(Session(i), 0x0A000002)),
                          *value));
    if (i >= 16) {
      ASSERT_TRUE(tx.Erase(*rules, *types.Make(rules->key_type, bess::live::RuleKeyJson(
                                                                   Session(i - 16), 0x0A000002))));
    }
    auto done = tx.Commit();
    if (!done && done.error().kind == bess::sdk::ErrorKind::kBusy) {
      busy++;
      continue;
    }
    ASSERT_TRUE(done) << "transaction " << i << ": " << done.error().message;
    applied++;
  }
  ASSERT_TRUE(client().PauseWorkers());
  ASSERT_TRUE(daemon().Alive());

  const auto after = bess::live::EmGateCounts(client());
  std::array<double, 4> forwarded{};
  for (size_t g = 0; g < 4; g++) {
    forwarded[g] = after[g] - before[g];
  }
  EXPECT_GT(applied, 10) << busy << " busy";
  EXPECT_GT(forwarded[0], 0) << "packets of sessions without a rule";
  EXPECT_GT(forwarded[1] + forwarded[2] + forwarded[3], 0)
      << "the rules steered nothing while the worker ran";
}

}  // namespace
