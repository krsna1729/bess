// SPDX-License-Identifier: BSD-3-Clause

// ListMetrics against a live bessd (was bessctl/module_tests/metrics.py):
// runtime metrics move with transactions; gate counters (D-116) count what
// crossed each tracked gate.

#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "live_daemon.h"

namespace {

using bess::live::AllowedCores;
using bess::live::LiveTest;
using bess::live::OnWorker;
using bess::live::Types;
namespace v2 = bess::pb::v2;

std::optional<double> Value(const std::vector<v2::MetricSample> &samples, const std::string &name,
                            const std::map<std::string, std::string> &labels = {}) {
  for (const v2::MetricSample &s : samples) {
    if (s.name() != name) {
      continue;
    }
    bool match = true;
    for (const auto &[k, v] : labels) {
      const auto it = s.labels().find(k);
      match = match && it != s.labels().end() && it->second == v;
    }
    if (match) {
      return s.value();
    }
  }
  return std::nullopt;
}

class MetricsTest : public LiveTest {
 protected:
  std::vector<v2::MetricSample> Metrics() {
    auto m = client().Metrics();
    EXPECT_TRUE(m) << m.error().message;
    return m ? *m : std::vector<v2::MetricSample>{};
  }
};

TEST_F(MetricsTest, RuntimeMetricsMoveWithTransactions) {
  const auto before = Metrics();
  for (const char *name : {"bess_rcu_pending_retired_objects", "bess_rcu_grace_periods_started_total",
                           "bess_transaction_generation", "bess_transaction_pending_cascades"}) {
    EXPECT_TRUE(Value(before, name)) << name;
  }
  const double applied = Value(before, "bess_transactions_total", {{"outcome", "applied"}}).value_or(-1);
  const double rejected =
      Value(before, "bess_transactions_total", {{"outcome", "rejected"}}).value_or(-1);

  Types types(client());
  auto arg = types.Make("bess.pb.ExactMatchArg",
                        R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 30, "num_bytes": 4}]})");
  auto pipeline = bess::sdk::PipelineBuilder().Module("em", "ExactMatch", arg.get()).Build();
  ASSERT_TRUE(pipeline) << pipeline.error().message;
  ASSERT_TRUE(client().ApplyPipeline(*pipeline));
  auto rules = client().Resource("em/rules");
  ASSERT_TRUE(rules) << rules.error().message;

  // 10.0.0.1 -> 10.0.0.2 to gate 1: applied.
  {
    auto key = types.Make(rules->key_type,
                          R"({"fields": [{"value_bin": "CgAAAQ=="}, {"value_bin": "CgAAAg=="}]})");
    auto value = types.Make(rules->value_type, R"({"gate": 1})");
    auto tx = client().NewTransaction();
    ASSERT_TRUE(tx.Upsert(*rules, *key, *value));
    auto done = tx.Commit();
    ASSERT_TRUE(done) << done.error().message;
  }
  // Erasing a key that is not there is refused: a rejection.
  {
    auto key = types.Make(rules->key_type,
                          R"({"fields": [{"value_bin": "CQkJCQ=="}, {"value_bin": "CQkJCQ=="}]})");
    auto tx = client().NewTransaction();
    ASSERT_TRUE(tx.Erase(*rules, *key));
    auto done = tx.Commit();
    ASSERT_FALSE(done);
    EXPECT_EQ(bess::sdk::ErrorKind::kRejected, done.error().kind) << done.error().message;
  }

  const auto after = Metrics();
  EXPECT_EQ(applied + 1, Value(after, "bess_transactions_total", {{"outcome", "applied"}}));
  EXPECT_EQ(rejected + 1, Value(after, "bess_transactions_total", {{"outcome", "rejected"}}));
  EXPECT_GT(Value(after, "bess_transaction_generation").value_or(0),
            Value(before, "bess_transaction_generation").value_or(0));
}

// Source -> Sink on a worker: the source's output gate counts what it sent;
// a connected gate without traffic counts none; the samples stay sorted by
// name.
TEST_F(MetricsTest, GateCountersCountWhatCrossed) {
  Types types(client());
  auto src_arg = types.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  bess::pb::v2::TaskPlacement task;
  task.set_module("src");
  task.set_wid(0);
  auto pipeline = bess::sdk::PipelineBuilder()
                      .Worker(0, 0, "")
                      .Module("src", "Source", src_arg.get())
                      .Module("sink", "Sink", nullptr)
                      .Module("idle", "Bypass", nullptr)
                      .Module("idle_sink", "Sink", nullptr)
                      .Chain({"src", "sink"})
                      .Chain({"idle", "idle_sink"})
                      .Task(task)
                      .Build();
  ASSERT_TRUE(pipeline) << pipeline.error().message;
  auto applied = client().ApplyPipeline(*pipeline);
  ASSERT_TRUE(applied) << applied.error().message;
  ASSERT_TRUE(client().ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto paused = client().PauseWorkers();
  ASSERT_TRUE(paused) << paused.error().message;

  const auto samples = Metrics();
  const auto out = Value(samples, "bess_gate_packets_total",
                         {{"module", "src"}, {"gate", "0"}, {"direction", "out"}});
  ASSERT_TRUE(out);
  EXPECT_GT(*out, 0);
  // Track is an output-gate hook: input gates have no samples.
  EXPECT_FALSE(Value(samples, "bess_gate_packets_total", {{"module", "sink"}}));
  EXPECT_GT(Value(samples, "bess_gate_batches_total",
                  {{"module", "src"}, {"gate", "0"}, {"direction", "out"}})
                .value_or(0),
            0);
  EXPECT_EQ(0, Value(samples, "bess_gate_packets_total",
                     {{"module", "idle"}, {"gate", "0"}, {"direction", "out"}}));
  for (size_t i = 1; i < samples.size(); i++) {
    EXPECT_LE(samples[i - 1].name(), samples[i].name()) << "sorted by name";
  }
}

// A module's task is placed on one worker. Moving it while traffic runs moves
// the runs with it: the worker it left stops counting and the worker it went to
// starts. A task reachable from two schedulers at once -- the state the D-019
// invariant in drr.h describes, which is what makes its ingress ring hand out a
// slot nobody wrote -- would leave the old worker's count growing.
//
// Both halves are load-bearing, demonstrated by mutation: pinning the metric's
// worker label to "0" fails the worker-1 assertion ("the DRR's task did not run
// on worker 1 after the move"), and reporting the module's total runs under each
// worker's series fails the frozen-count check at the end. The worker-1 half is
// also what a daemon without the module label trips; the ASSERT at the top is
// what a missing worker-0 series trips.
TEST_F(MetricsTest, TaskRunsFollowThePlacement) {
  if (AllowedCores().size() < 2) {
    GTEST_SKIP() << "moving a task needs two allowed CPUs";
  }
  Types types(client());
  auto src_arg = types.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  auto drr_arg = types.Make("bess.pb.DRRArg", R"({"num_flows": 1024})");

  const auto pipeline_on = [&](int wid) {
    return bess::sdk::PipelineBuilder()
        .Worker(0, AllowedCores().at(0), "")
        .Worker(1, AllowedCores().at(1), "")
        .Module("src", "Source", src_arg.get())
        .Module("drr", "DRR", drr_arg.get())
        .Module("sink", "Sink", nullptr)
        .Chain({"src", "drr", "sink"})
        .Task(OnWorker("src", 0))
        .Task(OnWorker("drr", wid))
        .Build();
  };

  auto first = pipeline_on(0);
  ASSERT_TRUE(first) << first.error().message;
  auto applied = client().ApplyPipeline(*first);
  ASSERT_TRUE(applied) << applied.error().message;
  ASSERT_TRUE(client().ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto paused_first = client().PauseWorkers();
  ASSERT_TRUE(paused_first) << paused_first.error().message;

  const auto before = Value(Metrics(), "bess_module_observations_total",
                            {{"module", "drr"}, {"observation", "task_runs"}, {"worker", "0"}});
  ASSERT_TRUE(before) << "the DRR's task ran on worker 0";
  EXPECT_GT(before.value_or(-1), 0);

  // The same graph with the task on the other worker. The workers are resumed
  // first, so ApplyPipeline has to quiesce running workers itself -- the path
  // where a placement and an orphan attach meet -- rather than being handed a
  // paused dataplane. A CounterSet is what makes the read above safe while
  // they run.
  ASSERT_TRUE(client().ResumeWorkers());
  auto second = pipeline_on(1);
  ASSERT_TRUE(second) << second.error().message;
  applied = client().ApplyPipeline(*second);
  ASSERT_TRUE(applied) << applied.error().message;
  ASSERT_TRUE(client().ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto paused_after_move = client().PauseWorkers();
  ASSERT_TRUE(paused_after_move) << paused_after_move.error().message;

  const auto settled = Value(Metrics(), "bess_module_observations_total",
                             {{"module", "drr"}, {"observation", "task_runs"}, {"worker", "0"}});
  ASSERT_TRUE(settled) << "the worker-0 series went away";
  const auto moved = Value(Metrics(), "bess_module_observations_total",
                           {{"module", "drr"}, {"observation", "task_runs"}, {"worker", "1"}});
  ASSERT_TRUE(moved) << "the DRR's task did not run on worker 1 after the move";
  EXPECT_GT(moved.value_or(-1), 0);

  // The task is worker 1's now, so more traffic must not move worker 0's count.
  ASSERT_TRUE(client().ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto paused_after = client().PauseWorkers();
  ASSERT_TRUE(paused_after) << paused_after.error().message;
  const auto after0 = Value(Metrics(), "bess_module_observations_total",
                            {{"module", "drr"}, {"observation", "task_runs"}, {"worker", "0"}});
  EXPECT_EQ(settled.value_or(-1), after0.value_or(-2))
      << "the worker the task left kept running it";
  EXPECT_TRUE(daemon().Alive());
}

}  // namespace
