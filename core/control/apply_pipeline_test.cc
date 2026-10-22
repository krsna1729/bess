// SPDX-License-Identifier: BSD-3-Clause

// End-to-end exercise of the transactional control plane from C++ (G0 commit
// 6): a complete multi-object pipeline -- worker, modules, connections and a
// traffic-class hierarchy -- is applied, re-applied (no-op), failed at commit
// (the active pipeline must survive) and then changed again.
//
// Unlike the other control tests, this binary brings up a runtime the way the
// daemon does (DPDK EAL with --no-huge, packet pools, port drivers), because
// launching a worker needs a live EAL.

#include <gtest/gtest.h>

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "control/control_plane.h"
#include "control/pipeline_spec.h"
#include "module.h"
#include "runtime/runtime_state.h"
#include "control/transaction.h"
#include "runtime/worker_manager.h"
#include "runtime/numa_placement.h"
#include "runtime/thread_placement.h"
#include "dataplane/cpu_id.h"
#include "pb/module_msg.pb.h"
#include "pb/port_msg.pb.h"
#include "runtime/opts.h"
#include "packet_pool.h"
#include "port.h"
#include "worker.h"

namespace {

using bess::control::ApplyOptions;
using bess::control::ApplyResult;
using bess::control::ControlPlane;
using bess::control::PipelineSnapshot;
using bess::control::PipelineSpec;
using bess::control::SpecFromSnapshot;

const size_t kPoolCapacity = 32767;

// One runtime for the whole binary: EAL cannot be initialized twice, and the
// daemon-equivalent setup is the point of this test.
void InitRuntimeOnce() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;

  FLAGS_m = 0;  // malloc-backed, sandbox-safe: PlainPacketPool
  bess::PacketPool::CreateDefaultPools(kPoolCapacity);
  PortBuilder::InitDrivers();
}

// Two CPUs a worker spec may name: the kernel exposes the CPU's topology and it
// is in this process's set, which is what pipeline_validator.cc accepts. Not
// Placement::WorkerCpus(), which leaves the control CPU out -- on a two-CPU
// host that leaves one -- and not a hardcoded 0 and 1, since a CPU can be
// offline and a spec that names one is refused.
std::vector<int> TwoUsableCpus() {
  std::vector<int> cpus;
  for (const int c : bess::runtime::ProcessPlacement().cpus) {
    const std::optional<bess::dataplane::CpuId> cpu =
        bess::dataplane::CpuIdFrom(c);
    if (cpu && bess::runtime::CpuPresent(*cpu)) {
      cpus.push_back(c);
      if (cpus.size() == 2) {
        break;
      }
    }
  }
  return cpus;
}

// A pipeline with everything the structural path can do: a worker, two
// modules, a connection between them, and a traffic-class hierarchy whose root
// lands on that worker.
PipelineSpec FullPipeline() {
  PipelineSpec spec;

  bess::control::WorkerSpec worker;
  worker.wid = 0;
  worker.core = 0;
  spec.workers.push_back(worker);

  bess::control::ModuleSpec src;
  src.name = "src";
  src.mclass = "Bypass";
  spec.modules.push_back(src);

  bess::control::ModuleSpec sink;
  sink.name = "sink";
  sink.mclass = "Bypass";
  spec.modules.push_back(sink);

  bess::control::ConnectionSpec connection;
  connection.upstream = "src";
  connection.ogate = 0;
  connection.downstream = "sink";
  connection.igate = 0;
  spec.connections.push_back(connection);

  bess::control::TrafficClassSpec root;
  root.name = "root";
  root.policy = "round_robin";
  root.wid = 0;
  spec.traffic_classes.push_back(root);

  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "root";
  spec.traffic_classes.push_back(child);

  return spec;
}

class ApplyPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    control_plane_ = std::make_unique<ControlPlane>();
  }

  void TearDown() override {
    // Leave the shared runtime empty for the next test.
    (void)control_plane_->Reset();
  }

  std::unique_ptr<ControlPlane> control_plane_;
};

TEST_F(ApplyPipelineTest, AppliesACompletePipeline) {
  const uint64_t generation_before = bess::runtime::runtime().generation();

  auto applied = control_plane_->ApplyPipeline(FullPipeline(), {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  EXPECT_EQ(generation_before + 1, applied->generation);
  EXPECT_TRUE(applied->workers_paused);

  const PipelineSnapshot snapshot = control_plane_->GetPipeline();
  ASSERT_EQ(1u, snapshot.workers.size());
  EXPECT_EQ(0, snapshot.workers[0].wid);
  ASSERT_EQ(2u, snapshot.modules.size());
  EXPECT_EQ("sink", snapshot.modules[0].name);
  EXPECT_EQ("src", snapshot.modules[1].name);
  ASSERT_EQ(1u, snapshot.connections.size());
  EXPECT_EQ("src", snapshot.connections[0].upstream);
  EXPECT_EQ("sink", snapshot.connections[0].downstream);

  // The user-visible traffic classes are there, and the internal ones are not
  // part of desired state.
  size_t user_tcs = 0;
  for (const auto &tc : snapshot.traffic_classes) {
    if (!tc.name.empty() && tc.name[0] != '!') {
      user_tcs++;
    }
  }
  EXPECT_EQ(2u, user_tcs);

  // Applying the same desired state again changes nothing.
  auto again = control_plane_->ApplyPipeline(FullPipeline(), {});
  ASSERT_TRUE(again.has_value()) << again.error().message;
  EXPECT_EQ(generation_before + 1, again->generation);
  EXPECT_EQ(0u, again->applied_ops);
  EXPECT_FALSE(again->workers_paused);
}

// The module half of PolicyChangeIsRefusedTransactionally: a module's argument
// is applied at construction, so a different one is a kReplace in the diff, and
// the transaction that would perform it is refused - the live module cannot be
// reconfigured in place, and replacing it transactionally is not supported.
// The path the refusal names is two transactions: remove the module, then add
// it back with the new argument, which is what builds it again.
TEST_F(ApplyPipelineTest, AChangedModuleArgumentIsARefusedReplace) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());
  const PipelineSnapshot active = control_plane_->GetPipeline();

  // The runtime's own report, re-applied, is not a change.
  const PipelineSpec live = SpecFromSnapshot(active);
  auto same = control_plane_->DiffPipeline(live);
  ASSERT_TRUE(same.has_value()) << same.error().message;
  EXPECT_TRUE(same->modules.empty())
      << "a module reports the argument it was built with, so re-applying it "
         "changes nothing";

  // A different argument is.
  PipelineSpec changed = live;
  for (bess::control::ModuleSpec &module : changed.modules) {
    if (module.name == "src") {
      bess::pb::BypassArg arg;
      arg.set_cycles_per_packet(7);
      ASSERT_TRUE(module.arg.PackFrom(arg));
    }
  }
  auto diff = control_plane_->DiffPipeline(changed);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->modules.size()) << "only src's argument changed";
  EXPECT_EQ("src", diff->modules[0].name);
  EXPECT_EQ(bess::control::ChangeKind::kReplace, diff->modules[0].kind);

  // The transaction is refused, and nothing moves.
  const uint64_t generation = bess::runtime::runtime().generation();
  auto applied = control_plane_->ApplyPipeline(changed, {});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(bess::control::ControlErrorCode::kUnsupportedTransaction,
            applied.error().code);
  EXPECT_EQ("module", applied.error().object);
  EXPECT_EQ("transaction", applied.error().field);
  EXPECT_NE(std::string::npos,
            applied.error().message.find("applied at construction"))
      << applied.error().message;
  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  EXPECT_TRUE(control_plane_->GetPipeline() == active);

  // The path the refusal names: remove the module in one transaction, add it
  // back with the new argument in the next. The live snapshot reports the
  // argument the module was built with, so reading the new one back is what
  // proves it was built again.
  PipelineSpec without_src = changed;
  without_src.modules.erase(
      std::remove_if(without_src.modules.begin(), without_src.modules.end(),
                     [](const bess::control::ModuleSpec &module) {
                       return module.name == "src";
                     }),
      without_src.modules.end());
  without_src.connections.erase(
      std::remove_if(without_src.connections.begin(),
                     without_src.connections.end(),
                     [](const bess::control::ConnectionSpec &connection) {
                       return connection.upstream == "src";
                     }),
      without_src.connections.end());
  ASSERT_TRUE(control_plane_->ApplyPipeline(without_src, {}).has_value());
  ASSERT_TRUE(control_plane_->ApplyPipeline(changed, {}).has_value());

  bool found = false;
  for (const bess::control::ModuleSnapshot &module :
       control_plane_->GetPipeline().modules) {
    if (module.name != "src") {
      continue;
    }
    found = true;
    bess::pb::BypassArg seen;
    ASSERT_TRUE(module.arg.UnpackTo(&seen)) << module.arg.type_url();
    EXPECT_EQ(7u, seen.cycles_per_packet());
  }
  EXPECT_TRUE(found);

  // And nothing is left over: the same desired state again moves nothing.
  auto settled = control_plane_->ApplyPipeline(changed, {});
  ASSERT_TRUE(settled.has_value()) << settled.error().message;
  EXPECT_EQ(0u, settled->applied_ops);
}

TEST_F(ApplyPipelineTest, SnapshotOfTheActiveRuntimeIsTheDesiredState) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());

  const PipelineSpec running =
      SpecFromSnapshot(control_plane_->GetPipeline());
  auto diff = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  EXPECT_TRUE(diff->empty())
      << "the running pipeline must diff clean against its own description";
}

// D-123: a desired worker without a core is automatic. It gets a worker CPU
// of the memory node; GetPipeline reports that CPU; re-applying the same
// desired state (core still unset) matches the worker wherever it runs, so
// nothing moves, nothing is replaced and the generation stays.
TEST_F(ApplyPipelineTest, ReapplyingAWorkerWithoutACoreKeepsItsCpuAndReplacesNothing) {
  const bess::runtime::Placement &placement = bess::runtime::ProcessPlacement();
  if (placement.WorkerCpus().empty()) {
    GTEST_SKIP() << "no CPU for automatic workers: " << placement.Summary();
  }
  PipelineSpec spec = FullPipeline();
  spec.workers[0].core.reset();

  auto applied = control_plane_->ApplyPipeline(spec, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSnapshot first = control_plane_->GetPipeline();
  ASSERT_EQ(1u, first.workers.size());
  const int core = first.workers[0].core;
  EXPECT_EQ(1u, placement.WorkerCpus().count(core)) << placement.Summary();
  EXPECT_EQ(0u, placement.control_cpus.count(core)) << placement.Summary();

  // The active pipeline names the CPU the worker runs on.
  const PipelineSpec running = SpecFromSnapshot(first);
  ASSERT_EQ(1u, running.workers.size());
  EXPECT_EQ(std::optional<int>(core), running.workers[0].core);

  auto diff = control_plane_->DiffPipeline(spec);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  EXPECT_TRUE(diff->empty()) << "an automatic worker matches wherever it runs";

  auto again = control_plane_->ApplyPipeline(spec, {});
  ASSERT_TRUE(again.has_value()) << again.error().message;
  EXPECT_EQ(applied->generation, again->generation);
  EXPECT_EQ(0u, again->applied_ops);
  EXPECT_FALSE(again->workers_paused);
  EXPECT_TRUE(control_plane_->GetPipeline() == first) << "nothing moved";

  // What GetPipeline reported (the core explicit now) is the same pipeline.
  auto pinned = control_plane_->ApplyPipeline(running, {});
  ASSERT_TRUE(pinned.has_value()) << pinned.error().message;
  EXPECT_EQ(applied->generation, pinned->generation);
  EXPECT_EQ(0u, pinned->applied_ops);
}

// D-123: an explicit core that differs from where the worker runs replaces
// it, as before automatic workers existed.
TEST_F(ApplyPipelineTest, AnExplicitCoreOtherThanTheWorkersCpuIsAReplace) {
  const bess::runtime::Placement &placement = bess::runtime::ProcessPlacement();
  if (placement.WorkerCpus().size() < 2) {
    GTEST_SKIP() << "needs two CPUs for automatic workers: " << placement.Summary();
  }
  PipelineSpec spec = FullPipeline();
  spec.workers[0].core.reset();
  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());
  const int core = control_plane_->GetPipeline().workers.at(0).core;

  int other = -1;
  for (const int cpu : placement.WorkerCpus()) {
    if (cpu != core) {
      other = cpu;
      break;
    }
  }
  ASSERT_GE(other, 0);
  spec.workers[0].core = other;
  auto diff = control_plane_->DiffPipeline(spec);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->workers.size());
  EXPECT_EQ(bess::control::ChangeKind::kReplace, diff->workers[0].kind);
}

// D-123: in one desired pipeline, the workers with an explicit core are
// created first, so an automatic worker never takes a CPU another worker of
// the same pipeline names.
TEST_F(ApplyPipelineTest, AnAutomaticWorkerSkipsACpuAnExplicitWorkerOfThePipelineNames) {
  const bess::runtime::Placement &placement = bess::runtime::ProcessPlacement();
  if (placement.WorkerCpus().size() < 2) {
    GTEST_SKIP() << "needs two CPUs for automatic workers: " << placement.Summary();
  }
  const std::optional<int> first = placement.NextWorkerCpu({});
  ASSERT_TRUE(first.has_value());

  PipelineSpec spec = FullPipeline();
  spec.workers[0].core.reset();  // wid 0: automatic, planned first by wid
  bess::control::WorkerSpec pinned;
  pinned.wid = 1;
  pinned.core = *first;  // the CPU wid 0 would take on its own
  spec.workers.push_back(pinned);

  auto applied = control_plane_->ApplyPipeline(spec, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSnapshot snapshot = control_plane_->GetPipeline();
  ASSERT_EQ(2u, snapshot.workers.size());
  EXPECT_EQ(*first, snapshot.workers[1].core);
  EXPECT_NE(*first, snapshot.workers[0].core);
  const int used[] = {*first};
  EXPECT_EQ(placement.NextWorkerCpu(used), std::optional<int>(snapshot.workers[0].core));
}

// The acceptance test that matters most: a transaction that fails while
// committing must leave the previous pipeline active and usable.
TEST_F(ApplyPipelineTest, FailedCommitKeepsTheActivePipeline) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());

  const PipelineSnapshot active = control_plane_->GetPipeline();
  const uint64_t generation = bess::runtime::runtime().generation();
  const size_t modules = bess::runtime::runtime().modules().Size();
  const size_t tcs = bess::runtime::runtime().traffic_classes().Size();

  // Add a traffic-class hierarchy whose second half cannot attach: a child of a
  // priority class without a priority. The parent is created first, so the
  // commit fails with the parent already in place.
  PipelineSpec broken = FullPipeline();
  bess::control::TrafficClassSpec parent;
  parent.name = "zprio";
  parent.policy = "priority";
  parent.wid = 0;
  broken.traffic_classes.push_back(parent);

  bess::control::TrafficClassSpec child;
  child.name = "achild";
  child.policy = "round_robin";
  child.parent = "zprio";  // no priority: rejected at attach time
  broken.traffic_classes.push_back(child);

  auto applied = control_plane_->ApplyPipeline(broken, {});
  ASSERT_FALSE(applied.has_value());

  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  EXPECT_EQ(modules, bess::runtime::runtime().modules().Size());
  EXPECT_EQ(tcs, bess::runtime::runtime().traffic_classes().Size());
  EXPECT_TRUE(control_plane_->GetPipeline() == active)
      << "the previous pipeline must still be the active one";

  // ... and it is still usable: the same desired state still diffs clean.
  auto diff = control_plane_->DiffPipeline(FullPipeline());
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// Removal goes through the retire phase, and the runtime ends up consistent.
TEST_F(ApplyPipelineTest, RemovesModulesAndConnectionsAgain) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());
  const uint64_t after_apply = bess::runtime::runtime().generation();

  PipelineSpec reduced = FullPipeline();
  reduced.connections.clear();
  reduced.modules.pop_back();  // drop "sink"
  reduced.traffic_classes.pop_back();  // drop "child"

  auto applied = control_plane_->ApplyPipeline(reduced, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  EXPECT_EQ(after_apply + 1, applied->generation);
  EXPECT_TRUE(applied->workers_paused);

  const PipelineSnapshot snapshot = control_plane_->GetPipeline();
  ASSERT_EQ(1u, snapshot.modules.size());
  EXPECT_EQ("src", snapshot.modules[0].name);
  EXPECT_TRUE(snapshot.connections.empty());
}


// ---------------------------------------------------------------------------
// Failure injection: every injected failure must leave no trace
// ---------------------------------------------------------------------------

namespace {

// Everything a transaction must leave untouched when it fails.
struct Fingerprint {
  uint64_t generation = 0;
  size_t ports = 0;
  size_t modules = 0;
  size_t traffic_classes = 0;
  size_t workers = 0;
  size_t orphan_tcs = 0;
  PipelineSnapshot snapshot;
  std::vector<size_t> queue_users;  // one entry per port queue, must all be 0

  bool operator==(const Fingerprint &other) const {
    return generation == other.generation && ports == other.ports &&
           modules == other.modules &&
           traffic_classes == other.traffic_classes &&
           workers == other.workers && orphan_tcs == other.orphan_tcs &&
           snapshot == other.snapshot && queue_users == other.queue_users;
  }
};

Fingerprint Capture() {
  Fingerprint fingerprint;
  const bess::runtime::RuntimeState &state = bess::runtime::runtime();

  fingerprint.generation = state.generation();
  fingerprint.ports = state.ports().Size();
  fingerprint.modules = state.modules().Size();
  fingerprint.traffic_classes = state.traffic_classes().Size();
  fingerprint.workers = static_cast<size_t>(state.workers().num_workers());
  fingerprint.orphan_tcs = state.workers().orphan_tcs().size();
  fingerprint.snapshot = bess::control::SnapshotRuntime(state);

  for (const auto &pair : state.ports().All()) {
    const Port *port = pair.second.get();
    for (packet_dir_t dir : {PACKET_DIR_INC, PACKET_DIR_OUT}) {
      for (queue_t qid = 0; qid < port->num_queues[dir]; qid++) {
        fingerprint.queue_users.push_back(port->users[dir][qid] != nullptr);
      }
    }
  }

  return fingerprint;
}

}  // namespace

// Every operation of a real plan is injected in turn: each failure must leave
// the runtime exactly as it was, with no leaked module, port, traffic class,
// worker, orphan task or acquired queue.
TEST_F(ApplyPipelineTest, InjectedFailuresLeaveNoTrace) {
  PipelineSpec desired = FullPipeline();

  bess::control::ModuleSpec extra;
  extra.name = "extra";
  extra.mclass = "Bypass";
  desired.modules.push_back(extra);

  // Apply it once, so failures happen against a populated, active runtime.
  ASSERT_TRUE(control_plane_->ApplyPipeline(desired, {}).has_value());
  const Fingerprint active = Capture();
  ASSERT_GT(active.modules, 0u);
  ASSERT_GT(active.traffic_classes, 0u);

  // Now change it in a way that has to create a module, connect it and attach a
  // traffic class -- and fail at each operation of that plan in turn.
  PipelineSpec changed = desired;
  bess::control::ModuleSpec added;
  added.name = "added";
  added.mclass = "Bypass";
  changed.modules.push_back(added);

  bess::control::ConnectionSpec edge;
  edge.upstream = "added";
  edge.ogate = 0;
  edge.downstream = "src";
  edge.igate = 0;
  changed.connections.push_back(edge);

  bess::control::TrafficClassSpec tc;
  tc.name = "added_tc";
  tc.policy = "round_robin";
  tc.wid = 0;
  changed.traffic_classes.push_back(tc);

  auto plan = control_plane_->PlanPipeline(changed);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;

  struct Step {
    bess::control::TransactionPhase phase;
    size_t index;
    const char *phase_name;
  };
  std::vector<Step> steps;
  for (size_t i = 0; i < plan->prepare_ops.size(); i++) {
    steps.push_back({bess::control::TransactionPhase::kPrepare, i, "prepare"});
  }
  for (size_t i = 0; i < plan->commit_ops.size(); i++) {
    steps.push_back({bess::control::TransactionPhase::kCommit, i, "commit"});
  }
  ASSERT_GE(steps.size(), 3u)
      << "the plan should have several operations to inject into";

  for (const Step &step : steps) {
    size_t seen = 0;
    bess::control::SetFailureInjector(
        [&step, &seen](bess::control::TransactionPhase phase,
                       const bess::control::PlanOperation &) {
          if (phase == step.phase && seen++ == step.index) {
            return std::optional<bess::control::ControlError>(
                bess::control::Err(EIO, "injected failure"));
          }
          return std::optional<bess::control::ControlError>();
        });

    auto applied = control_plane_->ApplyPipeline(changed, {});
    bess::control::ClearFailureInjector();

    EXPECT_FALSE(applied.has_value())
        << "injection at " << step.phase_name << " op " << step.index
        << " did not fail the apply";
    EXPECT_TRUE(Capture() == active)
        << "injection at " << step.phase_name << " op " << step.index
        << " left a trace";
  }

  // The active pipeline is still usable afterwards.
  auto diff = control_plane_->DiffPipeline(desired);
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// Retirement is not undoable, so its preconditions are proven before the
// commit; if a step still fails, the caller is told -- an ordinary successful
// apply must never claim a pipeline that does not exist.
TEST_F(ApplyPipelineTest, InjectedRetireFailureIsReportedAsFailure) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(FullPipeline(), {}).has_value());
  const uint64_t generation = bess::runtime::runtime().generation();

  PipelineSpec reduced = FullPipeline();
  reduced.connections.clear();  // the edge into "sink" goes away with it
  reduced.modules.pop_back();   // "sink" has to be retired

  bess::control::SetFailureInjector(
      [](bess::control::TransactionPhase phase,
         const bess::control::PlanOperation &) {
        if (phase != bess::control::TransactionPhase::kRetire) {
          return std::optional<bess::control::ControlError>();
        }
        return std::optional<bess::control::ControlError>(
            bess::control::Err(EIO, "injected retire failure"));
      });
  auto applied = control_plane_->ApplyPipeline(reduced, {});
  bess::control::ClearFailureInjector();

  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(bess::control::ControlErrorCode::kResourceFailure,
            applied.error().code);
  EXPECT_NE(std::string::npos,
            applied.error().message.find("retirement failed"));

  // The new state *is* active -- the commit happened -- so the generation says
  // so, and the module that could not be retired is still there. The caller
  // knows, which is the difference from silently succeeding.
  EXPECT_EQ(generation + 1, bess::runtime::runtime().generation());
  EXPECT_TRUE(bess::runtime::runtime().modules().Contains("sink"));
}

// The invariant: a successful apply leaves exactly the desired state.
TEST_F(ApplyPipelineTest, SuccessfulApplyMatchesDesiredState) {
  auto expect_desired = [this](const PipelineSpec &desired) {
    const PipelineSpec running =
        SpecFromSnapshot(control_plane_->GetPipeline());
    PipelineSpec expected = desired;
    bess::control::Normalize(&expected);
    PipelineSpec observed = running;
    bess::control::Normalize(&observed);
    EXPECT_TRUE(observed == expected) << "active state differs from desired";
  };

  // Creation.
  PipelineSpec full = FullPipeline();
  ASSERT_TRUE(control_plane_->ApplyPipeline(full, {}).has_value());
  expect_desired(full);

  // Change: add a module and a connection.
  bess::control::ModuleSpec extra;
  extra.name = "extra";
  extra.mclass = "Bypass";
  full.modules.push_back(extra);
  bess::control::ConnectionSpec edge;
  edge.upstream = "sink";
  edge.ogate = 0;
  edge.downstream = "extra";
  edge.igate = 0;
  full.connections.push_back(edge);
  ASSERT_TRUE(control_plane_->ApplyPipeline(full, {}).has_value());
  expect_desired(full);

  // Removal.
  full.modules.pop_back();
  full.connections.pop_back();
  ASSERT_TRUE(control_plane_->ApplyPipeline(full, {}).has_value());
  expect_desired(full);
}

// Traffic-class parameters are part of desired state: a changed rate limit is a
// change, and applying it moves the runtime.
TEST_F(ApplyPipelineTest, RateLimitParameterChangesAreVisibleAndApplied) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec limiter;
  limiter.name = "limiter";
  limiter.policy = "rate_limit";
  limiter.resource = "bit";
  limiter.limit["bit"] = 1000000000;  // 1 Gbps
  limiter.max_burst["bit"] = 1000000;
  limiter.wid = 0;
  spec.traffic_classes.push_back(limiter);

  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "limiter";
  spec.traffic_classes.push_back(child);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  // Same pipeline, ten times slower: this must not be "unchanged".
  PipelineSpec slower = spec;
  slower.traffic_classes[0].limit["bit"] = 100000000;  // 100 Mbps

  auto diff = control_plane_->DiffPipeline(slower);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->traffic_classes.size());
  EXPECT_EQ(bess::control::ChangeKind::kUpdateParams,
            diff->traffic_classes[0].kind);
  EXPECT_EQ("limiter", diff->traffic_classes[0].name);

  auto applied = control_plane_->ApplyPipeline(slower, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  // The snapshot shows the new rate, and the runtime matches desired state.
  bool found = false;
  for (const auto &tc : control_plane_->GetPipeline().traffic_classes) {
    if (tc.name == "limiter") {
      found = true;
      EXPECT_EQ(100000000u, tc.limit);
    }
  }
  EXPECT_TRUE(found);

  const PipelineSpec running =
      SpecFromSnapshot(control_plane_->GetPipeline());
  auto settled = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(settled.has_value());
  EXPECT_TRUE(settled->empty());
}

// Attachment changes (priority, share) are reversible: detach and reattach.
TEST_F(ApplyPipelineTest, AttachmentChangesAreReversibleUpdates) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec priority;
  priority.name = "prio";
  priority.policy = "priority";
  priority.wid = 0;
  spec.traffic_classes.push_back(priority);

  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "prio";
  child.has_priority = true;
  child.priority = 10;
  spec.traffic_classes.push_back(child);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  PipelineSpec reprioritised = spec;
  reprioritised.traffic_classes[1].priority = 20;

  auto diff = control_plane_->DiffPipeline(reprioritised);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->traffic_classes.size());
  EXPECT_EQ(bess::control::ChangeKind::kUpdate, diff->traffic_classes[0].kind);

  auto applied = control_plane_->ApplyPipeline(reprioritised, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  const PipelineSpec running =
      SpecFromSnapshot(control_plane_->GetPipeline());
  auto settled = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(settled.has_value());
  EXPECT_TRUE(settled->empty());
}

// A different policy is a different class: refused, not silently ignored.
TEST_F(ApplyPipelineTest, PolicyChangeIsRefusedTransactionally) {
  // A weighted-fair parent with a round-robin child, so that the child can
  // legitimately carry a share; the change is the child's policy.
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();
  bess::control::TrafficClassSpec parent;
  parent.name = "wparent";
  parent.policy = "weighted_fair";
  parent.resource = "packet";
  parent.wid = 0;
  spec.traffic_classes.push_back(parent);
  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "wparent";
  child.has_share = true;
  child.share = 3;
  spec.traffic_classes.push_back(child);
  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());
  const uint64_t generation = bess::runtime::runtime().generation();

  PipelineSpec changed = spec;
  for (auto &tc : changed.traffic_classes) {
    if (tc.name == "child") {
      tc.policy = "rate_limit";
      tc.resource = "packet";
      tc.limit["packet"] = 1000;
      tc.max_burst["packet"] = 100;
    }
  }

  auto diff = control_plane_->DiffPipeline(changed);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->traffic_classes.size());
  EXPECT_EQ(bess::control::ChangeKind::kReplace, diff->traffic_classes[0].kind);

  auto applied = control_plane_->ApplyPipeline(changed, {});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(bess::control::ControlErrorCode::kUnsupportedTransaction,
            applied.error().code);
  EXPECT_EQ(generation, bess::runtime::runtime().generation());
}

// A plan whose retirement cannot be guaranteed is refused up front: the port is
// still in use by a module the plan keeps.
TEST_F(ApplyPipelineTest, RetirementPreconditionsAreProvenUpFront) {
  PipelineSpec spec = FullPipeline();

  bess::pb::PMDPortArg port_arg;
  port_arg.set_vdev("net_null0");
  bess::control::PortSpec port;
  port.name = "p0";
  port.driver = "PMDPort";
  port.num_rx_queues = 1;
  port.num_tx_queues = 1;
  port.rx_queue_size = 1024;
  port.tx_queue_size = 1024;
  EXPECT_TRUE(port.arg.PackFrom(port_arg));
  spec.ports.push_back(port);

  bess::pb::QueueIncArg queue_arg;
  queue_arg.set_port("p0");
  queue_arg.set_qid(0);
  bess::control::ModuleSpec reader;
  reader.name = "reader";
  reader.mclass = "QueueInc";
  EXPECT_TRUE(reader.arg.PackFrom(queue_arg));
  spec.modules.push_back(reader);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  // Keep the module, drop the port it reads from: the port cannot be retired
  // while the module holds its queues, so the plan must be refused before
  // anything happens.
  PipelineSpec without_port = spec;
  without_port.ports.clear();

  const uint64_t generation = bess::runtime::runtime().generation();
  const PipelineSnapshot active = control_plane_->GetPipeline();

  auto applied = control_plane_->ApplyPipeline(without_port, {});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(bess::control::ControlErrorCode::kUnsupportedTransaction,
            applied.error().code);
  EXPECT_EQ("port", applied.error().object);
  EXPECT_NE(std::string::npos, applied.error().message.find("still in use"));
  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  EXPECT_TRUE(control_plane_->GetPipeline() == active);
}

// The acceptance test for the reparent ownership split: a refused move must
// not lose the class, its siblings, or their priorities.
TEST_F(ApplyPipelineTest, RefusedReparentKeepsEveryClassAndPlacement) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec parent;
  parent.name = "prio";
  parent.policy = "priority";
  parent.wid = 0;
  spec.traffic_classes.push_back(parent);

  bess::control::TrafficClassSpec child_a;
  child_a.name = "child_a";
  child_a.policy = "round_robin";
  child_a.parent = "prio";
  child_a.has_priority = true;
  child_a.priority = 10;
  spec.traffic_classes.push_back(child_a);

  bess::control::TrafficClassSpec child_b;
  child_b.name = "child_b";
  child_b.policy = "round_robin";
  child_b.parent = "prio";
  child_b.has_priority = true;
  child_b.priority = 20;
  spec.traffic_classes.push_back(child_b);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  const PipelineSnapshot active = control_plane_->GetPipeline();
  const uint64_t generation = bess::runtime::runtime().generation();
  const size_t tcs = bess::runtime::runtime().traffic_classes().Size();

  // child_a would collide with child_b's priority: refused.
  PipelineSpec collision = spec;
  collision.traffic_classes[1].priority = 20;

  auto applied = control_plane_->ApplyPipeline(collision, {});
  ASSERT_FALSE(applied.has_value());

  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  EXPECT_EQ(tcs, bess::runtime::runtime().traffic_classes().Size());
  EXPECT_TRUE(control_plane_->GetPipeline() == active);
  EXPECT_TRUE(bess::runtime::runtime().traffic_classes().Contains("child_a"));
  EXPECT_TRUE(bess::runtime::runtime().traffic_classes().Contains("child_b"));

  // Both children still hold their original priorities.
  for (const auto &tc : control_plane_->GetPipeline().traffic_classes) {
    if (tc.name == "child_a") {
      EXPECT_TRUE(tc.has_priority);
      EXPECT_EQ(10, tc.priority);
    }
    if (tc.name == "child_b") {
      EXPECT_TRUE(tc.has_priority);
      EXPECT_EQ(20, tc.priority);
    }
  }

  // And the pipeline still diffs clean against its own description.
  auto diff = control_plane_->DiffPipeline(spec);
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// An attach failure that validation cannot see (the parent's current child is
// only removed in the same transaction's retire phase) must roll the whole
// transaction back, including the class that was created for it.
TEST_F(ApplyPipelineTest, AttachFailureDuringCommitRollsBackCleanly) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec limiter;
  limiter.name = "limiter";
  limiter.policy = "rate_limit";
  limiter.resource = "bit";
  limiter.limit["bit"] = 1000000;
  limiter.max_burst["bit"] = 1000;
  limiter.wid = 0;
  spec.traffic_classes.push_back(limiter);

  bess::control::TrafficClassSpec old_child;
  old_child.name = "old_child";
  old_child.policy = "round_robin";
  old_child.parent = "limiter";
  spec.traffic_classes.push_back(old_child);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  const PipelineSnapshot active = control_plane_->GetPipeline();
  const uint64_t generation = bess::runtime::runtime().generation();
  const size_t tcs = bess::runtime::runtime().traffic_classes().Size();

  // Rename the child: the new class is created during the commit, while the old
  // one is only retired afterwards, so the rate limiter still holds its child
  // and the attach fails.
  PipelineSpec renamed = spec;
  renamed.traffic_classes[1].name = "new_child";

  auto applied = control_plane_->ApplyPipeline(renamed, {});
  ASSERT_FALSE(applied.has_value());

  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  EXPECT_EQ(tcs, bess::runtime::runtime().traffic_classes().Size());
  EXPECT_TRUE(bess::runtime::runtime().traffic_classes().Contains("old_child"));
  EXPECT_FALSE(bess::runtime::runtime().traffic_classes().Contains("new_child"));
  EXPECT_TRUE(control_plane_->GetPipeline() == active);
}

// Weighted-fair shares must be positive; validation refuses zero up front.
TEST_F(ApplyPipelineTest, ZeroShareIsRejectedBeforeAnyChange) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec parent;
  parent.name = "wparent";
  parent.policy = "weighted_fair";
  parent.resource = "packet";
  parent.wid = 0;
  spec.traffic_classes.push_back(parent);

  bess::control::TrafficClassSpec child;
  child.name = "child";
  child.policy = "round_robin";
  child.parent = "wparent";
  child.has_share = true;
  child.share = 0;
  spec.traffic_classes.push_back(child);

  const uint64_t generation = bess::runtime::runtime().generation();
  auto applied = control_plane_->ApplyPipeline(spec, {});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(EINVAL, applied.error().err);
  EXPECT_EQ("share", applied.error().field);
  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  EXPECT_TRUE(bess::runtime::runtime().traffic_classes().Empty());
}

// Pause duration is observable from the start.
TEST_F(ApplyPipelineTest, RecordsPhaseTimings) {
  auto applied = control_plane_->ApplyPipeline(FullPipeline(), {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  EXPECT_TRUE(applied->workers_paused);
  EXPECT_GE(applied->timing.validation_us, 0u);
  EXPECT_GE(applied->timing.prepare_us, 0u);
  EXPECT_GE(applied->timing.paused_commit_us, 0u);
  EXPECT_GE(applied->timing.retire_us, 0u);
}


// The legacy `UpdateTcParent` used to snapshot the old attachment *after*
// detaching the class, so a refused move restored it as a root/orphan instead
// of putting it back under its parent with its priority or share. It now
// snapshots first, which is what this pins down.
TEST_F(ApplyPipelineTest, LegacyReparentRestoresTheOriginalAttachment) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  // A port and a module with a task, so there is a real leaf traffic class.
  bess::pb::PMDPortArg port_arg;
  port_arg.set_vdev("net_null1");
  bess::control::PortSpec port;
  port.name = "p0";
  port.driver = "PMDPort";
  port.num_rx_queues = 1;
  port.num_tx_queues = 1;
  port.rx_queue_size = 1024;
  port.tx_queue_size = 1024;
  EXPECT_TRUE(port.arg.PackFrom(port_arg));
  spec.ports.push_back(port);

  bess::pb::QueueIncArg queue_arg;
  queue_arg.set_port("p0");
  queue_arg.set_qid(0);
  bess::control::ModuleSpec reader;
  reader.name = "reader";
  reader.mclass = "QueueInc";
  EXPECT_TRUE(reader.arg.PackFrom(queue_arg));
  spec.modules.push_back(reader);

  // A round-robin home for the leaf, and a priority class whose slot is taken.
  bess::control::TrafficClassSpec home;
  home.name = "home";
  home.policy = "round_robin";
  home.wid = 0;
  spec.traffic_classes.push_back(home);

  bess::control::TrafficClassSpec prio;
  prio.name = "prio";
  prio.policy = "priority";
  prio.wid = 0;
  spec.traffic_classes.push_back(prio);

  bess::control::TrafficClassSpec holder;
  holder.name = "holder";
  holder.policy = "round_robin";
  holder.parent = "prio";
  holder.has_priority = true;
  holder.priority = 10;
  spec.traffic_classes.push_back(holder);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  // Find the leaf the module created, and give it a known home through the
  // legacy path (this one is expected to succeed).
  const bess::control::PipelineSnapshot after_apply =
      control_plane_->GetPipeline();
  std::string leaf_name;
  for (const auto &tc : after_apply.traffic_classes) {
    if (tc.policy == "leaf" && tc.leaf_module_name == "reader") {
      leaf_name = tc.name;
    }
  }
  ASSERT_FALSE(leaf_name.empty());

  bess::control::TrafficClassSpec move;
  move.leaf_module_name = "reader";
  move.leaf_module_taskid = 0;
  move.parent = "home";
  ASSERT_TRUE(control_plane_->UpdateTcParent(move).has_value());

  const uint64_t generation = bess::runtime::runtime().generation();
  {
    const bess::control::PipelineSnapshot moved = control_plane_->GetPipeline();
    bool homed = false;
    for (const auto &tc : moved.traffic_classes) {
      if (tc.name == leaf_name) {
        homed = tc.parent == "home";
      }
    }
    ASSERT_TRUE(homed) << "the leaf should be under 'home'";
  }

  // Now ask for a move that must be refused: priority 10 is already taken.
  bess::control::TrafficClassSpec collision = move;
  collision.parent = "prio";
  collision.has_priority = true;
  collision.priority = 10;

  auto refused = control_plane_->UpdateTcParent(collision);
  ASSERT_FALSE(refused.has_value());

  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  EXPECT_TRUE(bess::runtime::runtime().traffic_classes().Contains(leaf_name));

  // The class went back where it was, not to the orphan list.
  bool restored = false;
  for (const auto &tc : control_plane_->GetPipeline().traffic_classes) {
    if (tc.name == leaf_name) {
      restored = tc.parent == "home";
    }
  }
  EXPECT_TRUE(restored) << "the leaf should still be under 'home'";

  const PipelineSpec running =
      SpecFromSnapshot(control_plane_->GetPipeline());
  auto diff = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// The legacy rule for non-leaf classes is stricter: they may only move as
// orphans. A refusal there must leave everything exactly as it was.
TEST_F(ApplyPipelineTest, LegacyReparentRefusesToMoveAnAttachedRoot) {
  PipelineSpec spec = FullPipeline();
  spec.traffic_classes.clear();

  bess::control::TrafficClassSpec root;
  root.name = "solo";
  root.policy = "round_robin";
  root.wid = 0;
  spec.traffic_classes.push_back(root);

  bess::control::TrafficClassSpec prio;
  prio.name = "prio";
  prio.policy = "priority";
  prio.wid = 0;
  spec.traffic_classes.push_back(prio);

  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  const PipelineSnapshot active = control_plane_->GetPipeline();
  const uint64_t generation = bess::runtime::runtime().generation();

  bess::control::TrafficClassSpec move;
  move.name = "solo";
  move.parent = "prio";
  move.has_priority = true;
  move.priority = 5;  // free: the move would succeed if it were allowed

  auto refused = control_plane_->UpdateTcParent(move);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(EINVAL, refused.error().err);

  EXPECT_EQ(generation, bess::runtime::runtime().generation());
  EXPECT_TRUE(control_plane_->GetPipeline() == active);
  EXPECT_TRUE(bess::runtime::runtime().traffic_classes().Contains("solo"));
}

// Two Source modules (one task each) into a Sink, two rate-limited classes on
// worker 0, and src0's task under rl0: what attach_task sets up, as desired
// state.
PipelineSpec PlacedTasks() {
  PipelineSpec spec;
  spec.workers.push_back({0, 0, ""});
  for (const char *name : {"src0", "src1"}) {
    spec.modules.push_back({name, "Source", {}});
    spec.connections.push_back({name, 0, "sink", 0, false});
  }
  spec.modules.push_back({"sink", "Sink", {}});
  for (const char *name : {"rl0", "rl1"}) {
    bess::control::TrafficClassSpec tc;
    tc.name = name;
    tc.policy = "rate_limit";
    tc.resource = "packet";
    tc.wid = 0;
    tc.limit["packet"] = 1000;
    spec.traffic_classes.push_back(tc);
  }
  bess::control::TaskPlacementSpec task;
  task.module = "src0";
  task.parent = "rl0";
  spec.tasks.push_back(task);
  return spec;
}

const bess::control::TaskPlacementSpec *PlacementOf(const PipelineSpec &spec,
                                                    const std::string &module) {
  for (const auto &task : spec.tasks) {
    if (task.module == module) {
      return &task;
    }
  }
  return nullptr;
}

// Where a task is scheduled is desired state: applied, read back by
// GetPipeline, a no-op to reapply, and moved by a changed placement. A task
// desired state does not name stays where it is.
TEST_F(ApplyPipelineTest, TaskPlacementIsDesiredState) {
  PipelineSpec spec = PlacedTasks();
  auto applied = control_plane_->ApplyPipeline(spec, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;

  PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  const auto *placed = PlacementOf(running, "src0");
  ASSERT_NE(nullptr, placed) << "GetPipeline lost the task's placement";
  EXPECT_EQ("rl0", placed->parent);
  auto diff = control_plane_->DiffPipeline(spec);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  EXPECT_TRUE(diff->empty()) << diff->tasks.size() << " task change(s)";
  // What GetPipeline printed applies as a no-op too.
  auto again = control_plane_->DiffPipeline(running);
  ASSERT_TRUE(again.has_value()) << again.error().message;
  EXPECT_TRUE(again->empty());

  // src0 moved under rl1; src1 placed at worker 0's root, where a task no
  // one placed already went (a single worker): no change for it.
  PipelineSpec moved = spec;
  moved.tasks[0].parent = "rl1";
  bess::control::TaskPlacementSpec root;
  root.module = "src1";
  root.wid = 0;
  moved.tasks.push_back(root);
  diff = control_plane_->DiffPipeline(moved);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  ASSERT_EQ(1u, diff->tasks.size());
  EXPECT_EQ("src0", diff->tasks[0].desired.module);
  applied = control_plane_->ApplyPipeline(moved, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  running = SpecFromSnapshot(control_plane_->GetPipeline());
  ASSERT_NE(nullptr, PlacementOf(running, "src0"));
  EXPECT_EQ("rl1", PlacementOf(running, "src0")->parent);
  ASSERT_NE(nullptr, PlacementOf(running, "src1"));
  EXPECT_EQ("", PlacementOf(running, "src1")->parent);
  EXPECT_EQ(0, PlacementOf(running, "src1")->wid);
  diff = control_plane_->DiffPipeline(moved);
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());

  // Not named: left where it is.
  PipelineSpec unnamed = moved;
  unnamed.tasks.clear();
  diff = control_plane_->DiffPipeline(unnamed);
  ASSERT_TRUE(diff.has_value());
  EXPECT_TRUE(diff->empty());
}

// Placements are applied together: two tasks may trade places even where a
// class takes one child (rate_limit), which one move at a time cannot do.
TEST_F(ApplyPipelineTest, TasksTradePlaces) {
  PipelineSpec spec = PlacedTasks();
  bess::control::TaskPlacementSpec second;
  second.module = "src1";
  second.parent = "rl1";
  spec.tasks.push_back(second);
  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  PipelineSpec swapped = spec;
  swapped.tasks[0].parent = "rl1";
  swapped.tasks[1].parent = "rl0";
  auto applied = control_plane_->ApplyPipeline(swapped, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  ASSERT_NE(nullptr, PlacementOf(running, "src0"));
  ASSERT_NE(nullptr, PlacementOf(running, "src1"));
  EXPECT_EQ("rl1", PlacementOf(running, "src0")->parent);
  EXPECT_EQ("rl0", PlacementOf(running, "src1")->parent);
}

// The placement is the last commit operation, but not the last step that can
// fail: the resource convergence runs after the commit and aborts the
// transaction. Its undo has to put the tasks back -- without one the placement
// stays while the rest of the commit rolls back, and the runtime matches
// neither the old spec nor the new one.
TEST_F(ApplyPipelineTest, ResourceFailureAfterCommitRestoresThePlacement) {
  PipelineSpec spec = PlacedTasks();
  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());
  const Fingerprint before = Capture();

  // src0 moves from rl0 to rl1, so the plan ends with a placement.
  PipelineSpec moved = spec;
  moved.tasks[0].parent = "rl1";
  auto plan = control_plane_->PlanPipeline(moved);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  ASSERT_FALSE(plan->commit_ops.empty());
  ASSERT_TRUE(std::holds_alternative<bess::control::PlaceTasksOp>(
      plan->commit_ops.back()))
      << "the plan should end with the task placement";

  bess::control::SetFailureInjector(
      [](bess::control::TransactionPhase phase,
         const bess::control::PlanOperation &) {
        if (phase != bess::control::TransactionPhase::kResources) {
          return std::optional<bess::control::ControlError>();
        }
        return std::optional<bess::control::ControlError>(
            bess::control::Err(EIO, "injected resource failure"));
      });
  auto applied = control_plane_->ApplyPipeline(moved, {});
  bess::control::ClearFailureInjector();

  ASSERT_FALSE(applied.has_value());
  EXPECT_TRUE(Capture() == before) << "the abort left the placement behind";
  auto diff = control_plane_->DiffPipeline(spec);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  EXPECT_TRUE(diff->empty()) << "the runtime no longer matches the old spec";

  // With the failure gone the same apply lands, placement included.
  applied = control_plane_->ApplyPipeline(moved, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  ASSERT_NE(nullptr, PlacementOf(running, "src0"));
  EXPECT_EQ("rl1", PlacementOf(running, "src0")->parent);
}

// A placement the scheduler refuses at commit (the class is full with a task
// desired state does not name) moves no task: those already moved go back.
TEST_F(ApplyPipelineTest, RefusedPlacementMovesNoTask) {
  PipelineSpec spec = PlacedTasks();  // src0 under rl0
  spec.modules.push_back({"src2", "Source", {}});
  spec.connections.push_back({"src2", 0, "sink", 0, false});
  bess::control::TaskPlacementSpec occupant;
  occupant.module = "src1";
  occupant.parent = "rl1";
  spec.tasks.push_back(occupant);
  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());
  const PipelineSnapshot active = control_plane_->GetPipeline();

  // src0 to worker 0's root (fine), then src2 into rl1, which src1 still
  // holds: desired state no longer names src1, so only the scheduler knows.
  PipelineSpec crowded = spec;
  crowded.tasks.clear();
  bess::control::TaskPlacementSpec root;
  root.module = "src0";
  root.wid = 0;
  crowded.tasks.push_back(root);
  bess::control::TaskPlacementSpec intruder;
  intruder.module = "src2";
  intruder.parent = "rl1";
  crowded.tasks.push_back(intruder);
  auto applied = control_plane_->ApplyPipeline(crowded, {});
  ASSERT_FALSE(applied.has_value()) << "rl1 took a second child";

  EXPECT_TRUE(control_plane_->GetPipeline() == active);
  const PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  ASSERT_NE(nullptr, PlacementOf(running, "src0"));
  EXPECT_EQ("rl0", PlacementOf(running, "src0")->parent) << "src0 stayed moved";
}

// Removing classes together with the modules whose tasks they hold: deleting
// a class deletes the leaves under it, so the modules go first.
TEST_F(ApplyPipelineTest, ClassesGoWithTheTasksTheyHold) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(PlacedTasks(), {}).has_value());

  PipelineSpec other;
  other.workers.push_back({0, 0, ""});
  other.modules.push_back({"src9", "Source", {}});
  other.modules.push_back({"sink9", "Sink", {}});
  other.connections.push_back({"src9", 0, "sink9", 0, false});
  auto applied = control_plane_->ApplyPipeline(other, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  EXPECT_TRUE(running.traffic_classes.empty());
  ASSERT_EQ(1u, running.tasks.size());
  EXPECT_EQ("src9", running.tasks[0].module);
}

// A class that would still hold the task of a module the plan keeps (and
// does not place elsewhere) is not removed: the plan is refused unchanged.
TEST_F(ApplyPipelineTest, AClassHoldingAKeptTaskStays) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(PlacedTasks(), {}).has_value());
  const PipelineSnapshot active = control_plane_->GetPipeline();

  PipelineSpec without = PlacedTasks();
  std::erase_if(without.traffic_classes, [](const auto &tc) { return tc.name == "rl0"; });
  without.tasks.clear();  // src0 is not placed: it would stay under rl0
  auto applied = control_plane_->ApplyPipeline(without, {});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ("transaction", applied.error().field) << applied.error().message;
  EXPECT_NE(std::string::npos, applied.error().message.find("src0"))
      << applied.error().message;
  EXPECT_TRUE(control_plane_->GetPipeline() == active);

  // Placed elsewhere, the class goes.
  bess::control::TaskPlacementSpec root;
  root.module = "src0";
  root.wid = 0;
  without.tasks.push_back(root);
  applied = control_plane_->ApplyPipeline(without, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
}

// A class holding a task may move out of a class the plan removes: the
// task goes with it, so the removal deletes nothing it should not.
TEST_F(ApplyPipelineTest, AClassMovesOutOfARemovedOneWithItsTask) {
  PipelineSpec spec = PlacedTasks();  // src0 under rl0
  bess::control::TrafficClassSpec outer;
  outer.name = "outer";
  outer.policy = "round_robin";
  outer.wid = 0;
  spec.traffic_classes.push_back(outer);
  for (auto &tc : spec.traffic_classes) {
    if (tc.name == "rl0") {
      tc.parent = "outer";
      tc.wid = -1;
    }
  }
  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  PipelineSpec next = spec;
  std::erase_if(next.traffic_classes, [](const auto &tc) { return tc.name == "outer"; });
  for (auto &tc : next.traffic_classes) {
    if (tc.name == "rl0") {
      tc.parent.clear();
      tc.wid = 0;
    }
  }
  auto applied = control_plane_->ApplyPipeline(next, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  ASSERT_NE(nullptr, PlacementOf(running, "src0"));
  EXPECT_EQ("rl0", PlacementOf(running, "src0")->parent);
}

// A task the plan moves off a worker it removes does not keep that worker.
TEST_F(ApplyPipelineTest, ATaskMovesOffARemovedWorker) {
  const std::vector<int> cpus = TwoUsableCpus();
  if (cpus.size() < 2) {
    GTEST_SKIP() << "needs two CPUs a worker spec may name";
  }
  PipelineSpec two;
  two.workers.push_back({0, cpus[0], ""});
  two.workers.push_back({1, cpus[1], ""});
  two.modules.push_back({"src0", "Source", {}});
  two.modules.push_back({"sink", "Sink", {}});
  two.connections.push_back({"src0", 0, "sink", 0, false});
  bess::control::TaskPlacementSpec on_one;
  on_one.module = "src0";
  on_one.wid = 1;
  two.tasks.push_back(on_one);
  auto two_applied = control_plane_->ApplyPipeline(two, {});
  ASSERT_TRUE(two_applied.has_value()) << two_applied.error().message;

  PipelineSpec one = two;
  one.workers.pop_back();
  one.tasks[0].wid = 0;
  auto applied = control_plane_->ApplyPipeline(one, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  ASSERT_EQ(1u, running.workers.size());
  ASSERT_NE(nullptr, PlacementOf(running, "src0"));
  EXPECT_EQ(0, PlacementOf(running, "src0")->wid);
}

// Tasks of new modules no one placed get a worker that stays: not one the
// same plan removes (the retire phase would then refuse to remove it), also
// when the plan's own steps pause workers on the way (a disconnect does).
TEST_F(ApplyPipelineTest, NewTasksAvoidARemovedWorker) {
  const std::vector<int> cpus = TwoUsableCpus();
  if (cpus.size() < 2) {
    GTEST_SKIP() << "needs two CPUs a worker spec may name";
  }
  PipelineSpec two;
  two.workers.push_back({0, cpus[0], ""});
  two.workers.push_back({1, cpus[1], ""});
  two.modules.push_back({"sink", "Sink", {}});
  two.modules.push_back({"old", "Bypass", {}});
  two.connections.push_back({"old", 0, "sink", 0, false});
  auto two_applied = control_plane_->ApplyPipeline(two, {});
  ASSERT_TRUE(two_applied.has_value()) << two_applied.error().message;

  PipelineSpec one = two;
  one.workers.pop_back();
  one.modules.pop_back();
  one.connections.clear();
  for (const char *name : {"new0", "new1", "new2"}) {  // round robin reaches 1
    one.modules.push_back({name, "Source", {}});
    one.connections.push_back({name, 0, "sink", 0, false});
  }
  auto applied = control_plane_->ApplyPipeline(one, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  ASSERT_EQ(1u, running.workers.size());
  for (const char *name : {"new0", "new1", "new2"}) {
    ASSERT_NE(nullptr, PlacementOf(running, name)) << name;
    EXPECT_EQ(0, PlacementOf(running, name)->wid) << name;
  }
}

// A task takes the place of a removed module's task, even in a class that
// takes one child (rate_limit): the leaving task makes room first.
TEST_F(ApplyPipelineTest, ATaskTakesALeavingTasksPlace) {
  ASSERT_TRUE(control_plane_->ApplyPipeline(PlacedTasks(), {}).has_value());

  PipelineSpec next = PlacedTasks();  // src0 under rl0
  std::erase_if(next.modules, [](const auto &m) { return m.name == "src0"; });
  std::erase_if(next.connections, [](const auto &c) { return c.upstream == "src0"; });
  next.modules.push_back({"src5", "Source", {}});
  next.connections.push_back({"src5", 0, "sink", 0, false});
  next.tasks[0].module = "src5";
  auto applied = control_plane_->ApplyPipeline(next, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  const PipelineSpec running = SpecFromSnapshot(control_plane_->GetPipeline());
  ASSERT_NE(nullptr, PlacementOf(running, "src5"));
  EXPECT_EQ("rl0", PlacementOf(running, "src5")->parent);
  EXPECT_EQ(nullptr, PlacementOf(running, "src0"));
}

// A class that moves and changes its limit gets both.
TEST_F(ApplyPipelineTest, AMovedClassKeepsItsNewLimit) {
  PipelineSpec spec = PlacedTasks();
  bess::control::TrafficClassSpec root;
  root.name = "top";
  root.policy = "round_robin";
  root.wid = 0;
  spec.traffic_classes.push_back(root);
  ASSERT_TRUE(control_plane_->ApplyPipeline(spec, {}).has_value());

  for (auto &tc : spec.traffic_classes) {
    if (tc.name == "rl0") {
      tc.parent = "top";
      tc.wid = -1;
      tc.limit["packet"] = 5000;
    }
  }
  auto applied = control_plane_->ApplyPipeline(spec, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  auto diff = control_plane_->DiffPipeline(spec);
  ASSERT_TRUE(diff.has_value()) << diff.error().message;
  EXPECT_TRUE(diff->empty()) << "the limit was left behind";
}

// A nested tree goes children first, whatever the names sort to: deleting a
// class deletes the classes under it, which must not be deleted again.
TEST_F(ApplyPipelineTest, ANestedTreeGoesChildrenFirst) {
  PipelineSpec tree;
  tree.workers.push_back({0, 0, ""});
  std::string parent;
  for (const char *name : {"a", "b", "c"}) {  // a over b over c
    bess::control::TrafficClassSpec tc;
    tc.name = name;
    tc.policy = "round_robin";
    tc.parent = parent;
    tc.wid = parent.empty() ? 0 : -1;
    tree.traffic_classes.push_back(tc);
    parent = name;
  }
  ASSERT_TRUE(control_plane_->ApplyPipeline(tree, {}).has_value());

  PipelineSpec bare;
  bare.workers.push_back({0, 0, ""});
  auto plan = control_plane_->PlanPipeline(bare);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  std::string order;
  for (const auto &op : plan->retire_ops) {
    if (const auto *remove = std::get_if<bess::control::RemoveTcOp>(&op)) {
      order += remove->name;
    }
  }
  EXPECT_EQ("cba", order);

  auto applied = control_plane_->ApplyPipeline(bare, {});
  ASSERT_TRUE(applied.has_value()) << applied.error().message;
  EXPECT_TRUE(SpecFromSnapshot(control_plane_->GetPipeline()).traffic_classes.empty());
}

// Impossible placements are refused before anything changes.
TEST_F(ApplyPipelineTest, ImpossibleTaskPlacementsAreRefused) {
  struct Case {
    const char *what;
    std::function<void(PipelineSpec &)> change;
    const char *field;
  };
  const Case cases[] = {
      {"no module", [](PipelineSpec &s) { s.tasks[0].module = "nope"; }, "module"},
      {"no parent class", [](PipelineSpec &s) { s.tasks[0].parent = "nope"; }, "parent"},
      {"neither parent nor wid", [](PipelineSpec &s) { s.tasks[0].parent = ""; }, "wid"},
      {"both", [](PipelineSpec &s) { s.tasks[0].wid = 0; }, "wid"},
      {"twice", [](PipelineSpec &s) { s.tasks.push_back(s.tasks[0]); }, "task"},
      {"task index", [](PipelineSpec &s) { s.tasks[0].task = MAX_TASKS_PER_MODULE; }, "task"},
      {"a priority under rate_limit",
       [](PipelineSpec &s) {
         s.tasks[0].has_priority = true;
         s.tasks[0].priority = 1;
       },
       "parent"},
      // A rate_limit class takes one child, tasks counted.
      {"second child of rate_limit",
       [](PipelineSpec &s) {
         bess::control::TaskPlacementSpec other = s.tasks[0];
         other.module = "src1";
         s.tasks.push_back(other);
       },
       "parent"},
  };
  for (const Case &c : cases) {
    PipelineSpec spec = PlacedTasks();
    c.change(spec);
    auto applied = control_plane_->ApplyPipeline(spec, {});
    ASSERT_FALSE(applied.has_value()) << c.what;
    EXPECT_EQ(c.field, applied.error().field) << c.what << ": " << applied.error().message;
    EXPECT_TRUE(bess::runtime::runtime().modules().All().empty()) << c.what;
  }
}

// -- resources as desired state (D-129) ------------------------------------------

google::protobuf::Any Packed(const google::protobuf::Message &m) {
  google::protobuf::Any any;
  EXPECT_TRUE(any.PackFrom(m));
  return any;
}

// An ExactMatch module on 4 bytes at offset 26 (IPv4 source), and its
// resources: `rules` maps each address in `rules` to gate 1; `settings` is the
// default gate when set.
PipelineSpec ExactMatchPipeline(const std::vector<std::string> &rules,
                                std::optional<uint64_t> default_gate) {
  PipelineSpec spec;
  bess::pb::ExactMatchArg arg;
  bess::pb::Field *field = arg.add_fields();
  field->set_offset(26);
  field->set_num_bytes(4);
  spec.modules.push_back({"em", "ExactMatch", Packed(arg)});
  bess::control::ResourceSpec rules_spec{"em/rules", {}};
  for (const std::string &addr : rules) {
    bess::pb::ExactMatchRuleKey key;
    key.add_fields()->set_value_bin(addr);
    bess::pb::ExactMatchRuleValue value;
    value.set_gate(1);
    rules_spec.entries.push_back({Packed(key), Packed(value)});
  }
  spec.resources.push_back(std::move(rules_spec));
  if (default_gate) {
    bess::pb::ExactMatchSettings settings;
    settings.set_default_gate(*default_gate);
    spec.resources.push_back({"em/settings", {{google::protobuf::Any(), Packed(settings)}}});
  }
  return spec;
}

// The contents of `name` as GetPipeline reads them.
std::optional<bess::control::ResourceSpec> ReadResource(ControlPlane &cp, const std::string &name) {
  auto read = cp.ReadPipeline({.include = true, .names = {name}});
  if (!read || read->value.resources.size() != 1) {
    return std::nullopt;
  }
  return read->value.resources[0];
}

uint64_t DefaultGate(ControlPlane &cp) {
  auto settings = ReadResource(cp, "em/settings");
  bess::pb::ExactMatchSettings value;
  if (!settings || settings->entries.size() != 1 ||
      !settings->entries[0].value.UnpackTo(&value)) {
    ADD_FAILURE() << "em/settings unreadable";
    return 0;
  }
  return value.default_gate();
}

// Listed resources converge to exactly their entries in the same apply as the
// topology; a listed resource with no entries is cleared; an unlisted one is
// left alone; and a resource-only change moves the resource generation, not
// the pipeline's.
TEST_F(ApplyPipelineTest, ListedResourcesConvergeAndUnlistedOnesStay) {
  const std::string a("\x0a\x00\x00\x01", 4);
  const std::string b("\x0a\x00\x00\x02", 4);
  auto applied = control_plane_->ApplyPipeline(ExactMatchPipeline({a, b}, 3), {});
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_EQ(3u, applied->resource_ops);
  auto rules = ReadResource(*control_plane_, "em/rules");
  ASSERT_TRUE(rules);
  EXPECT_EQ(2u, rules->entries.size());
  EXPECT_EQ(3u, DefaultGate(*control_plane_));

  // Same desired state: nothing to do.
  auto again = control_plane_->ApplyPipeline(ExactMatchPipeline({a, b}, 3), {});
  ASSERT_TRUE(again) << again.error().message;
  EXPECT_EQ(0u, again->resource_ops);
  EXPECT_EQ(0u, again->applied_ops);
  EXPECT_EQ(applied->generation, again->generation);
  EXPECT_EQ(applied->resource_generation, again->resource_generation);

  // One rule replaced, settings not listed: only the rules move.
  const std::string c("\x0a\x00\x00\x03", 4);
  auto changed = control_plane_->ApplyPipeline(ExactMatchPipeline({a, c}, std::nullopt), {});
  ASSERT_TRUE(changed) << changed.error().message;
  EXPECT_EQ(2u, changed->resource_ops) << "b erased, c created";
  EXPECT_EQ(applied->generation, changed->generation) << "a resource-only change";
  EXPECT_LT(applied->resource_generation, changed->resource_generation);
  EXPECT_EQ(3u, DefaultGate(*control_plane_)) << "an unlisted resource is untouched";

  // Listed with no entries: cleared.
  auto cleared = control_plane_->ApplyPipeline(ExactMatchPipeline({}, std::nullopt), {});
  ASSERT_TRUE(cleared) << cleared.error().message;
  EXPECT_EQ(2u, cleared->resource_ops);
  rules = ReadResource(*control_plane_, "em/rules");
  ASSERT_TRUE(rules);
  EXPECT_TRUE(rules->entries.empty());
  EXPECT_EQ(3u, DefaultGate(*control_plane_));
}

// A resolving Router whose manifest lists `static_ids` in rt/next_hops,
// `bound_ids` in rt/neighbors (192.0.2.<id + 1> on interface 2) and, with
// `route`, a route to bound hop 1. The neighbors are listed after the routes
// on purpose: the apply orders operations by what references what, not by the
// manifest's order.
PipelineSpec ResolvingRouterPipeline(const std::vector<uint32_t> &static_ids,
                                     const std::vector<uint32_t> &bound_ids = {1},
                                     bool route = true) {
  bess::pb::RouterArg arg;
  arg.set_max_ipv4_routes(16);
  auto *interface = arg.add_interfaces();
  interface->set_id(2);
  interface->set_egress_gate(1);
  interface->set_mac_addr("02:00:00:00:00:01");
  interface->add_addresses("192.0.2.1/24");
  PipelineSpec spec;
  spec.modules.push_back({"rt", "Router", Packed(arg)});
  bess::control::ResourceSpec hops{"rt/next_hops", {}};
  for (uint32_t id : static_ids) {
    bess::pb::RouterNextHopIdKey key;
    key.set_id(id);
    bess::pb::RouterNextHopValue value;
    value.set_egress_gate(1);  // resolved: the canonical form omits the state
    value.set_dst_mac(std::string("\x02\x00\x00\x00\x00\xaa", 6));
    value.set_src_mac(std::string("\x02\x00\x00\x00\x00\x01", 6));
    hops.entries.push_back({Packed(key), Packed(value)});
  }
  spec.resources.push_back(std::move(hops));
  bess::control::ResourceSpec routes{"rt/routes", {}};
  if (route) {
    bess::pb::RouterRouteKey key;
    key.set_ipv4("198.51.100.0");
    key.set_prefix_length(24);
    bess::pb::RouterRouteValue to_bound_hop;
    to_bound_hop.set_next_hop_id(1);
    routes.entries.push_back({Packed(key), Packed(to_bound_hop)});
  }
  spec.resources.push_back(std::move(routes));
  bess::control::ResourceSpec neighbors{"rt/neighbors", {}};
  for (uint32_t id : bound_ids) {
    bess::pb::RouterNextHopIdKey key;
    key.set_id(id);
    bess::pb::RouterNeighborBinding value;
    value.set_interface_id(2);
    value.set_ipv4("192.0.2." + std::to_string(id + 1));
    neighbors.entries.push_back({Packed(key), Packed(value)});
  }
  spec.resources.push_back(std::move(neighbors));
  return spec;
}

// Neighbor bindings are desired state of their own (D-180): a manifest lists
// them in rt/neighbors and the routes that name them in rt/routes, in any
// order. The bound hops are not rt/next_hops entries, so a static list
// neither erases nor reports them, and one that names a bound id is refused.
// A binding a route still names cannot be dropped; dropped together with the
// route, it goes.
TEST_F(ApplyPipelineTest, NeighborBindingsConvergeAsTheirOwnResource) {
  // Neighbor resolution runs on the maintenance loop; stand in for its interval.
  bess::runtime::runtime().ticks().set_interval(std::chrono::milliseconds(1));
  auto applied = control_plane_->ApplyPipeline(ResolvingRouterPipeline({2, 3}), {});
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_EQ(4u, applied->resource_ops)
      << "hops 2 and 3, the binding of hop 1, and the route to it";

  auto again = control_plane_->ApplyPipeline(ResolvingRouterPipeline({2, 3}), {});
  ASSERT_TRUE(again) << again.error().message;
  EXPECT_EQ(0u, again->resource_ops) << "everything reads back as listed";

  // Hop 3 goes; bound hop 1 stays, as the route that names it shows.
  auto shrunk = control_plane_->ApplyPipeline(ResolvingRouterPipeline({2}), {});
  ASSERT_TRUE(shrunk) << shrunk.error().message;
  EXPECT_EQ(1u, shrunk->resource_ops);
  auto hops = ReadResource(*control_plane_, "rt/next_hops");
  ASSERT_TRUE(hops);
  ASSERT_EQ(1u, hops->entries.size());
  bess::pb::RouterNextHopIdKey listed;
  ASSERT_TRUE(hops->entries[0].key.UnpackTo(&listed));
  EXPECT_EQ(2u, listed.id());
  auto routes = ReadResource(*control_plane_, "rt/routes");
  ASSERT_TRUE(routes);
  EXPECT_EQ(1u, routes->entries.size()) << "the route to hop 1 still stands";

  // Naming the bound id as a static hop is refused, and the apply changes
  // nothing.
  auto owned = control_plane_->ApplyPipeline(ResolvingRouterPipeline({1, 2}), {});
  EXPECT_FALSE(owned);
  hops = ReadResource(*control_plane_, "rt/next_hops");
  ASSERT_TRUE(hops);
  EXPECT_EQ(1u, hops->entries.size());

  // A second binding comes at runtime; dropping hop 1's binding while the
  // route names it is refused.
  auto grown = control_plane_->ApplyPipeline(ResolvingRouterPipeline({2}, {1, 4}), {});
  ASSERT_TRUE(grown) << grown.error().message;
  EXPECT_EQ(1u, grown->resource_ops);
  auto referenced = control_plane_->ApplyPipeline(ResolvingRouterPipeline({2}, {4}), {});
  EXPECT_FALSE(referenced) << "the route still names hop 1";
  auto neighbors = ReadResource(*control_plane_, "rt/neighbors");
  ASSERT_TRUE(neighbors);
  EXPECT_EQ(2u, neighbors->entries.size());

  // The binding and the route that names it go together.
  auto dropped =
      control_plane_->ApplyPipeline(ResolvingRouterPipeline({2}, {4}, /*route=*/false), {});
  ASSERT_TRUE(dropped) << dropped.error().message;
  EXPECT_EQ(2u, dropped->resource_ops);
  neighbors = ReadResource(*control_plane_, "rt/neighbors");
  ASSERT_TRUE(neighbors);
  ASSERT_EQ(1u, neighbors->entries.size());
  ASSERT_TRUE(neighbors->entries[0].key.UnpackTo(&listed));
  EXPECT_EQ(4u, listed.id());
}

// A listed resource of a module the apply creates is reported, unchecked,
// even when the list is empty: the apply converges it, and entries the module
// seeds from its init argument would go without a delta saying so.
TEST_F(ApplyPipelineTest, ACreatedModulesListedResourceIsAlwaysInTheDiff) {
  auto diff = control_plane_->DiffPipelineVersioned(ExactMatchPipeline({}, std::nullopt));
  ASSERT_TRUE(diff) << diff.error().message;
  ASSERT_EQ(1u, diff->value.resources.size());
  const bess::control::ResourceDelta &delta = diff->value.resources[0];
  EXPECT_EQ("em/rules", delta.name);
  EXPECT_EQ(0u, delta.created);
  EXPECT_FALSE(delta.checked) << "removals are unknown before the module exists";
}

// What GetPipeline reads with its resources applies back as a no-op, and a
// stale resource generation is a conflict that changes nothing.
TEST_F(ApplyPipelineTest, ResourcesReadBackApplyAsANoOpAndAreVersioned) {
  const std::string a("\x0a\x00\x00\x01", 4);
  ASSERT_TRUE(control_plane_->ApplyPipeline(ExactMatchPipeline({a}, 2), {}));
  auto read = control_plane_->ReadPipeline({.include = true});
  ASSERT_TRUE(read) << read.error().message;
  PipelineSpec spec = SpecFromSnapshot(read->value.snapshot);
  spec.resources = read->value.resources;

  auto diff = control_plane_->DiffPipelineVersioned(spec);
  ASSERT_TRUE(diff) << diff.error().message;
  for (const auto &r : diff->value.resources) {
    EXPECT_EQ(0u, r.created + r.updated + r.removed) << r.name;
  }
  auto back = control_plane_->ApplyPipeline(
      spec, {.expected_generation = read->generation,
             .expected_resource_generation = read->resource_generation});
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(0u, back->resource_ops);
  EXPECT_EQ(read->generation, back->generation);
  EXPECT_EQ(read->resource_generation, back->resource_generation);

  // Resource contents move: the version read before is stale.
  ASSERT_TRUE(control_plane_->ApplyPipeline(ExactMatchPipeline({a}, 5), {}));
  auto stale = control_plane_->ApplyPipeline(
      ExactMatchPipeline({}, 7), {.expected_generation = read->generation,
                                  .expected_resource_generation = read->resource_generation});
  ASSERT_FALSE(stale);
  EXPECT_EQ(bess::control::ControlErrorCode::kConflict, stale.error().code);
  EXPECT_EQ(5u, DefaultGate(*control_plane_));
  auto rules = ReadResource(*control_plane_, "em/rules");
  ASSERT_TRUE(rules);
  EXPECT_EQ(1u, rules->entries.size());
}

// A resource write (here an apply's own) moves the resource generation, so an
// apply built on a read from before it conflicts instead of erasing the edit;
// a read moves nothing.
TEST_F(ApplyPipelineTest, ResourceEditsMoveTheResourceGeneration) {
  const std::string a("\x0a\x00\x00\x01", 4);
  ASSERT_TRUE(control_plane_->ApplyPipeline(ExactMatchPipeline({a}, 2), {}));
  auto read = control_plane_->ReadPipeline({.include = true});
  ASSERT_TRUE(read) << read.error().message;

  auto after_read = control_plane_->ReadPipeline({});
  ASSERT_TRUE(after_read) << after_read.error().message;
  EXPECT_EQ(read->resource_generation, after_read->resource_generation)
      << "a read moved the resource generation";

  // The same rules, another default gate: the write moves the generation.
  ASSERT_TRUE(control_plane_->ApplyPipeline(ExactMatchPipeline({a}, 4), {}));
  EXPECT_EQ(4u, DefaultGate(*control_plane_));

  PipelineSpec spec = SpecFromSnapshot(read->value.snapshot);
  spec.resources = read->value.resources;
  auto stale = control_plane_->ApplyPipeline(
      spec, {.expected_generation = read->generation,
             .expected_resource_generation = read->resource_generation});
  ASSERT_FALSE(stale) << "the apply undid the resource edit";
  EXPECT_EQ(bess::control::ControlErrorCode::kConflict, stale.error().code);
  EXPECT_EQ(4u, DefaultGate(*control_plane_));
}

// An action that edits the resource it owns (L2Forward's populate, D-140)
// moves the resource generation through RunModuleCommand, so an apply built on
// a read from before the action conflicts instead of erasing what it wrote; an
// action that only reads leaves the generation where it was.
TEST_F(ApplyPipelineTest, AnEditingActionMovesTheResourceGeneration) {
  ASSERT_TRUE(control_plane_->CreateModule({"l2", "L2Forward", Packed(bess::pb::L2ForwardArg())}));
  auto read = control_plane_->ReadPipeline({.include = true});
  ASSERT_TRUE(read) << read.error().message;

  bess::pb::L2ForwardCommandPopulateArg populate;
  populate.set_base("02:00:00:00:00:01");
  populate.set_count(1);
  populate.set_gate_count(1);
  auto ran = control_plane_->RunModuleCommand("l2", "populate", Packed(populate));
  ASSERT_TRUE(ran) << ran.error().message;
  EXPECT_EQ(0, ran->error().code()) << ran->error().errmsg();
  ASSERT_TRUE(ReadResource(*control_plane_, "l2/macs"));
  EXPECT_EQ(1u, ReadResource(*control_plane_, "l2/macs")->entries.size());

  auto after_action = control_plane_->ReadPipeline({});
  ASSERT_TRUE(after_action) << after_action.error().message;
  EXPECT_NE(read->resource_generation, after_action->resource_generation)
      << "the editing action did not move the resource generation";
  EXPECT_EQ(read->generation, after_action->generation)
      << "an action moved the pipeline generation";

  // A read-only action (lookup) does not move it: the EditsResources() flag,
  // not the action, decides.
  bess::pb::L2ForwardCommandLookupArg lookup;
  lookup.add_addrs("02:00:00:00:00:01");
  auto looked_up = control_plane_->RunModuleCommand("l2", "lookup", Packed(lookup));
  ASSERT_TRUE(looked_up) << looked_up.error().message;
  EXPECT_EQ(0, looked_up->error().code()) << looked_up->error().errmsg();
  auto after_lookup = control_plane_->ReadPipeline({});
  ASSERT_TRUE(after_lookup) << after_lookup.error().message;
  EXPECT_EQ(after_action->resource_generation, after_lookup->resource_generation)
      << "a read-only action moved the resource generation";

  PipelineSpec spec = SpecFromSnapshot(read->value.snapshot);
  spec.resources = read->value.resources;
  auto stale = control_plane_->ApplyPipeline(
      spec, {.expected_generation = read->generation,
             .expected_resource_generation = read->resource_generation});
  ASSERT_FALSE(stale) << "the apply undid the action's edit";
  EXPECT_EQ(bess::control::ControlErrorCode::kConflict, stale.error().code);
  EXPECT_EQ("expected_resource_generation", stale.error().field) << stale.error().message;
  ASSERT_TRUE(ReadResource(*control_plane_, "l2/macs"));
  EXPECT_EQ(1u, ReadResource(*control_plane_, "l2/macs")->entries.size());

  // An action that does not exist ran nothing, so it moves nothing.
  auto missing = control_plane_->RunModuleCommand("l2", "no_such_action", Packed(populate));
  ASSERT_TRUE(missing) << missing.error().message;
  EXPECT_EQ(ENOTSUP, missing->error().code()) << missing->error().errmsg();
  auto after_missing = control_plane_->ReadPipeline({});
  ASSERT_TRUE(after_missing) << after_missing.error().message;
  EXPECT_EQ(after_lookup->resource_generation, after_missing->resource_generation);
}

// A resource the apply cannot converge (a gate ExactMatch refuses) aborts the
// whole apply: the topology it came with is rolled back too.
TEST_F(ApplyPipelineTest, ARefusedResourceRollsBackTheTopology) {
  const std::string a("\x0a\x00\x00\x01", 4);
  auto first = control_plane_->ApplyPipeline(ExactMatchPipeline({a}, 2), {});
  ASSERT_TRUE(first) << first.error().message;

  PipelineSpec spec = ExactMatchPipeline({}, 99999);
  spec.modules.push_back({"extra", "Bypass", {}});
  auto refused = control_plane_->ApplyPipeline(spec, {});
  ASSERT_FALSE(refused);
  EXPECT_EQ("resources", refused.error().field) << refused.error().message;
  EXPECT_EQ(nullptr, bess::runtime::runtime().modules().Find("extra"));
  EXPECT_EQ(2u, DefaultGate(*control_plane_));
  auto rules = ReadResource(*control_plane_, "em/rules");
  ASSERT_TRUE(rules);
  EXPECT_EQ(1u, rules->entries.size()) << "the rules erase did not stay";
  auto read = control_plane_->ReadPipeline({});
  ASSERT_TRUE(read);
  EXPECT_EQ(first->generation, read->generation);
}

// The init argument of each class's module in the round trip below, as
// protobuf JSON; any other class is created with an empty argument.
const std::map<std::string, std::string> &InitArgs() {
  static const std::map<std::string, std::string> args = {
      {"ExactMatch", R"({"fields": [{"offset": 26, "numBytes": 4}]})"},
      {"WildcardMatch", R"({"fields": [{"offset": 26, "numBytes": 4}]})"},
      {"PortInc", R"({"port": "p0"})"},
      {"QueueInc", R"({"port": "p1"})"},
      // 60 bytes: Ethernet, IPv4 10.0.0.1 -> 10.0.0.2, UDP 1000 -> 2000.
      {"FlowGen",
       R"({"template": "AgAAAAACAgAAAAABCABFAAAuAAAAAEARAAAKAAABCgAAAgPoB9AAGgAAAAAAAAAAAAAAAAAAAAAAAAAA",
           "pps": 1000, "flow_rate": 10, "flow_duration": 10, "arrival": "uniform",
           "duration": "uniform"})"},
      {"HashLB", R"({"gates": [0, 1]})"},
      {"RoundRobin", R"({"gates": [0]})"},
      {"RandomSplit", R"({"drop_rate": 0, "gates": [0]})"},
      {"Replicate", R"({"gates": [0]})"},
      // A Router routes at least one address family (D-180).
      {"Router", R"({"max_ipv4_routes": 16})"},
  };
  return args;
}

// Every in-tree class that declares resources: one module of it, read back
// with its resources (defaults included) and applied back changes nothing.
// A read-back that is not the canonical form a client would send would show
// up here as an update.
TEST_F(ApplyPipelineTest, EveryDeclaredResourceReadsBackAsANoOp) {
  for (const std::string name : {"p0", "p1"}) {
    bess::pb::UnixSocketPortArg port_arg;
    port_arg.set_path("@bess_apply_pipeline_test_" + name);
    bess::control::PortSpec port;
    port.name = name;
    port.driver = "UnixSocketPort";
    port.arg = Packed(port_arg);
    ASSERT_TRUE(control_plane_->CreatePort(port));
  }

  // Every class with resources, in a stable order.
  std::vector<std::pair<std::string, const ModuleBuilder *>> classes;
  for (const auto &[mclass, builder] : ModuleBuilder::all_module_builders()) {
    if (!builder.resource_tables().empty()) {
      classes.emplace_back(mclass, &builder);
    }
  }
  std::vector<std::string> expected;
  for (const auto &[mclass, builder] : classes) {
    const google::protobuf::Descriptor *type =
        google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(
            builder->init_arg_type());
    ASSERT_NE(nullptr, type) << mclass << " '" << builder->init_arg_type() << "'";
    std::unique_ptr<google::protobuf::Message> arg(
        google::protobuf::MessageFactory::generated_factory()->GetPrototype(type)->New());
    if (auto it = InitArgs().find(mclass); it != InitArgs().end()) {
      ASSERT_TRUE(google::protobuf::util::JsonStringToMessage(it->second, arg.get()).ok())
          << mclass;
    }
    const std::string name = "m_" + mclass;
    auto created = control_plane_->CreateModule({name, mclass, Packed(*arg)});
    ASSERT_TRUE(created) << mclass << ": " << created.error().message;
    for (const ResourceTable &t : builder->resource_tables()) {
      expected.push_back(name + "/" + t.table);
    }
  }
  ASSERT_FALSE(expected.empty());
  std::sort(expected.begin(), expected.end());

  auto read = control_plane_->ReadPipeline({.include = true});
  ASSERT_TRUE(read) << read.error().message;
  std::vector<std::string> listed;
  for (const auto &r : read->value.resources) {
    listed.push_back(r.name);
  }
  EXPECT_EQ(expected, listed);

  PipelineSpec spec = SpecFromSnapshot(read->value.snapshot);
  spec.resources = read->value.resources;
  auto diff = control_plane_->DiffPipeline(spec);
  ASSERT_TRUE(diff) << diff.error().message;
  for (const auto &r : diff->resources) {
    EXPECT_EQ(0u, r.created + r.updated + r.removed)
        << r.name << ": +" << r.created << " ~" << r.updated << " -" << r.removed;
  }
  auto back = control_plane_->ApplyPipeline(spec, {});
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(0u, back->resource_ops);
  EXPECT_EQ(read->resource_generation, back->resource_generation);
  EXPECT_EQ(read->generation, back->generation);
}

}  // namespace
