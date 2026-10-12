// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

// Benchmarks for TC / scheduler.

#include "utils/logging.h"
#include <benchmark/benchmark.h>

#include <vector>

#include "module.h"

#include "scheduler.h"
#include "traffic_class.h"

#define CT TrafficClassBuilder::CreateTree

using namespace bess;
using namespace bess::traffic_class_initializer_types;

namespace {

// The fixture's second argument: the resource, as an integer.
constexpr int64_t kCountArg = static_cast<int64_t>(Resource::kCount);
constexpr int64_t kCycleArg = static_cast<int64_t>(Resource::kCycle);

class DummyModule : public Module {
 public:
  struct task_result RunTask(Context *, bess::PacketBatch *,
                             void *arg) override;
};

[[gnu::noinline]] struct task_result DummyModule::RunTask(Context *,
                                                          bess::PacketBatch *,
                                                          void *) {
  return {.block = false, .packets = 0, .bits = 0};
}

// Performs TC Scheduler init/deinit before/after each test.
// Sets up a tree for weighted fair benchmarking.
class TCWeightedFair : public benchmark::Fixture {
 public:
  TCWeightedFair() : s_(), dummy_() {}

  void SetUp(benchmark::State &state) override {
    int num_classes = state.range(0);
    const auto resource = static_cast<Resource>(state.range(1));

    dummy_ = new DummyModule;

    TrafficClass *root =
        CT("root", {PRIORITY},
           {{0, CT("weighted", {WEIGHTED_FAIR, resource}, {})}});
    s_ = new DefaultScheduler(root);
    WeightedFairTrafficClass *weighted =
        static_cast<WeightedFairTrafficClass *>(
            TrafficClassBuilder::Find("weighted"));
    for (int i = 0; i < num_classes; i++) {
      std::string name("class_" + std::to_string(i));
      LeafTrafficClass *c =
          TrafficClassBuilder::CreateTrafficClass<LeafTrafficClass>(
              name, new Task(dummy_, nullptr));

      resource_share_t share = 1;
      CHECK(weighted->AddChild(c, share));
    }
    CHECK(!root->blocked());
    CHECK(!weighted->blocked());
  }

  void TearDown(benchmark::State &) override {
    delete s_;
    s_ = nullptr;

    delete dummy_;
    dummy_ = nullptr;

    TrafficClassBuilder::ClearAll();
  }

 protected:
  DefaultScheduler *s_;
  Module *dummy_;
};

// Benchmarks the schedule_once() routine in TC.  For Resource::kCount.
BENCHMARK_DEFINE_F(TCWeightedFair, TCScheduleOnceCount)
(benchmark::State &state) {
  while (state.KeepRunning()) {
    Context ctx = {};
    s_->ScheduleOnce(&ctx);
  }
  state.SetItemsProcessed(state.iterations());
  state.SetComplexityN(state.range(0));
}

// Benchmarks the schedule_once() routine in TC.  For Resource::kCycle.
BENCHMARK_DEFINE_F(TCWeightedFair, TCScheduleOnceCycle)
(benchmark::State &state) {
  while (state.KeepRunning()) {
    Context ctx = {};
    s_->ScheduleOnce(&ctx);
  }
  state.SetItemsProcessed(state.iterations());
  state.SetComplexityN(state.range(0));
}

BENCHMARK_REGISTER_F(TCWeightedFair, TCScheduleOnceCount)
    ->Args({4 << 0, kCountArg})
    ->Args({4 << 1, kCountArg})
    ->Args({4 << 2, kCountArg})
    ->Args({4 << 3, kCountArg})
    ->Args({4 << 4, kCountArg})
    ->Args({4 << 5, kCountArg})
    ->Args({4 << 6, kCountArg})
    ->Args({4 << 7, kCountArg})
    ->Args({4 << 8, kCountArg})
    ->Args({4 << 9, kCountArg})
    ->Args({4 << 10, kCountArg})
    ->Args({4 << 11, kCountArg})
    ->Args({4 << 12, kCountArg})
    ->Args({4 << 13, kCountArg})
    ->Args({4 << 14, kCountArg})
    ->Complexity();

BENCHMARK_REGISTER_F(TCWeightedFair, TCScheduleOnceCycle)
    ->Args({4 << 0, kCycleArg})
    ->Args({4 << 1, kCycleArg})
    ->Args({4 << 2, kCycleArg})
    ->Args({4 << 3, kCycleArg})
    ->Args({4 << 4, kCycleArg})
    ->Args({4 << 5, kCycleArg})
    ->Args({4 << 6, kCycleArg})
    ->Args({4 << 7, kCycleArg})
    ->Args({4 << 8, kCycleArg})
    ->Args({4 << 9, kCycleArg})
    ->Args({4 << 10, kCycleArg})
    ->Args({4 << 11, kCycleArg})
    ->Args({4 << 12, kCycleArg})
    ->Args({4 << 13, kCycleArg})
    ->Args({4 << 14, kCycleArg})
    ->Complexity();

// Performs TC Scheduler init/deinit before/after each test.
class TCRoundRobin : public benchmark::Fixture {
 public:
  TCRoundRobin() : s_(), dummy_() {}

  void SetUp(benchmark::State &state) override {
    int num_classes = state.range(0);

    dummy_ = new DummyModule;

    TrafficClass *root = CT("rr", {ROUND_ROBIN}, {});
    s_ = new DefaultScheduler(root);
    RoundRobinTrafficClass *rr =
        static_cast<RoundRobinTrafficClass *>(TrafficClassBuilder::Find("rr"));

    for (int i = 0; i < num_classes; i++) {
      std::string name("class_" + std::to_string(i));
      LeafTrafficClass *c =
          TrafficClassBuilder::CreateTrafficClass<LeafTrafficClass>(
              name, new Task(dummy_, nullptr));

      CHECK(rr->AddChild(c));
    }
    CHECK(!rr->blocked());
  }

  void TearDown(benchmark::State &) override {
    delete s_;
    s_ = nullptr;

    delete dummy_;
    dummy_ = nullptr;

    TrafficClassBuilder::ClearAll();
  }

 protected:
  DefaultScheduler *s_;
  Module *dummy_;
};

BENCHMARK_DEFINE_F(TCRoundRobin, TCScheduleOnce)(benchmark::State &state) {
  while (state.KeepRunning()) {
    Context ctx = {};
    s_->ScheduleOnce(&ctx);
  }
  state.SetItemsProcessed(state.iterations());
  state.SetComplexityN(state.range(0));
}

BENCHMARK_REGISTER_F(TCRoundRobin, TCScheduleOnce)
    ->Args({1 << 0})
    ->Args({4 << 0})
    ->Args({4 << 1})
    ->Args({4 << 2})
    ->Args({4 << 3})
    ->Args({4 << 4})
    ->Args({4 << 5})
    ->Args({4 << 6})
    ->Args({4 << 7})
    ->Args({4 << 8})
    ->Args({4 << 9})
    ->Args({4 << 10})
    ->Args({4 << 11})
    ->Args({4 << 12})
    ->Args({4 << 13})
    ->Args({4 << 14})
    ->Complexity();

}  // namespace

BENCHMARK_MAIN();
