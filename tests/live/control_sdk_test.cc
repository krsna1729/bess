// SPDX-License-Identifier: BSD-3-Clause

// The C++ control SDK against a live bessd (was
// bessctl/module_tests/control_sdk.py, which drove pybess.sdk): a transaction
// steers packets, a stale one changes nothing, an erase returns them to the
// default gate. Black-box recovery, through the daemon's real transport with
// ApplyTransaction's fate scripted: an answer lost after the daemon applied is
// recovered through GetTransaction (applied once); a request lost before it
// arrived is sent again; a restart between the send and the question is
// kDaemonRestarted, the old handles are refused, and a pipeline built under
// the old epoch is refused by the new daemon. Request-id replay, conflicts,
// typed keys, discovery and the pipeline round trip are conformance cases
// (sdk/conformance/cases.json, tests/live/conformance_test.cc).

#include <unistd.h>

#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "live_daemon.h"
#include "steering.h"

namespace {

using bess::live::LiveTest;
using bess::live::Types;
using bess::sdk::Deadline;
using bess::sdk::ErrorKind;
using bess::sdk::Trailers;
namespace v2 = bess::pb::v2;

// 65.43.21.0 -> 12.34.56.78.
constexpr uint32_t kSip = 0x412B1500;
constexpr uint32_t kDip = 0x0C22384E;

enum class Fate { kDeliver, kLoseAnswer, kLoseRequest };

// The daemon's real transport, with ApplyTransaction's fate scripted per
// call: delivered; sent, then the answer lost (after `then`); or never sent.
class LossyTransport final : public bess::sdk::Transport {
 public:
  LossyTransport(const std::string &address, std::deque<Fate> fates,
                 std::function<void()> then = {})
      : real_(bess::sdk::GrpcTransport(address)), fates_(std::move(fates)), then_(std::move(then)) {}

  int sent() const { return sent_; }

  grpc::Status ApplyTransaction(Deadline deadline, const v2::ApplyTransactionRequest &req,
                                v2::ApplyTransactionResponse *resp, Trailers *trailers) override {
    Fate fate = Fate::kDeliver;
    if (!fates_.empty()) {
      fate = fates_.front();
      fates_.pop_front();
    }
    if (fate == Fate::kLoseRequest) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "lost");
    }
    sent_++;
    const grpc::Status status = real_->ApplyTransaction(deadline, req, resp, trailers);
    if (fate == Fate::kLoseAnswer) {
      if (then_) {
        then_();
      }
      resp->Clear();
      if (trailers != nullptr) {
        trailers->clear();
      }
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "lost");
    }
    return status;
  }

#define FORWARD(Rpc)                                                                      \
  grpc::Status Rpc(Deadline d, const v2::Rpc##Request &q, v2::Rpc##Response *r,           \
                   Trailers *t) override {                                                \
    return real_->Rpc(d, q, r, t);                                                        \
  }
  FORWARD(GetTransaction)
  FORWARD(ListTransactionResources)
  FORWARD(GetCapabilities)
  FORWARD(GetSchema)
  FORWARD(ListMetrics)
  FORWARD(GetPipeline)
  FORWARD(ValidatePipeline)
  FORWARD(DiffPipeline)
  FORWARD(PlanPipeline)
  FORWARD(ApplyPipeline)
  FORWARD(RunModuleCommand)
  FORWARD(ImportPlugin)
  FORWARD(UnloadPlugin)
  FORWARD(GetPortStatus)
  FORWARD(GetPacketPools)
  FORWARD(SetCapture)
  FORWARD(PauseWorkers)
  FORWARD(ResumeWorkers)
  FORWARD(Shutdown)
#undef FORWARD

  std::unique_ptr<bess::sdk::EventStream> WatchEvents(const v2::WatchEventsRequest &req,
                                                      grpc::Status *status) override {
    return real_->WatchEvents(req, status);
  }

 private:
  std::shared_ptr<bess::sdk::Transport> real_;
  std::deque<Fate> fates_;
  std::function<void()> then_;
  int sent_ = 0;
};

class ControlSdkTest : public LiveTest {
 protected:
  void SetUp() override {
    LiveTest::SetUp();
    types_.emplace(client());
    ASSERT_NO_FATAL_FAILURE(bess::live::ApplySteeringPipeline(*types_, client(), kSip, kDip));
  }

  std::unique_ptr<google::protobuf::Message> Key() {
    return types_->Make("bess.pb.ExactMatchRuleKey", bess::live::RuleKeyJson(kSip, kDip));
  }
  std::unique_ptr<google::protobuf::Message> Gate(int gate) {
    return types_->Make("bess.pb.ExactMatchRuleValue", std::format(R"({{"gate": {}}})", gate));
  }

  double AppliedTransactions(bess::sdk::Client &c) {
    auto samples = c.Metrics();
    EXPECT_TRUE(samples) << samples.error().message;
    if (samples) {
      for (const v2::MetricSample &s : *samples) {
        const auto outcome = s.labels().find("outcome");
        if (s.name() == "bess_transactions_total" && outcome != s.labels().end() &&
            outcome->second == "applied") {
          return s.value();
        }
      }
    }
    ADD_FAILURE() << "no bess_transactions_total{outcome=\"applied\"}";
    return -1;
  }

  std::optional<Types> types_;
};

TEST_F(ControlSdkTest, TransactionsSteerPackets) {
  auto rules = client().Resource(bess::live::kSteeringResource);
  ASSERT_TRUE(rules) << rules.error().message;
  EXPECT_EQ("bess.pb.ExactMatchRuleKey", rules->key_type);
  EXPECT_EQ("bess.pb.ExactMatchRuleValue", rules->value_type);
  EXPECT_TRUE(bess::live::SteeredTo(client(), 3)) << "no rule: the default gate";

  auto tx = client().NewTransaction();
  ASSERT_TRUE(tx.Upsert(*rules, *Key(), *Gate(1)));
  auto applied = tx.Commit();
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_TRUE(bess::live::SteeredTo(client(), 1));

  // A stale expected generation changes nothing.
  auto stale = client().NewTransaction({.expected_generation = applied->generation - 1});
  ASSERT_TRUE(stale.Upsert(*rules, *Key(), *Gate(2)));
  auto refused = stale.Commit();
  ASSERT_FALSE(refused);
  EXPECT_EQ(ErrorKind::kConflict, refused.error().kind) << refused.error().message;
  EXPECT_TRUE(bess::live::SteeredTo(client(), 1));

  auto erase = client().NewTransaction();
  ASSERT_TRUE(erase.Erase(*rules, *Key()));
  ASSERT_TRUE(erase.Commit());
  EXPECT_TRUE(bess::live::SteeredTo(client(), 3));
}

TEST_F(ControlSdkTest, ALostAnswerIsRecoveredAndALostRequestResent) {
  // Applied, answer lost: the client asks, and reports the recorded outcome
  // -- one application, not two.
  {
    auto lossy = std::make_shared<LossyTransport>(daemon().address(),
                                                  std::deque<Fate>{Fate::kLoseAnswer});
    bess::sdk::Client c(lossy);
    auto rules = c.Resource(bess::live::kSteeringResource);
    ASSERT_TRUE(rules) << rules.error().message;
    const double before = AppliedTransactions(c);
    auto tx = c.NewTransaction();
    ASSERT_TRUE(tx.Upsert(*rules, *Key(), *Gate(1)));
    auto applied = tx.Commit();
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied->replayed);
    EXPECT_EQ(1, lossy->sent());
    EXPECT_EQ(before + 1, AppliedTransactions(c));
    EXPECT_TRUE(bess::live::SteeredTo(client(), 1));
  }
  // Never arrived: "not known", so the same request is sent again.
  {
    auto lossy = std::make_shared<LossyTransport>(daemon().address(),
                                                  std::deque<Fate>{Fate::kLoseRequest});
    bess::sdk::Client c(lossy);
    auto rules = c.Resource(bess::live::kSteeringResource);
    ASSERT_TRUE(rules) << rules.error().message;
    auto tx = c.NewTransaction();
    ASSERT_TRUE(tx.Upsert(*rules, *Key(), *Gate(2)));
    auto applied = tx.Commit();
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_FALSE(applied->replayed);
    EXPECT_EQ(1, lossy->sent());
    EXPECT_TRUE(bess::live::SteeredTo(client(), 2));
  }
}

TEST_F(ControlSdkTest, ARestartBetweenSendAndQuestionIsDaemonRestarted) {
  auto before = daemon().NewClient()->Pipeline();
  ASSERT_TRUE(before) << before.error().message;
  auto lossy = std::make_shared<LossyTransport>(
      daemon().address(), std::deque<Fate>{Fate::kLoseAnswer},
      [this] { daemon().RestartInPlace(); });
  bess::sdk::Client c(lossy, {.attempt_timeout = std::chrono::seconds(10), .attempts = 6});
  auto rules = c.Resource(bess::live::kSteeringResource);
  ASSERT_TRUE(rules) << rules.error().message;
  const auto epoch = c.DaemonEpoch();
  ASSERT_TRUE(epoch);

  auto tx = c.NewTransaction();
  ASSERT_TRUE(tx.Upsert(*rules, *Key(), *Gate(1)));
  auto lost = tx.Commit();
  ASSERT_FALSE(lost);
  EXPECT_EQ(ErrorKind::kDaemonRestarted, lost.error().kind) << lost.error().message;
  EXPECT_NE(epoch, c.DaemonEpoch());

  // The old handle belongs to the lost daemon: refused, never sent.
  auto again = c.NewTransaction();
  auto stale_handle = again.Upsert(*rules, *Key(), *Gate(1));
  ASSERT_FALSE(stale_handle);
  EXPECT_EQ(ErrorKind::kStaleResource, stale_handle.error().kind);
  EXPECT_EQ(1, lossy->sent());
  // The restarted daemon has no such module.
  auto gone = c.Resource(bess::live::kSteeringResource);
  ASSERT_FALSE(gone);
  EXPECT_EQ(ErrorKind::kInvalidRequest, gone.error().kind);

  // A pipeline built under the old epoch is refused by the new daemon
  // (D-106), even where its generation happens to match: nothing applied.
  auto fresh = daemon().NewClient();
  auto now = fresh->Pipeline();
  ASSERT_TRUE(now) << now.error().message;
  const bess::sdk::PipelineVersion stale{now->version.generation, before->version.daemon_epoch};
  auto refused = fresh->ApplyPipeline(before->pipeline, stale);
  ASSERT_FALSE(refused);
  EXPECT_EQ(ErrorKind::kDaemonRestarted, refused.error().kind) << refused.error().message;
  auto after = fresh->Pipeline();
  ASSERT_TRUE(after) << after.error().message;
  EXPECT_EQ(now->version, after->version);
  EXPECT_EQ(0, after->pipeline.modules_size());
}

// A port's queues and steering in GetCapabilities. (Its version, RPCs,
// classes and resources are conformance's "discovery" case.)
TEST_F(ControlSdkTest, CapabilitiesNamePorts) {
  auto snap = client().Pipeline();
  ASSERT_TRUE(snap) << snap.error().message;
  auto arg = types_->Make("bess.pb.UnixSocketPortArg",
                          std::format(R"({{"path": "@bess-live-caps-{}"}})", getpid()));
  auto p = bess::sdk::PipelineBuilder(snap->pipeline).Port("usp", "UnixSocketPort", arg.get()).Build();
  ASSERT_TRUE(p) << p.error().message;
  auto applied = client().ApplyPipeline(*p);
  ASSERT_TRUE(applied) << applied.error().message;

  auto caps = client().Capabilities();
  ASSERT_TRUE(caps) << caps.error().message;
  EXPECT_EQ(client().DaemonEpoch(), caps->daemon_epoch());
  const v2::PortInfo *port = nullptr;
  for (const v2::PortInfo &info : caps->ports()) {
    if (info.name() == "usp") {
      port = &info;
    }
  }
  ASSERT_NE(nullptr, port);
  EXPECT_EQ("UnixSocketPort", port->driver());
  EXPECT_EQ(1u, port->rx_queues());
  EXPECT_TRUE(port->symmetric_rss()) << "one queue: trivially symmetric";
}

}  // namespace
