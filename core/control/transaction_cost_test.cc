// SPDX-License-Identifier: BSD-3-Clause

// The fixed cost of one dataplane transaction, counted rather than timed
// (engineering guide §8): heap allocations per 1-operation and 8-operation
// transaction at the typed engine (ApplyOwned, BM_StageOwned's boundary) and
// at the RPC adapter (DataplaneTransactions::Apply, BM_StageAdapter's), and
// control-plane lock acquisitions per TransactionChannel frame. The graph is
// the stage bench's: eight ExactMatch modules keyed on four bytes at offset
// 26, each resident with kResident rules, one key rewritten per stage.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <any>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "control/api_v2.h"
#include "control/control_plane.h"
#include "control/dataplane_transactions.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "modules/exact_match.h"
#include "packet_pool.h"
#include "pb/control_v2.grpc.pb.h"
#include "pb/module_msg.pb.h"
#include "port.h"
#include "runtime/runtime_state.h"
#include "testing/allocation_faults.h"

namespace {

namespace v2 = bess::pb::v2;
using bess::control::ControlPlane;
using bess::control::ControlV2Service;
using bess::control::DataplaneTransactions;
using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;
using bess::fault_injection::AllocationFaults;

constexpr size_t kStages = 8;
constexpr uint32_t kResident = 64;
// Transactions measured after the warm-up, per count.
constexpr int kMeasured = 32;

void InitRuntimeOnce() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  bess::PacketPool::CreateDefaultPools(32767);
  PortBuilder::InitDrivers();
}

std::string StageKey(uint32_t slot) {
  const uint32_t address = 0x0b000001u + slot;
  return {static_cast<char>(address >> 24u), static_cast<char>(address >> 16u),
          static_cast<char>(address >> 8u), static_cast<char>(address)};
}

std::string Resource(size_t stage) {
  return "stage" + std::to_string(stage) + "/rules";
}

class TransactionCostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    control_ = std::make_unique<ControlPlane>();
    TransactionEngine &engine = bess::runtime::runtime().transactions();
    for (size_t stage = 0; stage < kStages; stage++) {
      bess::pb::ExactMatchArg arg;
      arg.add_fields()->set_offset(26);
      arg.mutable_fields(0)->set_num_bytes(4);
      arg.set_default_gate(0);
      google::protobuf::Any packed;
      ASSERT_TRUE(packed.PackFrom(arg));
      ASSERT_TRUE(control_->CreateModule(
          {"stage" + std::to_string(stage), "ExactMatch", packed}));
      const bess::dataplane::Resource *resource =
          engine.FindResource(Resource(stage));
      ASSERT_NE(nullptr, resource);
      owned_.push_back(resource);
      std::vector<Op> seed;
      for (uint32_t slot = 0; slot < kResident; slot++) {
        seed.push_back(Op::Upsert(Resource(stage), StageKey(slot),
                                  std::any(bess::classifier::RuleResult{1, {}})));
      }
      ASSERT_EQ(TransactionEngine::Outcome::kApplied, engine.Apply(seed).outcome);
    }
    service_ = std::make_unique<ControlV2Service>(*control_);
    adapter_ = std::make_unique<DataplaneTransactions>(
        engine, bess::framework::ResourceBindings::ProcessDefault(),
        service_->epoch());
  }

  void TearDown() override {
    adapter_.reset();
    service_.reset();
    bess::runtime::runtime().rcu().Drain();
    (void)control_->Reset();
    bess::runtime::runtime().rcu().Drain();
  }

  // A request rewriting one key's gate in each of the first `stages` stages.
  v2::ApplyTransactionRequest Request(size_t stages, uint64_t ordinal) {
    v2::ApplyTransactionRequest request;
    request.set_daemon_epoch(service_->epoch());
    request.set_request_id("cost-" + std::to_string(stages) + "-" +
                           std::to_string(ordinal));
    for (size_t stage = 0; stage < stages; stage++) {
      v2::TransactionOp *op = request.add_ops();
      op->set_resource(Resource(stage));
      bess::pb::ExactMatchRuleKey key;
      key.add_fields()->set_value_bin(StageKey(0));
      bess::pb::ExactMatchRuleValue value;
      value.set_gate(1 + ordinal % 2);
      EXPECT_TRUE(op->mutable_key()->PackFrom(key));
      EXPECT_TRUE(op->mutable_value()->PackFrom(value));
    }
    return request;
  }

  // Allocations per typed ApplyOwned of `stages` operations, steady state.
  double OwnedAllocations(size_t stages) {
    TransactionEngine &engine = bess::runtime::runtime().transactions();
    std::vector<Op> ops;
    for (size_t stage = 0; stage < stages; stage++) {
      ops.push_back(Op::Upsert(Resource(stage), StageKey(0),
                               std::any(bess::classifier::RuleResult{1, {}})));
    }
    const std::span<const bess::dataplane::Resource *const> owned(owned_.data(), stages);
    size_t allocations = 0;
    for (int i = 0; i < 8 + kMeasured; i++) {
      for (Op &op : ops) {
        std::any_cast<bess::classifier::RuleResult>(&op.value)->gate =
            static_cast<gate_idx_t>(1 + i % 2);
      }
      auto lock = control_->AcquireLock();
      AllocationFaults window;
      const TransactionEngine::Result result = engine.ApplyOwned(owned, ops);
      EXPECT_EQ(TransactionEngine::Outcome::kApplied, result.outcome);
      if (i >= 8) {
        allocations += window.allocations();
      }
    }
    return static_cast<double>(allocations) / kMeasured;
  }

  // Allocations per DataplaneTransactions::Apply of `stages` operations,
  // steady state: unique request ids, receipts recorded, responses dropped.
  double AdapterAllocations(size_t stages) {
    size_t allocations = 0;
    for (int i = 0; i < 8 + kMeasured; i++) {
      const v2::ApplyTransactionRequest request =
          Request(stages, static_cast<uint64_t>(i));
      auto lock = control_->AcquireLock();
      AllocationFaults window;
      auto result = adapter_->Apply(request);
      if (i >= 8) {
        allocations += window.allocations();
      }
      EXPECT_TRUE(result.has_value());
      if (result) {
        EXPECT_EQ(v2::TransactionRecord::OUTCOME_APPLIED, result->record().outcome());
        EXPECT_EQ(static_cast<int>(stages), result->record().ops_size());
      }
    }
    return static_cast<double>(allocations) / kMeasured;
  }

  std::unique_ptr<ControlPlane> control_;
  std::unique_ptr<ControlV2Service> service_;
  std::unique_ptr<DataplaneTransactions> adapter_;
  std::vector<const bess::dataplane::Resource *> owned_;
};

// What one transaction allocates in the steady state, by boundary. The typed
// engine: the result's operation list and each operation's staged work (the
// resource's StagedOp), nothing of its own per call or per operation. The
// adapter adds the codecs' decoded keys and values, the request's ops and
// record, and its receipt.
TEST_F(TransactionCostTest, AllocationsPerTransaction) {
  const double owned1 = OwnedAllocations(1);
  const double owned8 = OwnedAllocations(kStages);
  const double adapter1 = AdapterAllocations(1);
  const double adapter8 = AdapterAllocations(kStages);
  std::printf("allocations per transaction: owned 1-op %.2f, 8-op %.2f; "
              "adapter 1-op %.2f, 8-op %.2f\n",
              owned1, owned8, adapter1, adapter8);
  EXPECT_LE(owned1, 2.0);
  EXPECT_LE(owned8, 9.0);
  EXPECT_LE(adapter1, 10.25);
  EXPECT_LE(adapter8, 46.25);
}

// The adapter resolves a transaction's resources under one acquisition of
// the engine's lock, then applies it under one more: two per transaction,
// whatever its operation count.
TEST_F(TransactionCostTest, AdapterTakesTheEngineLockTwicePerTransaction) {
  TransactionEngine &engine = bess::runtime::runtime().transactions();
  for (const size_t stages : {size_t{1}, kStages}) {
    const v2::ApplyTransactionRequest request = Request(stages, 9000 + stages);
    auto lock = control_->AcquireLock();
    const uint64_t before = engine.lock_acquisitions();
    auto result = adapter_->Apply(request);
    const uint64_t taken = engine.lock_acquisitions() - before;
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(v2::TransactionRecord::OUTCOME_APPLIED, result->record().outcome());
    std::printf("engine lock acquisitions per %zu-op transaction: %llu\n", stages,
                static_cast<unsigned long long>(taken));
    EXPECT_EQ(2u, taken) << stages << " operations";
  }
}

TEST_F(TransactionCostTest, ChannelFrameTakesTheControlLockOnce) {
  grpc::ServerBuilder builder;
  bess::control::ConfigureControlServer(&builder);
  builder.RegisterService(service_.get());
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  ASSERT_NE(nullptr, server);
  auto stub = v2::Control::NewStub(server->InProcessChannel(grpc::ChannelArguments()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
  auto channel = stub->TransactionChannel(&context);
  uint64_t attempt = 0;
  auto exchange = [&](int transactions) {
    v2::TransactionChannelRequest request;
    request.set_attempt_id(++attempt);
    for (int i = 0; i < transactions; i++) {
      *request.add_transactions() =
          Request(1 + static_cast<size_t>(i) % kStages, attempt * 1000 + static_cast<uint64_t>(i));
    }
    const uint64_t before = control_->lock_acquisitions();
    EXPECT_TRUE(channel->Write(request));
    v2::TransactionChannelResponse response;
    EXPECT_TRUE(channel->Read(&response));
    const uint64_t taken = control_->lock_acquisitions() - before;
    EXPECT_EQ(transactions, response.results_size());
    for (const auto &result : response.results()) {
      EXPECT_EQ(v2::TransactionRecord::OUTCOME_APPLIED,
                result.transaction().record().outcome());
    }
    return taken;
  };
  const uint64_t one = exchange(1);
  const uint64_t full = exchange(bess::control::kMaxChannelTransactions);
  std::printf("control lock acquisitions per frame: 1 txn %llu, %d txns %llu\n",
              static_cast<unsigned long long>(one), bess::control::kMaxChannelTransactions,
              static_cast<unsigned long long>(full));
  EXPECT_EQ(1u, one);
  EXPECT_EQ(1u, full);
  EXPECT_TRUE(channel->WritesDone());
  EXPECT_TRUE(channel->Finish().ok());
  server->Shutdown();
}

}  // namespace
