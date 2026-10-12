// SPDX-License-Identifier: BSD-3-Clause

// ControlPlane::CheckSchedulingConstraints after an ApplyPipeline (was
// bessctl/module_tests/placement_constraint.py): the samples' pipelines meet
// their placement constraints (a queue splitting two traffic classes, a NAT
// revisited by one chain, both together); a NAT, which allows one worker, run
// by two workers is a fatal violation. Only the legacy API serves the check
// (CheckSchedulingConstraints); control_v2 has none.

#include <gtest/gtest.h>
#include <sched.h>

#include <string>
#include <vector>

#include "control/api_v2.h"
#include "control/control_plane.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "port.h"
#include "runtime/opts.h"

namespace {

namespace v2 = bess::pb::v2;
using bess::control::ControlPlane;
using bess::control::SchedulingConstraintsReport;

void InitRuntimeOnce() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  FLAGS_m = 0;  // malloc-backed, sandbox-safe
  bess::PacketPool::CreateDefaultPools(32767);
  PortBuilder::InitDrivers();
}

// The CPUs this process may run on, in order.
std::vector<int> AllowedCpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  EXPECT_EQ(0, sched_getaffinity(0, sizeof(set), &set));
  std::vector<int> cpus;
  for (int c = 0; c < CPU_SETSIZE; c++) {
    if (CPU_ISSET(c, &set)) {
      cpus.push_back(c);
    }
  }
  return cpus;
}

class PipelineBuilder {
 public:
  PipelineBuilder &Worker(int wid, int core) {
    auto *w = p_.add_workers();
    w->set_wid(wid);
    w->set_core(core);
    return *this;
  }
  PipelineBuilder &Module(const std::string &name, const std::string &mclass,
                          const google::protobuf::Message *arg = nullptr) {
    auto *m = p_.add_modules();
    m->set_name(name);
    m->set_mclass(mclass);
    if (arg != nullptr) {
      EXPECT_TRUE(m->mutable_arg()->PackFrom(*arg));
    }
    return *this;
  }
  PipelineBuilder &Connect(const std::string &up, uint32_t ogate, const std::string &down,
                           uint32_t igate) {
    auto *c = p_.add_connections();
    c->set_upstream(up);
    c->set_ogate(ogate);
    c->set_downstream(down);
    c->set_igate(igate);
    return *this;
  }
  PipelineBuilder &RateLimit(const std::string &name, int wid, int64_t packets) {
    auto *tc = p_.add_traffic_classes();
    tc->set_name(name);
    tc->set_policy("rate_limit");
    tc->set_resource("packet");
    tc->set_wid(wid);
    (*tc->mutable_limit())["packet"] = packets;
    return *this;
  }
  PipelineBuilder &Task(const std::string &module, const std::string &parent, int wid = -1) {
    auto *t = p_.add_tasks();
    t->set_module(module);
    if (!parent.empty()) {
      t->set_parent(parent);
    }
    if (wid >= 0) {
      t->set_wid(wid);
    }
    return *this;
  }
  const v2::Pipeline &pipeline() const { return p_; }

 private:
  v2::Pipeline p_;
};

bess::pb::NATArg NatArg() {
  bess::pb::NATArg arg;
  arg.add_ext_addrs()->set_ext_addr("192.168.1.1");
  return arg;
}

class SchedulingConstraintsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    cpus_ = AllowedCpus();
    ASSERT_FALSE(cpus_.empty());
  }
  void TearDown() override {
    (void)control_plane_.PauseAll();
    (void)control_plane_.Reset();
  }

  void Apply(const PipelineBuilder &b) {
    auto spec = bess::control::FromProto(b.pipeline());
    ASSERT_TRUE(spec) << spec.error().message;
    auto applied = control_plane_.ApplyPipeline(*spec, {});
    ASSERT_TRUE(applied) << applied.error().message;
  }

  SchedulingConstraintsReport Check() {
    auto report = control_plane_.CheckSchedulingConstraints();
    EXPECT_TRUE(report) << report.error().message;
    return report ? *report : SchedulingConstraintsReport{};
  }

  static void ExpectNoViolation(const SchedulingConstraintsReport &r) {
    EXPECT_TRUE(r.violations.empty()) << r.violations.front().name;
    EXPECT_TRUE(r.modules.empty()) << r.modules.front().name;
    EXPECT_FALSE(r.fatal);
  }

  ControlPlane control_plane_;
  std::vector<int> cpus_;
};

// conf/samples/queue: the source under a fast rate limit, the queue's
// dequeue task under a slow one.
TEST_F(SchedulingConstraintsTest, QueueBetweenTwoTrafficClasses) {
  bess::pb::VLANPushArg vlan;
  vlan.set_tci(2);
  PipelineBuilder b;
  b.Worker(0, cpus_[0])
      .Module("src", "Source")
      .Module("queue", "Queue")
      .Module("vlan", "VLANPush", &vlan)
      .Module("sink", "Sink")
      .Connect("src", 0, "queue", 0)
      .Connect("queue", 0, "vlan", 0)
      .Connect("vlan", 0, "sink", 0)
      .RateLimit("fast", 0, 9000000)
      .RateLimit("slow", 0, 1000000)
      .Task("src", "fast")
      .Task("queue", "slow");
  Apply(b);
  ExpectNoViolation(Check());
}

// The nat sample: one chain visits the NAT twice (0:nat:1 and 1:nat:0).
TEST_F(SchedulingConstraintsTest, NatRevisitedByOneChain) {
  const bess::pb::NATArg nat = NatArg();
  PipelineBuilder b;
  b.Worker(0, cpus_[0])
      .Module("src", "Source")
      .Module("nat", "NAT", &nat)
      .Module("mac", "MACSwap")
      .Module("ip", "IPSwap")
      .Module("sink", "Sink")
      .Connect("src", 0, "nat", 0)
      .Connect("nat", 1, "mac", 0)
      .Connect("mac", 0, "ip", 0)
      .Connect("ip", 0, "nat", 1)
      .Connect("nat", 0, "sink", 0)
      .Task("src", "", 0);
  Apply(b);
  ExpectNoViolation(Check());
}

// Both: the queue's task drives the NAT's return direction.
TEST_F(SchedulingConstraintsTest, NatAndQueue) {
  const bess::pb::NATArg nat = NatArg();
  PipelineBuilder b;
  b.Worker(0, cpus_[0])
      .Module("src", "Source")
      .Module("nat", "NAT", &nat)
      .Module("queue", "Queue")
      .Module("ip", "IPSwap")
      .Module("sink", "Sink")
      .Connect("src", 0, "nat", 0)
      .Connect("nat", 1, "queue", 0)
      .Connect("queue", 0, "ip", 0)
      .Connect("ip", 0, "nat", 1)
      .Connect("nat", 0, "sink", 0)
      .Task("src", "", 0)
      .Task("queue", "", 0);
  Apply(b);
  ExpectNoViolation(Check());
}

// A NAT (one worker allowed) fed by sources on two workers: fatal.
TEST_F(SchedulingConstraintsTest, NatOnTwoWorkersIsFatal) {
  if (cpus_.size() < 2) {
    GTEST_SKIP() << "requires at least two available CPU cores";
  }
  const bess::pb::NATArg nat = NatArg();
  PipelineBuilder b;
  b.Worker(0, cpus_[0])
      .Worker(1, cpus_[1])
      .Module("src0", "Source")
      .Module("src1", "Source")
      .Module("nat", "NAT", &nat)
      .Module("sink0", "Sink")
      .Module("sink1", "Sink")
      .Connect("src0", 0, "nat", 0)
      .Connect("nat", 1, "sink0", 0)
      .Connect("src1", 0, "nat", 1)
      .Connect("nat", 0, "sink1", 0)
      .Task("src0", "", 0)
      .Task("src1", "", 1);
  Apply(b);
  const SchedulingConstraintsReport r = Check();
  EXPECT_TRUE(r.fatal);
  ASSERT_EQ(1u, r.modules.size());
  EXPECT_EQ("nat", r.modules[0].name);
}

}  // namespace
