// SPDX-License-Identifier: BSD-3-Clause

// Reference example for D-045: two module instances share one
// application-owned object through an explicit instance, not a global service.
// The module resolves the instance once in Init(), keeps the lease, and caches
// the raw pointer; the packet-side method touches neither registry nor lease.

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>

#include "module.h"
#include "module_graph.h"
#include "pb/module_msg.pb.h"
#include "framework/instance_registry.h"
#include "runtime/runtime_state.h"

namespace {

using bess::framework::InstanceError;
using bess::framework::InstanceErrorName;
using bess::framework::InstanceLease;

constexpr char kInstanceName[] = "counter0";

// The application's own state; BESS knows nothing about its meaning.
struct SharedCounter {
  std::atomic<uint64_t> packets{0};
};

class SharedCounterModule final : public Module {
 public:
  static const Commands cmds;

  CommandResponse Init(const bess::pb::EmptyArg &) {
    auto lease = init_context().instances().Lookup<SharedCounter>(kInstanceName);
    if (!lease) {
      return CommandFailure(ENOENT, "instance '%s': %s", kInstanceName,
                            InstanceErrorName(lease.error()));
    }
    lease_ = std::move(*lease);
    counter_ = lease_.get();
    return CommandSuccess();
  }

  // What a ProcessBatch() would do: a direct pointer, no lookup, no refcount.
  void Count(uint64_t packets) {
    counter_->packets.fetch_add(packets, std::memory_order_relaxed);
  }

 private:
  InstanceLease<SharedCounter> lease_;  // keeps the instance alive
  SharedCounter *counter_ = nullptr;    // the cached hot-path pointer
};

const Commands SharedCounterModule::cmds = {};
ADD_MODULE(SharedCounterModule, "shared_instance_counter",
           "reference module sharing an application instance")

class SharedInstanceTest : public ::testing::Test {
 protected:
  void TearDown() override {
    ModuleGraph::DestroyAllModules();
    // Ignored when the test never created it.
    (void)bess::runtime::runtime().instances().Destroy(kInstanceName);
  }

  static bess::framework::InstanceRegistry &Registry() {
    return bess::runtime::runtime().instances();
  }

  static SharedCounterModule *CreateModule(const std::string &name,
                                           pb_error_t *perr) {
    const auto &builders = ModuleBuilder::all_module_builders();
    const auto it = builders.find("SharedCounterModule");
    EXPECT_NE(it, builders.end());
    google::protobuf::Any packed;
    EXPECT_TRUE(packed.PackFrom(bess::pb::EmptyArg()));
    return dynamic_cast<SharedCounterModule *>(
        ModuleGraph::CreateModule(it->second, name, packed, perr));
  }
};

TEST_F(SharedInstanceTest, TwoModulesShareOneInstance) {
  auto created = Registry().Create<SharedCounter>(kInstanceName);
  ASSERT_TRUE(created.has_value());
  SharedCounter *counter = created->get();
  created->Release();

  pb_error_t perr;
  SharedCounterModule *a = CreateModule("a", &perr);
  ASSERT_NE(nullptr, a) << perr.errmsg();
  SharedCounterModule *b = CreateModule("b", &perr);
  ASSERT_NE(nullptr, b) << perr.errmsg();

  a->Count(3);
  b->Count(4);
  EXPECT_EQ(7u, counter->packets.load());
  EXPECT_EQ(2u, Registry().Describe()[0].leases);
}

TEST_F(SharedInstanceTest, DestroyWhileModulesUseItIsRefusedThenAllowed) {
  ASSERT_TRUE(Registry().Create<SharedCounter>(kInstanceName).has_value());
  pb_error_t perr;
  ASSERT_NE(nullptr, CreateModule("a", &perr)) << perr.errmsg();
  ASSERT_NE(nullptr, CreateModule("b", &perr)) << perr.errmsg();

  auto refused = Registry().Destroy(kInstanceName);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(InstanceError::kInUse, refused.error());

  ModuleGraph::DestroyAllModules();
  EXPECT_EQ(0u, Registry().Describe()[0].leases);
  EXPECT_TRUE(Registry().Destroy(kInstanceName).has_value());
}

TEST_F(SharedInstanceTest, ModuleInitFailsWithoutCreatingTheInstance) {
  pb_error_t perr;
  EXPECT_EQ(nullptr, CreateModule("a", &perr));
  EXPECT_EQ(ENOENT, perr.code());
  EXPECT_EQ(0u, Registry().size());  // lookup never creates
}

TEST_F(SharedInstanceTest, ModuleInitFailsOnATypeMismatch) {
  ASSERT_TRUE(Registry().Create<std::string>(kInstanceName).has_value());
  pb_error_t perr;
  EXPECT_EQ(nullptr, CreateModule("a", &perr));
  EXPECT_EQ(ENOENT, perr.code());
  EXPECT_EQ(0u, Registry().Describe()[0].leases);
}

}  // namespace
