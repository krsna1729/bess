// SPDX-License-Identifier: BSD-3-Clause

// G1.2c: the dataplane transaction RPC (Decision D-025) over a real
// in-process gRPC channel, against real ExactMatch and WildcardMatch modules:
// typed keys and values, per-operation results, idempotency by request_id,
// GetTransaction, the daemon epoch and resource discovery.

#include "control/api_v2.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "control/control_plane.h"
#include "control/dataplane_transactions.h"
#include "runtime/runtime_state.h"
#include "module.h"
#include "modules/exact_match.h"
#include "modules/wildcard_match.h"
#include "runtime/opts.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "port.h"

namespace {

namespace v2 = bess::pb::v2;
using bess::control::ControlPlane;
using bess::control::ControlV2Service;
using bess::control::DataplaneTransactions;

// Any::PackFrom, checked.
void Pack(google::protobuf::Any *any, const google::protobuf::Message &msg) {
  ASSERT_TRUE(any->PackFrom(msg));
}

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

// Both classify on 4 bytes at offset 26 and 2 bytes at offset 34.
template <typename Arg>
google::protobuf::Any TwoFields() {
  Arg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  f = arg.add_fields();
  f->set_offset(34);
  f->set_num_bytes(2);
  google::protobuf::Any packed;
  Pack(&packed, arg);
  return packed;
}

bess::pb::FieldData Int(uint64_t v) {
  bess::pb::FieldData d;
  d.set_value_int(v);
  return d;
}

v2::TransactionOp ExactRule(const std::string &module, uint32_t a, uint16_t b,
                            uint64_t gate) {
  v2::TransactionOp op;
  op.set_resource(module + "/rules");
  bess::pb::ExactMatchRuleKey key;
  *key.add_fields() = Int(a);
  *key.add_fields() = Int(b);
  Pack(op.mutable_key(), key);
  bess::pb::ExactMatchRuleValue value;
  value.set_gate(gate);
  Pack(op.mutable_value(), value);
  return op;
}

v2::TransactionOp EraseExact(const std::string &module, uint32_t a,
                             uint16_t b) {
  v2::TransactionOp op = ExactRule(module, a, b, 0);
  op.set_erase(true);
  op.clear_value();
  return op;
}

v2::TransactionOp WildRule(const std::string &module, uint32_t a,
                           uint32_t mask_a, int64_t priority, uint64_t gate) {
  v2::TransactionOp op;
  op.set_resource(module + "/rules");
  bess::pb::WildcardMatchRuleKey key;
  *key.add_values() = Int(a);
  *key.add_values() = Int(0);
  *key.add_masks() = Int(mask_a);
  *key.add_masks() = Int(0);
  Pack(op.mutable_key(), key);
  bess::pb::WildcardMatchRuleValue value;
  value.set_priority(priority);
  value.set_gate(gate);
  Pack(op.mutable_value(), value);
  return op;
}

class DataplaneTransactionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    control_plane_ = std::make_unique<ControlPlane>();
    ASSERT_TRUE(control_plane_->CreateModule(
        {"em0", "ExactMatch", TwoFields<bess::pb::ExactMatchArg>()}));
    ASSERT_TRUE(control_plane_->CreateModule(
        {"wm0", "WildcardMatch", TwoFields<bess::pb::WildcardMatchArg>()}));
    service_ = std::make_unique<ControlV2Service>(*control_plane_);
    grpc::ServerBuilder builder;
    bess::control::ConfigureControlServer(&builder);  // as bessd does
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(nullptr, server_);
    stub_ = v2::Control::NewStub(
        server_->InProcessChannel(grpc::ChannelArguments()));
  }

  void TearDown() override {
    server_->Shutdown();
    (void)control_plane_->Reset();
  }

  v2::ApplyTransactionResponse Apply(const v2::ApplyTransactionRequest &req,
                                     grpc::Status *status = nullptr) {
    grpc::ClientContext context;
    v2::ApplyTransactionResponse response;
    grpc::Status s = stub_->ApplyTransaction(&context, req, &response);
    if (status != nullptr) {
      *status = s;
    } else {
      EXPECT_TRUE(s.ok()) << s.error_message();
    }
    if (s.ok()) {
      // Zero (UNSPECIFIED) is never sent: an unset field cannot pass for
      // success.
      EXPECT_NE(response.record().outcome(),
                v2::TransactionRecord::OUTCOME_UNSPECIFIED);
      EXPECT_NE(response.record().visibility(),
                v2::TransactionRecord::VISIBILITY_UNSPECIFIED);
      EXPECT_EQ(response.record().ops_size(), req.ops_size());
      for (const auto &op : response.record().ops()) {
        EXPECT_NE(op.status(), v2::TransactionOpResult::STATUS_UNSPECIFIED);
      }
    }
    return response;
  }

  v2::GetTransactionResponse Get(const std::string &id) {
    grpc::ClientContext context;
    v2::GetTransactionRequest req;
    req.set_request_id(id);
    v2::GetTransactionResponse response;
    EXPECT_TRUE(stub_->GetTransaction(&context, req, &response).ok());
    return response;
  }

  static size_t Rules(const char *name) {
    Module *m = bess::runtime::runtime().modules().Find(name);
    if (auto *em = dynamic_cast<ExactMatch *>(m)) {
      bess::pb::ExactMatchConfig config;
      EXPECT_TRUE(em->GetRuntimeConfig(bess::pb::EmptyArg())
                      .data()
                      .UnpackTo(&config));
      return static_cast<size_t>(config.rules_size());
    }
    auto *wm = dynamic_cast<WildcardMatch *>(m);
    bess::pb::WildcardMatchConfig config;
    EXPECT_TRUE(
        wm->GetRuntimeConfig(bess::pb::EmptyArg()).data().UnpackTo(&config));
    return static_cast<size_t>(config.rules_size());
  }

  std::unique_ptr<ControlPlane> control_plane_;
  std::unique_ptr<ControlV2Service> service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<v2::Control::Stub> stub_;
};

TEST_F(DataplaneTransactionsTest, ResourcesAreDiscoverable) {
  grpc::ClientContext context;
  v2::ListTransactionResourcesResponse list;
  ASSERT_TRUE(stub_
                  ->ListTransactionResources(
                      &context, v2::ListTransactionResourcesRequest(), &list)
                  .ok());
  ASSERT_EQ(list.resources_size(), 2);
  EXPECT_EQ(list.resources(0).name(), "em0/rules");
  EXPECT_EQ(list.resources(0).key_type(), "bess.pb.ExactMatchRuleKey");
  EXPECT_EQ(list.resources(0).value_type(), "bess.pb.ExactMatchRuleValue");
  EXPECT_EQ(list.resources(1).name(), "wm0/rules");
  EXPECT_EQ(list.resources(1).key_type(), "bess.pb.WildcardMatchRuleKey");
  EXPECT_NE(list.daemon_epoch(), 0u);
}

TEST_F(DataplaneTransactionsTest, TypedOperationsAcrossModules) {
  v2::ApplyTransactionRequest req;
  *req.add_ops() = ExactRule("em0", 0x0a000001, 80, 3);
  *req.add_ops() = WildRule("wm0", 0x0a000000, 0xff000000, 5, 4);
  const auto r = Apply(req);
  ASSERT_EQ(r.record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(r.record().visibility(),
            v2::TransactionRecord::VISIBILITY_DEPENDENCY_ORDERED);
  ASSERT_EQ(r.record().ops_size(), 2);
  EXPECT_EQ(r.record().ops(1).status(), v2::TransactionOpResult::STATUS_APPLIED);
  EXPECT_FALSE(r.replayed());
  EXPECT_EQ(Rules("em0"), 1u);
  EXPECT_EQ(Rules("wm0"), 1u);

  // The server packs keys exactly as the commands do: the legacy delete
  // finds the rule the RPC added.
  bess::pb::ExactMatchCommandDeleteArg del;
  *del.add_fields() = Int(0x0a000001);
  *del.add_fields() = Int(80);
  auto *em = static_cast<ExactMatch *>(
      bess::runtime::runtime().modules().Find("em0"));
  EXPECT_FALSE(em->CommandDelete(del).has_error());
  EXPECT_EQ(Rules("em0"), 0u);
  // And the RPC erases what the command path added.
  ASSERT_FALSE(em->CommandAdd([] {
                   bess::pb::ExactMatchCommandAddArg add;
                   add.set_gate(9);
                   *add.add_fields() = Int(7);
                   *add.add_fields() = Int(8);
                   return add;
                 }())
                   .has_error());
  v2::ApplyTransactionRequest erase;
  *erase.add_ops() = EraseExact("em0", 7, 8);
  EXPECT_EQ(Apply(erase).record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(Rules("em0"), 0u);
}

TEST_F(DataplaneTransactionsTest, UndecodableOperationsRejectTheTransaction) {
  auto expect_rejected = [&](v2::TransactionOp bad, const std::string &why) {
    v2::ApplyTransactionRequest req;
    *req.add_ops() = ExactRule("em0", 1, 1, 1);
    *req.add_ops() = std::move(bad);
    const auto r = Apply(req);
    ASSERT_EQ(r.record().outcome(), v2::TransactionRecord::OUTCOME_REJECTED) << why;
    EXPECT_EQ(r.record().ops(0).status(),
              v2::TransactionOpResult::STATUS_NOT_APPLIED);
    EXPECT_EQ(r.record().ops(1).status(), v2::TransactionOpResult::STATUS_FAILED);
    EXPECT_NE(r.record().ops(1).error().find(why), std::string::npos)
        << r.record().ops(1).error();
    EXPECT_EQ(Rules("em0"), 0u);
  };
  v2::TransactionOp op = ExactRule("em0", 2, 2, 2);
  op.set_resource("nope/rules");
  expect_rejected(op, "unknown resource");
  op = ExactRule("em0", 2, 2, 2);
  Pack(op.mutable_key(), bess::pb::ExactMatchRuleValue());
  expect_rejected(op, "not a bess.pb.ExactMatchRuleKey");
  op = ExactRule("em0", 2, 2, 2);
  bess::pb::ExactMatchRuleKey one_field;
  *one_field.add_fields() = Int(2);
  Pack(op.mutable_key(), one_field);
  expect_rejected(op, "should have 2 fields");
  expect_rejected(ExactRule("em0", 2, 2, MAX_GATES + 5), "invalid gate");
  op = ExactRule("em0", 2, 2, 2);
  {
    bess::pb::ExactMatchRuleValue val;
    val.set_gate(2);
    val.set_action_id(5);
    Pack(op.mutable_value(), val);
  }
  expect_rejected(op, "'action_id' needs an 'action_resource'");
  op = ExactRule("em0", 2, 2, 2);
  op.set_erase(true);
  expect_rejected(op, "takes no value");
  // The engine's own rejection comes through the same way.
  expect_rejected(WildRule("wm0", 0x0a000001, 0xff000000, 1, 1),
                  "invalid pair of value and mask");
}

TEST_F(DataplaneTransactionsTest, RequestIdsMakeRetriesSafe) {
  v2::ApplyTransactionRequest req;
  req.set_request_id("session-42");
  *req.add_ops() = ExactRule("em0", 42, 1, 2);
  const auto first = Apply(req);
  ASSERT_EQ(first.record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(first.record().request_id(), "session-42");

  // A retry (the client never saw the first answer): the recorded outcome,
  // nothing applied twice.
  const auto again = Apply(req);
  EXPECT_TRUE(again.replayed());
  EXPECT_EQ(again.record().generation(), first.record().generation());
  EXPECT_EQ(again.daemon_epoch(), first.daemon_epoch());
  EXPECT_EQ(Rules("em0"), 1u);

  // The same id for something else: refused, typed.
  v2::ApplyTransactionRequest other = req;
  *other.mutable_ops(0) = ExactRule("em0", 43, 1, 2);
  grpc::Status status;
  Apply(other, &status);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::ABORTED);
  EXPECT_EQ(Rules("em0"), 1u);

  // GetTransaction after a timeout.
  const auto known = Get("session-42");
  EXPECT_TRUE(known.known());
  EXPECT_EQ(known.record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(known.daemon_epoch(), first.daemon_epoch());
  EXPECT_FALSE(Get("never-sent").known());

  // A rejected outcome is recorded too; CONFLICT (nothing attempted) is not,
  // so the client retries under the same id.
  v2::ApplyTransactionRequest stale;
  stale.set_request_id("stale");
  stale.set_expected_generation(first.record().generation() + 100);
  *stale.add_ops() = ExactRule("em0", 44, 1, 2);
  EXPECT_EQ(Apply(stale).record().outcome(), v2::TransactionRecord::OUTCOME_CONFLICT);
  EXPECT_FALSE(Get("stale").known());
  stale.set_expected_generation(first.record().generation());
  EXPECT_EQ(Apply(stale).record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_TRUE(Get("stale").known());
}

// A large transaction: 50,000 typed rules (about 5 MB on the wire). gRPC's
// default 4 MiB limit refuses it; the control server's settings take it.
TEST_F(DataplaneTransactionsTest, LargeTransactionsFitTheServerLimits) {
  v2::ApplyTransactionRequest req;
  for (uint32_t i = 0; i < 50000; i++) {
    *req.add_ops() = ExactRule("em0", i, static_cast<uint16_t>(i), 1);
  }
  const size_t bytes = req.ByteSizeLong();
  EXPECT_GT(bytes, size_t{4} << 20);
  EXPECT_LT(bytes, static_cast<size_t>(bess::control::kMaxMessageBytes));

  // A server with gRPC's defaults.
  auto plain_service = std::make_unique<ControlV2Service>(*control_plane_);
  grpc::ServerBuilder builder;
  builder.RegisterService(plain_service.get());
  auto plain = builder.BuildAndStart();
  auto plain_stub = v2::Control::NewStub(
      plain->InProcessChannel(grpc::ChannelArguments()));
  grpc::ClientContext context;
  v2::ApplyTransactionResponse refused;
  const grpc::Status status =
      plain_stub->ApplyTransaction(&context, req, &refused);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
  plain->Shutdown();
  EXPECT_EQ(Rules("em0"), 0u);

  const auto r = Apply(req);
  ASSERT_EQ(r.record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(Rules("em0"), 50000u);
  std::printf("[rpc] %zu bytes for 50000 typed rules (%.0f B/op)\n", bytes,
              static_cast<double>(bytes) / 50000);
}

// The record window is bounded, oldest first.
TEST(DataplaneTransactionsWindowTest, OldestRecordsAgeOut) {
  InitRuntimeOnce();
  bess::dataplane::TransactionEngine engine(bess::runtime::runtime().rcu());
  DataplaneTransactions transactions(engine, /*epoch=*/7);
  v2::ApplyTransactionRequest req;
  for (size_t i = 0; i <= DataplaneTransactions::kMaxRecords; i++) {
    req.set_request_id("r" + std::to_string(i));
    ASSERT_TRUE(transactions.Apply(req).has_value());
  }
  EXPECT_EQ(transactions.records(), DataplaneTransactions::kMaxRecords);
  EXPECT_FALSE(transactions.Get("r0").known());
  EXPECT_TRUE(transactions.Get("r1").known());
  EXPECT_EQ(transactions.Get("r1").daemon_epoch(), 7u);
}

}  // namespace
