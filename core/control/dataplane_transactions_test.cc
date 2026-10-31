// SPDX-License-Identifier: BSD-3-Clause

// G1.2c: the dataplane transaction RPC (Decision D-025) over a real
// in-process gRPC channel, against real ExactMatch and WildcardMatch modules:
// typed keys and values, per-operation results, idempotency by request_id,
// GetTransaction, the daemon epoch and resource discovery.

#include "control/api_v2.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <any>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <google/protobuf/wrappers.pb.h>

#include "control/control_plane.h"
#include "control/dataplane_transactions.h"
#include "dataplane/scope.h"
#include "dataplane/slot_resource.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "rcu/rcu_domain.h"
#include "runtime/runtime_state.h"
#include "module.h"
#include "modules/exact_match.h"
#include "modules/wildcard_match.h"
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
  // The library default (D-198): normal pages, malloc-backed, sandbox-safe.
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

  // Every request here is built against the daemon under test: it names that
  // daemon's epoch (D-106; the refusal of others is api_v2_test's).
  uint64_t Epoch() {
    grpc::ClientContext context;
    v2::ListTransactionResourcesResponse list;
    EXPECT_TRUE(stub_->ListTransactionResources(&context, {}, &list).ok());
    return list.daemon_epoch();
  }

  v2::ApplyTransactionResponse Apply(v2::ApplyTransactionRequest req,
                                     grpc::Status *status = nullptr) {
    req.set_daemon_epoch(Epoch());
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

  // The number of rules the module's "<name>/rules" resource holds (the
  // table the RPC writes).
  static size_t Rules(const std::string &module) {
    const bess::dataplane::Resource *r =
        bess::runtime::runtime().transactions().FindResource(module + "/rules");
    EXPECT_NE(nullptr, r) << module;
    const bess::framework::ResourceCodec *codec =
        r == nullptr ? nullptr : bess::framework::ResourceBindings::ProcessDefault().Find(*r);
    EXPECT_NE(nullptr, codec) << module;
    size_t rules = 0;
    if (codec != nullptr) {
      codec->Entries(
          [&](const google::protobuf::Message &, const google::protobuf::Message &) { rules++; });
    }
    return rules;
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
  ASSERT_EQ(list.resources_size(), 4);
  EXPECT_EQ(list.resources(0).name(), "em0/rules");
  EXPECT_EQ(list.resources(0).key_type(), "bess.pb.ExactMatchRuleKey");
  EXPECT_EQ(list.resources(0).value_type(), "bess.pb.ExactMatchRuleValue");
  EXPECT_EQ(list.resources(0).kind(), v2::TransactionResource::KIND_MAP);
  EXPECT_EQ(list.resources(1).name(), "em0/settings");
  EXPECT_EQ(list.resources(1).key_type(), "google.protobuf.Empty");
  EXPECT_EQ(list.resources(1).value_type(), "bess.pb.ExactMatchSettings");
  EXPECT_EQ(list.resources(1).kind(), v2::TransactionResource::KIND_SINGLETON);
  EXPECT_FALSE(list.resources(1).requires_paused_workers());
  EXPECT_EQ(list.resources(2).name(), "wm0/rules");
  EXPECT_EQ(list.resources(2).key_type(), "bess.pb.WildcardMatchRuleKey");
  EXPECT_EQ(list.resources(3).name(), "wm0/settings");
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

  // An erase op names the same key the upsert wrote: the RPC reads back what
  // it wrote (the engine's key encoding is the codec's).
  v2::ApplyTransactionRequest erase;
  *erase.add_ops() = EraseExact("em0", 0x0a000001, 80);
  EXPECT_EQ(Apply(erase).record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(Rules("em0"), 0u);
  EXPECT_EQ(Rules("wm0"), 1u) << "the other module's rule stays";
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
  // Either module's bad op rejects the other's (was dataplane_transactions.py).
  expect_rejected(WildRule("wm0", 0x0b000000, 0xff000000, 1, MAX_GATES + 5), "invalid gate");
  op = ExactRule("em0", 2, 2, 2);
  {
    bess::pb::ExactMatchRuleValue val;
    val.set_gate(2);
    val.set_result("x");
    Pack(op.mutable_value(), val);
  }
  expect_rejected(op, "the schema's record is 0");
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

// D-207: a record keeps its id, not one result per operation; a replay and
// GetTransaction still answer every operation exactly as the first answer.
TEST_F(DataplaneTransactionsTest, RecordsKeepOnlyTheResultsThatDiffer) {
  v2::ApplyTransactionRequest bulk;
  bulk.set_request_id("bulk");
  for (uint32_t i = 0; i < 1000; i++) {
    *bulk.add_ops() = ExactRule("em0", i, 1, 2);
  }
  const auto first = Apply(bulk);
  ASSERT_EQ(first.record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  ASSERT_EQ(first.record().ops_size(), 1000);
  EXPECT_EQ(Get("bulk").record().SerializeAsString(), first.record().SerializeAsString());
  const auto again = Apply(bulk);
  EXPECT_TRUE(again.replayed());
  EXPECT_EQ(again.record().SerializeAsString(), first.record().SerializeAsString());

  // A rejection keeps its failed operation, where it was, with its error.
  v2::ApplyTransactionRequest bad;
  bad.set_request_id("bad");
  for (uint32_t i = 0; i < 10; i++) {
    *bad.add_ops() = ExactRule("em0", 5000 + i, 1, 2);
  }
  bad.mutable_ops(7)->set_resource("nope/rules");
  const auto rejected = Apply(bad);
  ASSERT_EQ(rejected.record().outcome(), v2::TransactionRecord::OUTCOME_REJECTED);
  ASSERT_EQ(rejected.record().ops(7).status(), v2::TransactionOpResult::STATUS_FAILED);
  EXPECT_EQ(Get("bad").record().SerializeAsString(), rejected.record().SerializeAsString());
}

// D-207: at the memory limit an upsert is refused with nothing applied, and
// an erase-only transaction still applies (the erase headroom, half the
// reserve), record and all: a full daemon can always be emptied.
TEST_F(DataplaneTransactionsTest, ErasesApplyAtTheLimit) {
  v2::ApplyTransactionRequest load;
  load.set_request_id("load");
  for (uint32_t i = 0; i < 100; i++) {
    *load.add_ops() = ExactRule("em0", i, 1, 2);
  }
  ASSERT_EQ(Apply(load).record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);

  bess::utils::MemoryPolicy saved = bess::utils::memory_policy();
  bess::utils::MemoryPolicy full = saved;
  // 4 MiB past limit - reserve; the erase headroom is half the reserve.
  full.reserve_bytes = size_t{512} << 20u;
  full.limit_bytes = bess::utils::GetMemoryStatus().used_bytes +
                     full.reserve_bytes - (size_t{4} << 20u);
  full.limit_source = "test";
  bess::utils::SetMemoryPolicy(full);
  v2::ApplyTransactionRequest more;
  more.set_request_id("more");
  *more.add_ops() = ExactRule("em0", 1000, 1, 2);
  grpc::Status refused;
  Apply(more, &refused);
  v2::ApplyTransactionRequest drain;
  drain.set_request_id("drain");
  for (uint32_t i = 0; i < 100; i++) {
    *drain.add_ops() = EraseExact("em0", i, 1);
  }
  const auto drained = Apply(drain);
  bess::utils::SetMemoryPolicy(std::move(saved));
  EXPECT_EQ(refused.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_EQ(drained.record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(Rules("em0"), 0u);
  EXPECT_TRUE(Get("drain").known());
  EXPECT_FALSE(Get("more").known());
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
  bess::framework::ResourceBindings bindings;
  DataplaneTransactions transactions(engine, bindings, /*epoch=*/7);
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

// D-207: with request ids as clients send them (a 36-character UUID), the
// count binds, never the bytes: a record ages out exactly when it did before
// the byte bound existed, oldest first.
TEST(DataplaneTransactionsWindowTest, RealisticRecordsAgeOutByCountNotBytes) {
  InitRuntimeOnce();
  bess::dataplane::TransactionEngine engine(bess::runtime::runtime().rcu());
  bess::framework::ResourceBindings bindings;
  DataplaneTransactions transactions(engine, bindings, /*epoch=*/7);
  auto id = [](size_t i) { return std::format("{:08x}-0000-4000-8000-{:012x}", i, i); };
  v2::ApplyTransactionRequest req;
  for (size_t i = 0; i < DataplaneTransactions::kMaxRecords + 2; i++) {
    req.set_request_id(id(i));
    ASSERT_TRUE(transactions.Apply(req).has_value());
  }
  EXPECT_EQ(transactions.records(), DataplaneTransactions::kMaxRecords);
  EXPECT_LT(transactions.bytes(), DataplaneTransactions::kMaxRecordBytes / 2);
  EXPECT_FALSE(transactions.Get(id(0)).known());
  EXPECT_FALSE(transactions.Get(id(1)).known());
  EXPECT_TRUE(transactions.Get(id(2)).known());
  EXPECT_TRUE(transactions.Get(id(DataplaneTransactions::kMaxRecords + 1)).known());
}

// D-207: the window is bounded in charged bytes too, oldest out first.
TEST(DataplaneTransactionsWindowTest, TheWindowIsBoundedInBytes) {
  InitRuntimeOnce();
  bess::dataplane::TransactionEngine engine(bess::runtime::runtime().rcu());
  bess::framework::ResourceBindings bindings;
  DataplaneTransactions transactions(engine, bindings, /*epoch=*/7);
  // About 4 KiB a record (its 4000-byte id): the bytes bind first.
  const std::string pad(4000, 'x');
  v2::ApplyTransactionRequest req;
  for (size_t i = 0; i < 2000; i++) {
    req.set_request_id(pad + std::to_string(i));
    ASSERT_TRUE(transactions.Apply(req).has_value());
    ASSERT_LE(transactions.bytes(), DataplaneTransactions::kMaxRecordBytes);
  }
  EXPECT_LT(transactions.records(), 2000u);
  EXPECT_GT(transactions.records(), 500u);
  // Oldest out first: what is kept is the newest records() transactions.
  const size_t oldest = 2000 - transactions.records();
  EXPECT_FALSE(transactions.Get(pad + std::to_string(oldest - 1)).known());
  EXPECT_TRUE(transactions.Get(pad + std::to_string(oldest)).known());
  EXPECT_TRUE(transactions.Get(pad + "1999").known());
}

// The window is control-plane glibc charged to the daemon's scope (D-206,
// class C): every record's id, serialized record and window slot moves
// bessd/transaction_records, all of it glibc, and it is all given back with
// the window. The record is charged before the transaction runs: at the
// memory limit the transaction is refused with nothing applied, and its
// retry applies once. A record reads back byte for byte.
TEST(DataplaneTransactionsWindowTest, RecordsAreChargedToTheDaemonScope) {
  InitRuntimeOnce();
  const auto row = [] {
    for (const auto &use : bess::framework::MemoryAccounts::List()) {
      if (use.module == "bessd" && use.resource == "transaction_records") {
        return use;
      }
    }
    return bess::framework::ModuleMemoryUse{};
  };
  bess::dataplane::TransactionEngine engine(bess::runtime::runtime().rcu());
  bess::framework::ResourceBindings bindings;
  {
    DataplaneTransactions transactions(engine, bindings, /*epoch=*/7);
    v2::ApplyTransactionRequest req;
    req.set_request_id("a-request-id-longer-than-sso-0");
    const auto first = transactions.Apply(req);
    ASSERT_TRUE(first.has_value());
    const size_t one = row().admitted_bytes;
    // The id's heap text, the record's bytes, the node, buckets, the deque.
    EXPECT_GE(one, 4 * bess::utils::MallocChunkBytes(1));
    EXPECT_EQ(row().glibc_bytes, one);
    const auto got = transactions.Get("a-request-id-longer-than-sso-0");
    ASSERT_TRUE(got.known());
    EXPECT_EQ(got.record().SerializeAsString(),
              first->record().SerializeAsString());
    const auto replayed = transactions.Apply(req);
    ASSERT_TRUE(replayed.has_value());
    EXPECT_TRUE(replayed->replayed());
    EXPECT_EQ(replayed->record().SerializeAsString(),
              first->record().SerializeAsString());
    req.set_request_id("a-request-id-longer-than-sso-1");
    ASSERT_TRUE(transactions.Apply(req).has_value());
    EXPECT_GT(row().admitted_bytes, one);

    const bess::utils::MemoryPolicy saved = bess::utils::memory_policy();
    bess::utils::MemoryPolicy tight = saved;
    tight.limit_bytes = size_t{1} << 20;  // far below what the test holds
    tight.limit_source = "test";
    tight.reserve_bytes = 0;
    bess::utils::SetMemoryPolicy(tight);
    const uint64_t generation = engine.generation();
    const size_t held = row().admitted_bytes;
    req.set_request_id("a-request-id-longer-than-sso-2");
    const auto refused = transactions.Apply(req);
    bess::utils::SetMemoryPolicy(saved);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code,
              bess::control::ControlErrorCode::kResourceExhausted);
    EXPECT_EQ(engine.generation(), generation) << "applied though refused";
    EXPECT_EQ(row().admitted_bytes, held);
    EXPECT_FALSE(transactions.Get("a-request-id-longer-than-sso-2").known());
    const auto retried = transactions.Apply(req);
    ASSERT_TRUE(retried.has_value());
    EXPECT_FALSE(retried->replayed());
    EXPECT_TRUE(transactions.Get("a-request-id-longer-than-sso-2").known());
  }
  EXPECT_EQ(row().admitted_bytes, 0u);
}

// -- M8 (D-050): the consistency level over the wire ----------------------------------

struct SessionVersion {
  uint32_t epoch = 0;
};
struct MeterObject {
  uint32_t rate = 0;
};

struct MeterTag;
using MeterKey = bess::dataplane::StrongId<MeterTag, uint32_t>;

// The codecs' read-back: every live object of `table` as (id, `field`).
template <typename Id, typename T>
auto ReadBack(const bess::dataplane::SlotTable<Id, T> &table,
              uint32_t T::*field) {
  return [&table, field](const auto &visit) {
    for (size_t i = 1; i <= table.capacity(); i++) {
      const T *object = table.Current(Id(static_cast<uint32_t>(i)));
      if (object == nullptr) {
        continue;
      }
      google::protobuf::UInt32Value key, value;
      key.set_value(static_cast<uint32_t>(i));
      value.set_value(object->*field);
      visit(key, value);
    }
  };
}

// A scope table ("sessions": one immutable SessionVersion per scope) and an
// ordinary resource ("meters") behind the real RPC server and codecs; keys and
// values travel as UInt32Value messages. The scope table is the in-tree
// acceptance application of the scope-snapshot level; the meters cannot
// provide it.
class ConsistencyRpcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    InitRuntimeOnce();
    using bess::framework::TypedCodec;
    using google::protobuf::UInt32Value;
    ASSERT_TRUE(engine_.Register(&sessions_res_).has_value());
    ASSERT_TRUE(engine_.Register(&meters_res_).has_value());
    session_binding_ = bindings_.Bind(
        sessions_res_,
        std::make_shared<TypedCodec<UInt32Value, UInt32Value>>(
            [](const UInt32Value &key)
                -> std::expected<bess::dataplane::ResourceKey, std::string> {
              return bess::dataplane::EncodeKey(
                  bess::dataplane::ScopeId(key.value()));
            },
            [](const UInt32Value &value) -> std::expected<std::any, std::string> {
              return std::any(SessionVersion{value.value()});
            },
            ReadBack(sessions_, &SessionVersion::epoch)));
    meter_binding_ = bindings_.Bind(
        meters_res_,
        std::make_shared<TypedCodec<UInt32Value, UInt32Value>>(
            [](const UInt32Value &key)
                -> std::expected<bess::dataplane::ResourceKey, std::string> {
              return bess::dataplane::EncodeKey(MeterKey(key.value()));
            },
            [](const UInt32Value &value) -> std::expected<std::any, std::string> {
              return std::any(MeterObject{value.value()});
            },
            ReadBack(meters_, &MeterObject::rate)));
    service_ = std::make_unique<ControlV2Service>(control_plane_, engine_,
                                                  bindings_);
    grpc::ServerBuilder builder;
    bess::control::ConfigureControlServer(&builder);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(nullptr, server_);
    stub_ = v2::Control::NewStub(
        server_->InProcessChannel(grpc::ChannelArguments()));
  }

  void TearDown() override {
    server_->Shutdown();
    while (engine_.ReclaimRetired() != 0) {
    }
    domain_.Drain();
  }

  static v2::TransactionOp Op(const char *resource, uint32_t key,
                              uint32_t value) {
    v2::TransactionOp op;
    op.set_resource(resource);
    google::protobuf::UInt32Value k, v;
    k.set_value(key);
    v.set_value(value);
    Pack(op.mutable_key(), k);
    Pack(op.mutable_value(), v);
    return op;
  }
  static v2::ApplyTransactionRequest Request(
      const std::string &id, v2::ApplyTransactionRequest::Consistency level,
      std::initializer_list<v2::TransactionOp> ops) {
    v2::ApplyTransactionRequest req;
    req.set_request_id(id);
    req.set_consistency(level);
    for (const auto &op : ops) {
      *req.add_ops() = op;
    }
    return req;
  }

  // Built against the daemon under test: it names that daemon's epoch (D-106).
  grpc::Status Apply(v2::ApplyTransactionRequest req,
                     v2::ApplyTransactionResponse *response,
                     v2::ErrorDetail *detail = nullptr) {
    {
      grpc::ClientContext context;
      v2::ListTransactionResourcesResponse list;
      EXPECT_TRUE(stub_->ListTransactionResources(&context, {}, &list).ok());
      req.set_daemon_epoch(list.daemon_epoch());
    }
    grpc::ClientContext context;
    const grpc::Status status =
        stub_->ApplyTransaction(&context, req, response);
    if (detail != nullptr) {
      const auto &trailers = context.GetServerTrailingMetadata();
      const auto it = trailers.find("bess-error-bin");
      if (it != trailers.end()) {
        EXPECT_TRUE(detail->ParseFromString(
            std::string(it->second.data(), it->second.size())));
      }
    }
    return status;
  }

  const SessionVersion *Session(uint32_t scope) const {
    return sessions_.Lookup(bess::dataplane::ScopeId(scope));
  }

  // A retire budget of 4 (half the high water): BUSY is reachable with a few
  // meters.
  bess::rcu::RcuDomain domain_{8, /*retire_high_water=*/8};
  bess::dataplane::ScopeTable<SessionVersion> sessions_{16};
  bess::dataplane::SlotTable<MeterKey, MeterObject> meters_{16};
  bess::dataplane::ScopeResource<SessionVersion> sessions_res_{"sessions",
                                                              sessions_};
  bess::dataplane::SlotResource<MeterKey, MeterObject> meters_res_{"meters",
                                                                   meters_};
  bess::dataplane::TransactionEngine engine_{domain_};
  bess::framework::ResourceBindings bindings_;
  bess::framework::ResourceBinding session_binding_, meter_binding_;
  ControlPlane control_plane_;
  std::unique_ptr<ControlV2Service> service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<v2::Control::Stub> stub_;
};

using Level = v2::ApplyTransactionRequest;

TEST_F(ConsistencyRpcTest, ScopeSnapshotIsRequestedAppliedAndReported) {
  v2::ApplyTransactionResponse response;
  const auto req = Request("s1", Level::CONSISTENCY_SCOPE_SNAPSHOT,
                           {Op("sessions", 1, 5), Op("sessions", 2, 5)});
  ASSERT_TRUE(Apply(req, &response).ok());
  EXPECT_EQ(response.record().outcome(),
            v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(response.record().visibility(),
            v2::TransactionRecord::VISIBILITY_SCOPE_SNAPSHOT);
  ASSERT_NE(Session(1), nullptr);
  EXPECT_EQ(Session(1)->epoch, 5u);
  EXPECT_EQ(Session(2)->epoch, 5u);

  // A retry replays the record, level included; the same id asking for the
  // other level is a different request.
  v2::ApplyTransactionResponse again;
  ASSERT_TRUE(Apply(req, &again).ok());
  EXPECT_TRUE(again.replayed());
  EXPECT_EQ(again.record().visibility(),
            v2::TransactionRecord::VISIBILITY_SCOPE_SNAPSHOT);
  v2::ApplyTransactionResponse other;
  EXPECT_EQ(Apply(Request("s1", Level::CONSISTENCY_REFERENTIAL,
                          {Op("sessions", 1, 5), Op("sessions", 2, 5)}),
                  &other)
                .error_code(),
            grpc::StatusCode::ABORTED);
}

TEST_F(ConsistencyRpcTest, UnsetAndReferentialAreTheDependencyOrderedLevel) {
  for (const auto level :
       {Level::CONSISTENCY_UNSPECIFIED, Level::CONSISTENCY_REFERENTIAL}) {
    v2::ApplyTransactionResponse response;
    ASSERT_TRUE(Apply(Request("", level,
                              {Op("meters", 1, 100), Op("sessions", 1, 6)}),
                      &response)
                    .ok());
    EXPECT_EQ(response.record().outcome(),
              v2::TransactionRecord::OUTCOME_APPLIED);
    EXPECT_EQ(response.record().visibility(),
              v2::TransactionRecord::VISIBILITY_DEPENDENCY_ORDERED);
  }
}

// The point of M8: a request for the scope-snapshot level that includes a
// resource which cannot provide it is refused, typed, with the reason -- and
// nothing, of the scope resource either, is applied. It is not recorded (it
// attempted nothing), so the corrected request goes through under the same id.
TEST_F(ConsistencyRpcTest, UnsupportedLevelIsATypedErrorNeverADowngrade) {
  v2::ApplyTransactionResponse applied;
  ASSERT_TRUE(Apply(Request("", Level::CONSISTENCY_SCOPE_SNAPSHOT,
                            {Op("sessions", 1, 1)}),
                    &applied)
                  .ok());
  const uint64_t generation = engine_.generation();

  const auto mixed = Request("m1", Level::CONSISTENCY_SCOPE_SNAPSHOT,
                             {Op("sessions", 1, 2), Op("meters", 7, 700)});
  v2::ApplyTransactionResponse response;
  v2::ErrorDetail detail;
  const grpc::Status status = Apply(mixed, &response, &detail);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_EQ(detail.code(), v2::ErrorDetail::UNSUPPORTED_TRANSACTION);
  EXPECT_EQ(detail.field(), "consistency");
  EXPECT_EQ(detail.object(), "meters");
  EXPECT_NE(detail.message().find("scope-snapshot"), std::string::npos)
      << detail.message();
  EXPECT_EQ(Session(1)->epoch, 1u);  // the scope operation did not apply
  EXPECT_EQ(meters_.Lookup(MeterKey(7)), nullptr);
  EXPECT_EQ(engine_.generation(), generation);

  // Not recorded...
  grpc::ClientContext context;
  v2::GetTransactionRequest get;
  get.set_request_id("m1");
  v2::GetTransactionResponse known;
  ASSERT_TRUE(stub_->GetTransaction(&context, get, &known).ok());
  EXPECT_FALSE(known.known());
  // ...and the same operations are fine as the referential transaction they
  // can be, under the same id.
  v2::ApplyTransactionResponse referential;
  ASSERT_TRUE(Apply(Request("m1", Level::CONSISTENCY_REFERENTIAL,
                            {Op("sessions", 1, 2), Op("meters", 7, 700)}),
                    &referential)
                  .ok());
  EXPECT_EQ(referential.record().outcome(),
            v2::TransactionRecord::OUTCOME_APPLIED);
  EXPECT_EQ(referential.record().visibility(),
            v2::TransactionRecord::VISIBILITY_DEPENDENCY_ORDERED);
  EXPECT_EQ(Session(1)->epoch, 2u);
}

// A level this build does not know is not read as the default.
TEST_F(ConsistencyRpcTest, UnknownLevelIsRefused) {
  v2::ApplyTransactionRequest req;
  *req.add_ops() = Op("sessions", 1, 9);
  req.set_consistency(static_cast<Level::Consistency>(99));
  v2::ApplyTransactionResponse response;
  v2::ErrorDetail detail;
  EXPECT_EQ(Apply(req, &response, &detail).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(detail.field(), "consistency");
  EXPECT_EQ(Session(1), nullptr);
}

// BUSY -- reclamation behind a reader that has not quiesced -- is waited out
// by the daemon within a bound (kBusyRetries sleeps from kBusyFirstBackoff):
// a reader that quiesces during the wait is never seen by the client; one
// that stays stalled gets BUSY once the bound is spent, recorded nowhere.
TEST_F(ConsistencyRpcTest, TheDaemonWaitsOutBusyWithinABound) {
  v2::ApplyTransactionResponse r;
  for (uint32_t k = 1; k <= 8; k++) {
    ASSERT_TRUE(Apply(Request("", Level::CONSISTENCY_REFERENTIAL, {Op("meters", k, k)}), &r).ok());
  }
  auto erase = [](uint32_t first, uint32_t n, const std::string &id) {
    v2::ApplyTransactionRequest req = Request(id, Level::CONSISTENCY_REFERENTIAL, {});
    for (uint32_t k = first; k < first + n; k++) {
      v2::TransactionOp *op = req.add_ops();
      *op = Op("meters", k, 0);
      op->set_erase(true);
      op->clear_value();
    }
    return req;
  };
  constexpr bess::rcu::ReaderId kReader = 3;
  ASSERT_TRUE(domain_.Register(kReader).has_value());
  domain_.Online(kReader);  // stalled
  // However the test ends, the reader leaves: teardown must not wait for it.
  struct Leave {
    bess::rcu::RcuDomain &domain;
    ~Leave() {
      if (domain.IsOnline(kReader)) {
        domain.Offline(kReader);
      }
      domain.Unregister(kReader);
    }
  } leave{domain_};
  // Applied; its removals wait for the reader and fill the budget.
  ASSERT_TRUE(Apply(erase(1, 4, ""), &r).ok());
  ASSERT_EQ(r.record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED);

  using Outcome = bess::dataplane::TransactionEngine::Outcome;
  auto count = [&](Outcome o) { return engine_.outcome_counts()[static_cast<size_t>(o)]; };
  const uint64_t busy = count(Outcome::kBusy), applied = count(Outcome::kApplied);
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(Apply(erase(5, 1, "stalled"), &r).ok());
  const auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(r.record().outcome(), v2::TransactionRecord::OUTCOME_BUSY);
  EXPECT_GE(waited, bess::control::kBusyFirstBackoff * ((1 << bess::control::kBusyRetries) - 1))
      << "answered BUSY without waiting";
  EXPECT_LT(waited, std::chrono::seconds(2)) << "the wait is not bounded";
  // One answer, however many attempts it took.
  EXPECT_EQ(count(Outcome::kBusy), busy + 1);
  EXPECT_EQ(engine_.busy_retried(), static_cast<uint64_t>(bess::control::kBusyRetries));
  {
    grpc::ClientContext context;
    v2::GetTransactionRequest get;
    get.set_request_id("stalled");
    v2::GetTransactionResponse known;
    ASSERT_TRUE(stub_->GetTransaction(&context, get, &known).ok());
    EXPECT_FALSE(known.known()) << "a BUSY attempted nothing: nothing to record";
  }

  // The reader quiesces a moment after the request arrives: the daemon's
  // wait covers it.
  {
    std::jthread quiesce([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      domain_.Quiescent(kReader);
      domain_.Offline(kReader);
    });
    ASSERT_TRUE(Apply(erase(5, 1, "brief"), &r).ok());
  }
  EXPECT_EQ(r.record().outcome(), v2::TransactionRecord::OUTCOME_APPLIED)
      << "the client saw a BUSY the daemon could have waited out";
  EXPECT_EQ(count(Outcome::kBusy), busy + 1) << "a BUSY waited out counted as an answer";
  EXPECT_EQ(count(Outcome::kApplied), applied + 1);
}

}  // namespace
