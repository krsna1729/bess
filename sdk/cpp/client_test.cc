// SPDX-License-Identifier: BSD-3-Clause

// The C++ SDK against a scripted daemon: the same cases as the Go SDK's
// client_test.go, so both SDKs keep one contract.

#include "bess/sdk/client.h"

#include <gtest/gtest.h>
#include <google/protobuf/util/message_differencer.h>

#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace bess::sdk {
namespace {

using std::chrono::milliseconds;

// Two control messages stand in for a resource's key and value types.
const std::string kKeyType(pb::GetTransactionRequest::descriptor()->full_name());
const std::string kValueType(
    pb::ListTransactionResourcesRequest::descriptor()->full_name());
constexpr char kResource[] = "m0/table";

constexpr auto kApplied = pb::TransactionRecord::OUTCOME_APPLIED;
constexpr auto kRejected = pb::TransactionRecord::OUTCOME_REJECTED;
constexpr auto kConflictOutcome = pb::TransactionRecord::OUTCOME_CONFLICT;
constexpr auto kBusyOutcome = pb::TransactionRecord::OUTCOME_BUSY;

// One scripted answer: a response (outcome / known, with an epoch) or a
// failed status with optional trailers.
struct Step {
  pb::TransactionRecord::Outcome outcome = pb::TransactionRecord::OUTCOME_UNSPECIFIED;
  bool known = false;
  uint64_t epoch = 0;
  grpc::Status status = grpc::Status::OK;
  Trailers trailers;
};

Step Timeout() { return {.status = grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "deadline")}; }
Step Unavailable() { return {.status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "down")}; }
Step Answer(pb::TransactionRecord::Outcome o, uint64_t epoch) { return {.outcome = o, .epoch = epoch}; }
Step NotKnown(uint64_t epoch) { return {.known = false, .epoch = epoch}; }
Step Known(pb::TransactionRecord::Outcome o, uint64_t epoch) {
  return {.outcome = o, .known = true, .epoch = epoch};
}
Step Failed(grpc::StatusCode code, pb::ErrorDetail detail) {
  return {.status = grpc::Status(code, "x"),
          .trailers = {{kErrorDetailKey, detail.SerializeAsString()}}};
}
Step Refusal(grpc::StatusCode code, pb::ErrorDetail::Code detail) {
  pb::ErrorDetail d;
  d.set_code(detail);
  d.set_message("refused");
  return Failed(code, d);
}

using StreamStep = std::variant<pb::Event, grpc::Status>;

class ScriptedStream final : public EventStream {
 public:
  explicit ScriptedStream(std::deque<StreamStep> steps) : steps_(std::move(steps)) {}
  bool Read(pb::Event *event) override {
    if (steps_.empty()) {
      status_ = grpc::Status(grpc::StatusCode::UNAVAILABLE, "end of script");
      return false;
    }
    StreamStep step = std::move(steps_.front());
    steps_.pop_front();
    if (auto *s = std::get_if<grpc::Status>(&step)) {
      status_ = *s;
      return false;
    }
    *event = std::get<pb::Event>(step);
    return true;
  }
  grpc::Status Finish() override { return status_; }
  void Cancel() override { status_ = grpc::Status(grpc::StatusCode::CANCELLED, "cancelled"); }

 private:
  std::deque<StreamStep> steps_;
  grpc::Status status_;
};

// A stream that delivers nothing until it is cancelled: an idle daemon.
class IdleStream final : public EventStream {
 public:
  bool Read(pb::Event *) override {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return cancelled_; });
    return false;
  }
  grpc::Status Finish() override { return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled"); }
  void Cancel() override {
    {
      std::lock_guard lock(mu_);
      cancelled_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool cancelled_ = false;
};

class Fake final : public Transport {
 public:
  uint64_t epoch = 0;
  std::deque<Step> applies, gets, pipelines;
  std::vector<pb::ApplyTransactionRequest> sent;
  std::vector<std::string> asked;
  std::vector<pb::ApplyPipelineRequest> applied;
  int listed = 0;
  bool reject_ops = false;
  bool old_daemon = false;  // GetCapabilities: UNIMPLEMENTED
  bool refuse = false;      // GetCapabilities: PERMISSION_DENIED
  std::deque<std::deque<StreamStep>> streams;
  bool idle = false;  // every stream is an IdleStream
  std::vector<std::pair<uint64_t, uint64_t>> watched;  // (from, epoch) per call

  uint64_t EpochOr1() const { return epoch == 0 ? 1 : epoch; }

  // What bessd answers a mutating request built under another epoch (D-106).
  static grpc::Status RefuseEpoch(uint64_t sent, uint64_t daemon, Trailers *t) {
    if (sent == 0) {
      pb::ErrorDetail d;
      d.set_code(pb::ErrorDetail::INVALID_ARGUMENT);
      d.set_field("daemon_epoch");
      t->emplace(kErrorDetailKey, d.SerializeAsString());
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "no epoch");
    }
    pb::ErrorDetail d;
    d.set_code(pb::ErrorDetail::EPOCH_MISMATCH);
    d.set_field("daemon_epoch");
    d.set_daemon_epoch(daemon);
    d.set_message("built under another epoch");
    t->emplace(kErrorDetailKey, d.SerializeAsString());
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, d.message());
  }

  grpc::Status ApplyTransaction(Deadline, const pb::ApplyTransactionRequest &in,
                                pb::ApplyTransactionResponse *out,
                                Trailers *t) override {
    sent.push_back(in);
    Step s = Pop(applies);
    if (!s.status.ok()) {
      *t = s.trailers;
      return s.status;
    }
    if (in.daemon_epoch() != s.epoch) {  // the step's epoch is the daemon's
      return RefuseEpoch(in.daemon_epoch(), s.epoch, t);
    }
    pb::TransactionRecord *rec = out->mutable_record();
    rec->set_request_id(in.request_id());
    rec->set_outcome(s.outcome);
    rec->set_generation(7);
    if (reject_ops) {
      auto *op = rec->add_ops();
      op->set_status(pb::TransactionOpResult::STATUS_FAILED);
      op->set_error("no such next hop");
    }
    out->set_daemon_epoch(s.epoch);
    return grpc::Status::OK;
  }

  grpc::Status GetTransaction(Deadline, const pb::GetTransactionRequest &in,
                              pb::GetTransactionResponse *out, Trailers *t) override {
    asked.push_back(in.request_id());
    Step s = Pop(gets);
    if (!s.status.ok()) {
      *t = s.trailers;
      return s.status;
    }
    out->set_known(s.known);
    out->set_daemon_epoch(s.epoch);
    out->mutable_record()->set_request_id(in.request_id());
    out->mutable_record()->set_outcome(s.outcome);
    out->mutable_record()->set_generation(7);
    return grpc::Status::OK;
  }

  grpc::Status ListTransactionResources(Deadline,
                                        const pb::ListTransactionResourcesRequest &,
                                        pb::ListTransactionResourcesResponse *out,
                                        Trailers *) override {
    listed++;
    out->set_daemon_epoch(EpochOr1());
    auto *r = out->add_resources();
    r->set_name(kResource);
    r->set_key_type(kKeyType);
    r->set_value_type(kValueType);
    return grpc::Status::OK;
  }

  grpc::Status GetCapabilities(Deadline, const pb::GetCapabilitiesRequest &,
                               pb::GetCapabilitiesResponse *out, Trailers *) override {
    if (old_daemon) {
      return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "unknown method GetCapabilities");
    }
    if (refuse) {
      return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "no");
    }
    out->add_rpcs("bess.pb.v2.Control/ApplyTransaction");
    out->set_daemon_epoch(EpochOr1());
    return grpc::Status::OK;
  }

  grpc::Status GetPipeline(Deadline, const pb::GetPipelineRequest &,
                           pb::GetPipelineResponse *out, Trailers *) override {
    out->mutable_pipeline();
    out->set_generation(3);
    out->set_daemon_epoch(EpochOr1());
    return grpc::Status::OK;
  }

  grpc::Status ApplyPipeline(Deadline, const pb::ApplyPipelineRequest &in,
                             pb::ApplyPipelineResponse *out, Trailers *t) override {
    applied.push_back(in);
    if (in.daemon_epoch() != EpochOr1()) {
      return RefuseEpoch(in.daemon_epoch(), EpochOr1(), t);
    }
    if (!pipelines.empty()) {
      Step s = Pop(pipelines);
      if (!s.status.ok()) {
        *t = s.trailers;
        return s.status;
      }
    }
    out->set_generation(4);
    out->set_daemon_epoch(EpochOr1());
    return grpc::Status::OK;
  }

  std::vector<pb::SetCaptureRequest> captures;
  grpc::Status SetCapture(Deadline, const pb::SetCaptureRequest &in, pb::SetCaptureResponse *out,
                          Trailers *t) override {
    captures.push_back(in);
    if (in.daemon_epoch() != EpochOr1()) {
      return RefuseEpoch(in.daemon_epoch(), EpochOr1(), t);
    }
    out->set_daemon_epoch(EpochOr1());
    return grpc::Status::OK;
  }

  std::unique_ptr<EventStream> WatchEvents(const pb::WatchEventsRequest &in,
                                           grpc::Status *status) override {
    watched.emplace_back(in.from_sequence(), in.daemon_epoch());
    if (idle) {
      *status = grpc::Status::OK;
      return std::make_unique<IdleStream>();
    }
    if (streams.empty()) {
      *status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "no more streams");
      return nullptr;
    }
    auto st = std::make_unique<ScriptedStream>(std::move(streams.front()));
    streams.pop_front();
    *status = grpc::Status::OK;
    return st;
  }

 private:
  static Step Pop(std::deque<Step> &q) {
    EXPECT_FALSE(q.empty()) << "script ran out";
    if (q.empty()) {
      return Unavailable();
    }
    Step s = std::move(q.front());
    q.pop_front();
    return s;
  }
};

struct Harness {
  std::shared_ptr<Fake> f;
  std::vector<std::chrono::nanoseconds> sleeps;
  std::unique_ptr<Client> c;

  explicit Harness(int attempts) : f(std::make_shared<Fake>()) {
    c = std::make_unique<Client>(
        f,
        RetryPolicy{.attempt_timeout = std::chrono::seconds(1),
                    .attempts = attempts,
                    .busy_backoff = milliseconds(10)},
        [this](std::chrono::nanoseconds d) { sleeps.push_back(d); });
  }

  Result<Applied> Commit(TransactionOptions options = {}) {
    auto r = c->Resource(kResource);
    EXPECT_TRUE(r.has_value());
    Transaction tx = c->NewTransaction(std::move(options));
    pb::GetTransactionRequest key;
    key.set_request_id("k");
    EXPECT_TRUE(tx.Upsert(*r, key, pb::ListTransactionResourcesRequest()));
    return tx.Commit();
  }
};

Result<void> Upsert(Transaction &tx, const Resource &r) {
  return tx.Upsert(r, pb::GetTransactionRequest(), pb::ListTransactionResourcesRequest());
}

template <typename T>
ErrorKind KindOf(const Result<T> &r) {
  EXPECT_FALSE(r.has_value());
  return r.has_value() ? ErrorKind::kCancelled : r.error().kind;
}

TEST(SdkClient, TypesAreCheckedBeforeAnythingIsSent) {
  Harness h(4);
  auto r = h.c->Resource(kResource);
  ASSERT_TRUE(r);
  Transaction tx = h.c->NewTransaction();
  EXPECT_EQ(KindOf(tx.Upsert(*r, pb::ListTransactionResourcesRequest(),
                             pb::ListTransactionResourcesRequest())),
            ErrorKind::kInvalidRequest);
  EXPECT_EQ(KindOf(tx.Upsert(*r, pb::GetTransactionRequest(), pb::GetTransactionRequest())),
            ErrorKind::kInvalidRequest);
  EXPECT_EQ(KindOf(h.c->Resource("missing/table")), ErrorKind::kInvalidRequest);
  EXPECT_TRUE(h.f->sent.empty());
}

TEST(SdkClient, Applied) {
  Harness h(4);
  h.f->applies = {Answer(kApplied, 1)};
  auto a = h.Commit();
  ASSERT_TRUE(a) << a.error().message;
  EXPECT_EQ(a->generation, 7u);
  EXPECT_FALSE(a->replayed);
  EXPECT_FALSE(a->request_id.empty());
  EXPECT_EQ(h.f->sent.size(), 1u);
}

TEST(SdkClient, ATimeoutIsResolvedByAskingNotByResending) {
  Harness h(4);
  h.f->applies = {Timeout()};
  h.f->gets = {Known(kApplied, 1)};
  auto a = h.Commit();
  ASSERT_TRUE(a) << a.error().message;
  EXPECT_TRUE(a->replayed);
  ASSERT_EQ(h.f->sent.size(), 1u);
  EXPECT_EQ(h.f->asked[0], h.f->sent[0].request_id());
}

TEST(SdkClient, AnUnseenRequestIsResentIdentically) {
  Harness h(4);
  h.f->applies = {Timeout(), Answer(kApplied, 1)};
  h.f->gets = {NotKnown(1)};
  ASSERT_TRUE(h.Commit());
  ASSERT_EQ(h.f->sent.size(), 2u);
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(h.f->sent[0], h.f->sent[1]));
}

TEST(SdkClient, AnUnansweredQuestionIsAskedAgainNotAnsweredByResending) {
  Harness h(4);
  h.f->applies = {Unavailable(), Answer(kApplied, 1)};
  h.f->gets = {Unavailable(), NotKnown(1)};
  ASSERT_TRUE(h.Commit());
  EXPECT_EQ(h.f->asked.size(), 2u);
  ASSERT_EQ(h.f->sent.size(), 2u);
  EXPECT_EQ(h.f->sent[0].request_id(), h.f->sent[1].request_id());
}

TEST(SdkClient, ARestartBehindAFailedQuestionIsNotAppliedBlind) {
  Harness h(4);
  h.f->applies = {Timeout()};
  h.f->gets = {Unavailable(), NotKnown(2)};
  EXPECT_EQ(KindOf(h.Commit()), ErrorKind::kDaemonRestarted);
  EXPECT_EQ(h.f->sent.size(), 1u) << "resent to the restarted daemon";
}

TEST(SdkClient, ARestartDuringRecoveryIsReportedNotGuessed) {
  Harness h(4);
  h.f->applies = {Timeout()};
  h.f->gets = {NotKnown(2)};
  EXPECT_EQ(KindOf(h.Commit()), ErrorKind::kDaemonRestarted);
}

TEST(SdkClient, TheEpochIsLearnedBeforeTheFirstSend) {
  Harness h(4);
  h.f->applies = {Answer(kApplied, 2)};
  Transaction tx = h.c->NewTransaction();
  // No discovery through this client.
  const Resource r{.name = kResource, .key_type = kKeyType, .value_type = kValueType,
                   .daemon_epoch = 1};
  ASSERT_TRUE(Upsert(tx, r));
  EXPECT_EQ(KindOf(tx.Commit()), ErrorKind::kDaemonRestarted);
  EXPECT_EQ(h.f->listed, 1) << "epoch not learned first";
}

TEST(SdkClient, AHandleFromBeforeARestartIsRefusedNotSent) {
  Harness h(4);
  h.f->epoch = 1;
  h.f->applies = {Answer(kApplied, 2)};
  auto old = h.c->Resource(kResource);
  ASSERT_TRUE(old);
  Transaction built = h.c->NewTransaction();
  ASSERT_TRUE(Upsert(built, *old));
  h.f->epoch = 2;
  ASSERT_TRUE(h.c->Resources(true));  // the client sees the restart
  Transaction other = h.c->NewTransaction();
  EXPECT_EQ(KindOf(Upsert(other, *old)), ErrorKind::kStaleResource);
  EXPECT_EQ(KindOf(built.Commit()), ErrorKind::kStaleResource);  // never sent
  EXPECT_TRUE(h.f->sent.empty());
  auto fresh = h.c->Resource(kResource);  // looked up again: usable
  ASSERT_TRUE(fresh);
  EXPECT_EQ(fresh->daemon_epoch, 2u);
  Transaction tx = h.c->NewTransaction();
  ASSERT_TRUE(Upsert(tx, *fresh));
  ASSERT_TRUE(tx.Commit());
  EXPECT_EQ(h.f->sent.size(), 1u);
}

TEST(SdkClient, ARestartSeenByACommitInvalidatesTheOldHandles) {
  Harness h(4);
  h.f->epoch = 1;
  h.f->applies = {Answer(kApplied, 2)};
  auto old = h.c->Resource(kResource);
  ASSERT_TRUE(old);
  Transaction tx = h.c->NewTransaction();
  ASSERT_TRUE(Upsert(tx, *old));
  EXPECT_EQ(KindOf(tx.Commit()), ErrorKind::kDaemonRestarted);
  Transaction next = h.c->NewTransaction();
  EXPECT_EQ(KindOf(Upsert(next, *old)), ErrorKind::kStaleResource);
}

TEST(SdkClient, OneTransactionHoldsHandlesOfOneEpoch) {
  Harness h(4);
  h.f->epoch = 1;
  auto old = h.c->Resource(kResource);
  ASSERT_TRUE(old);
  Transaction tx = h.c->NewTransaction();
  ASSERT_TRUE(Upsert(tx, *old));
  h.f->epoch = 2;
  auto all = h.c->Resources(true);  // the client sees the restart
  ASSERT_TRUE(all);
  const Resource now = all->at(kResource);  // valid now, but the transaction is not
  EXPECT_EQ(KindOf(Upsert(tx, now)), ErrorKind::kStaleResource);
  EXPECT_EQ(KindOf(tx.Erase(now, pb::GetTransactionRequest())), ErrorKind::kStaleResource);
}

TEST(SdkClient, InternalWithoutBessDetailIsNoAnswer) {
  {
    Harness h(4);
    h.f->applies = {{.status = grpc::Status(grpc::StatusCode::INTERNAL, "stream reset")}};
    h.f->gets = {Known(kApplied, 1)};
    auto a = h.Commit();
    ASSERT_TRUE(a) << a.error().message;
    EXPECT_TRUE(a->replayed);
  }
  Harness h(4);
  pb::ErrorDetail d;
  d.set_code(pb::ErrorDetail::INTERNAL);
  d.set_message("engine failure");
  h.f->applies = {Failed(grpc::StatusCode::INTERNAL, d)};
  EXPECT_EQ(KindOf(h.Commit()), ErrorKind::kInvalidRequest);
}

TEST(SdkClient, ServerErrorsCarryTheirDetail) {
  Harness h(4);
  pb::ErrorDetail d;
  d.set_code(pb::ErrorDetail::INVALID_ARGUMENT);
  d.set_message("bad key");
  d.set_field("key");
  h.f->applies = {Failed(grpc::StatusCode::INVALID_ARGUMENT, d)};
  auto a = h.Commit();
  ASSERT_EQ(KindOf(a), ErrorKind::kInvalidRequest);
  ASSERT_TRUE(a.error().detail);
  EXPECT_EQ(a.error().detail->field(), "key");
  EXPECT_EQ(a.error().message, "bad key");
}

TEST(SdkClient, BusyIsRetriedWithBackoff) {
  Harness h(4);
  h.f->applies = {Answer(kBusyOutcome, 1), Answer(kBusyOutcome, 1), Answer(kApplied, 1)};
  ASSERT_TRUE(h.Commit());
  ASSERT_EQ(h.sleeps.size(), 2u);
  EXPECT_EQ(h.sleeps[0], milliseconds(10));
  EXPECT_EQ(h.sleeps[1], milliseconds(20));
}

TEST(SdkClient, BusyThroughEveryAttempt) {
  Harness h(3);
  h.f->applies = {Answer(kBusyOutcome, 1), Answer(kBusyOutcome, 1), Answer(kBusyOutcome, 1)};
  EXPECT_EQ(KindOf(h.Commit()), ErrorKind::kBusy);
}

TEST(SdkClient, OutOfAttemptsAfterAnUnansweredSendIsUnknownNotBusy) {
  const struct {
    int attempts;
    std::deque<Step> applies, gets;
  } cases[] = {
      {2, {Timeout()}, {NotKnown(1)}},
      {3, {Timeout(), Answer(kBusyOutcome, 1)}, {NotKnown(1)}},
      {4, {Timeout(), Timeout()}, {NotKnown(1), NotKnown(1)}},
  };
  for (const auto &c : cases) {
    Harness h(c.attempts);
    h.f->applies = c.applies;
    h.f->gets = c.gets;
    EXPECT_EQ(KindOf(h.Commit()), ErrorKind::kTransport) << c.attempts;
  }
}

TEST(SdkClient, NoAnswerAtAllIsAnUnknownOutcome) {
  Harness h(2);
  h.f->applies = {Timeout()};
  h.f->gets = {Timeout()};
  auto a = h.Commit();
  ASSERT_EQ(KindOf(a), ErrorKind::kTransport);
  EXPECT_EQ(a.error().request_id, h.f->sent[0].request_id());
}

TEST(SdkClient, ARefusedQuestionAfterAnUnansweredSendIsUnknown) {
  Harness h(4);
  h.f->applies = {Timeout()};
  h.f->gets = {{.status = grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "proxy")}};
  auto a = h.Commit();
  ASSERT_EQ(KindOf(a), ErrorKind::kTransport);
  EXPECT_EQ(a.error().code, grpc::StatusCode::PERMISSION_DENIED);
}

TEST(SdkClient, ResourcesReturnsACopy) {
  Harness h(4);
  auto all = h.c->Resources();
  ASSERT_TRUE(all);
  all->erase(kResource);
  EXPECT_TRUE(h.c->Resource(kResource)) << "editing the returned map changed the cache";
  EXPECT_EQ(h.f->listed, 1);
}

TEST(SdkClient, ConflictAndRejection) {
  {
    Harness h(4);
    h.f->applies = {Answer(kConflictOutcome, 1)};
    auto a = h.Commit({.expected_generation = 3});
    ASSERT_EQ(KindOf(a), ErrorKind::kConflict);
    EXPECT_FALSE(a.error().after_unknown_attempt);
    EXPECT_EQ(h.f->sent[0].expected_generation(), 3u);
  }
  {
    Harness h(4);
    h.f->applies = {Timeout(), Answer(kConflictOutcome, 1)};
    h.f->gets = {NotKnown(1)};
    auto a = h.Commit({.expected_generation = 3});
    ASSERT_EQ(KindOf(a), ErrorKind::kConflict);
    EXPECT_TRUE(a.error().after_unknown_attempt)
        << "a conflict after an unanswered send must say it may have applied";
  }
  Harness h(4);
  h.f->applies = {Answer(kRejected, 1)};
  h.f->reject_ops = true;
  auto a = h.Commit();
  ASSERT_EQ(KindOf(a), ErrorKind::kRejected);
  ASSERT_EQ(a.error().failures.size(), 1u);
  EXPECT_EQ(a.error().failures[0].error, "no such next hop");
  EXPECT_EQ(h.f->sent.size(), 1u);
}

TEST(SdkClient, ACommittedTransactionIsNotSentAgain) {
  Harness h(4);
  h.f->applies = {Answer(kApplied, 1)};
  auto r = h.c->Resource(kResource);
  ASSERT_TRUE(r);
  Transaction tx = h.c->NewTransaction();
  ASSERT_TRUE(Upsert(tx, *r));
  auto a1 = tx.Commit();
  auto a2 = tx.Commit();
  ASSERT_TRUE(a1);
  ASSERT_TRUE(a2);
  EXPECT_EQ(a1->request_id, a2->request_id);
  EXPECT_EQ(h.f->sent.size(), 1u);
}

TEST(SdkClient, SupportsReadsTheDaemonsRpcsAndAnOldDaemonSupportsNothingNew) {
  Harness h(1);
  EXPECT_EQ(h.c->Supports("ApplyTransaction"), true);
  EXPECT_EQ(h.c->Supports("WatchEvents"), false);
  Harness old(1);
  old.f->old_daemon = true;
  EXPECT_EQ(old.c->Supports("ApplyTransaction"), false);
  Harness refused(1);
  refused.f->refuse = true;
  EXPECT_FALSE(refused.c->Supports("ApplyTransaction"))
      << "a refusal other than UNIMPLEMENTED was reported as unsupported";
}

pb::Event Ev(std::string type, uint64_t seq, uint64_t epoch) {
  pb::Event e;
  e.set_type(std::move(type));
  e.set_sequence(seq);
  e.set_daemon_epoch(epoch);
  return e;
}

TEST(SdkClient, WatchEventsResumesAfterTheLastEventAndReportsARestart) {
  const grpc::Status down(grpc::StatusCode::UNAVAILABLE, "down");
  pb::Event gap = Ev("bess.gap", 0, 1);
  gap.set_gap_from(7);
  gap.set_gap_to(9);
  Harness h(1);
  h.f->streams = {
      {Ev("bess.start", 5, 1), down},  // fails before its first event
      // Ends cleanly (a proxy closing a long stream): resumed like a failure.
      {Ev("bess.start", 5, 1), Ev("a", 5, 1), Ev("b", 6, 1), grpc::Status::OK},
      {Ev("bess.start", 7, 1), gap, Ev("c", 9, 1), Ev("bess.progress", 14, 1), down},
      {Ev("bess.restart", 0, 2)},
  };
  std::vector<std::string> seen;
  auto r = h.c->WatchEvents(5, 0, {}, [&](const pb::Event &e) {
    seen.push_back(e.type());
    return true;
  });
  ASSERT_EQ(KindOf(r), ErrorKind::kDaemonRestarted);
  EXPECT_EQ(r.error().to_epoch, 2u);
  EXPECT_EQ(seen, (std::vector<std::string>{"a", "b", "bess.gap", "c"}));
  // Resumed where it stopped, under the epoch seen.
  EXPECT_EQ(h.f->watched, (std::vector<std::pair<uint64_t, uint64_t>>{
                              {5, 0}, {5, 1}, {7, 1}, {15, 1}}));
}

TEST(SdkClient, WatchEventsEndsWhenTheCallerStops) {
  Harness h(1);
  h.f->streams = {{Ev("bess.start", 1, 1), Ev("a", 1, 1), Ev("b", 2, 1)}};
  int seen = 0;
  auto r = h.c->WatchEvents(0, 0, {}, [&](const pb::Event &) { return ++seen < 1; });
  EXPECT_TRUE(r);
  EXPECT_EQ(seen, 1);
  EXPECT_EQ(h.f->watched.size(), 1u);
  std::stop_source stop;
  stop.request_stop();
  EXPECT_TRUE(h.c->WatchEvents(0, 0, {}, [](const pb::Event &) { return true; },
                               stop.get_token()));
  EXPECT_EQ(h.f->watched.size(), 1u) << "a stopped watch opened a stream";
}

TEST(SdkClient, AStopEndsAWatchBlockedOnAnIdleStream) {
  Harness h(1);
  h.f->idle = true;
  std::stop_source stop;
  std::jthread stopper([&] {
    std::this_thread::sleep_for(milliseconds(50));
    stop.request_stop();
  });
  auto r = h.c->WatchEvents(0, 0, {}, [](const pb::Event &) { return true; },
                            stop.get_token());
  EXPECT_TRUE(r);
  EXPECT_EQ(h.f->watched.size(), 1u);
}

TEST(SdkClient, ThePipelineBuilderAssemblesAndEditsASnapshot) {
  pb::GetTransactionRequest arg;
  arg.set_request_id("x");
  pb::TaskPlacement on_worker;
  on_worker.set_module("a");
  on_worker.set_wid(0);
  pb::TaskPlacement of_b;
  of_b.set_module("b");
  of_b.set_parent("rl");
  auto p = PipelineBuilder()
               .Task(on_worker)
               .Task(of_b)
               .Worker(0, 2, "")
               .Module("a", "Source", nullptr)
               .Module("b", "Bypass", &arg)
               .Module("c", "Sink", nullptr)
               .Chain({"a", "b", "c"})
               .Connect("b", 1, "c", 0)
               .Build();
  ASSERT_TRUE(p);
  std::string got;
  for (const auto &c : p->connections()) {
    got += std::format("{}:{}->{} ", c.upstream(), c.ogate(), c.downstream());
  }
  EXPECT_EQ(got, "a:0->b b:0->c b:1->c ");
  EXPECT_TRUE(p->modules(1).arg().Is<pb::GetTransactionRequest>());
  EXPECT_FALSE(p->modules(0).has_arg());
  auto edited = PipelineBuilder(*p).Remove("b").Build();
  ASSERT_TRUE(edited);
  EXPECT_EQ(edited->modules_size(), 2);
  EXPECT_EQ(edited->connections_size(), 0);
  ASSERT_EQ(edited->tasks_size(), 1) << "the removed module's placement stayed";
  EXPECT_EQ(edited->tasks(0).module(), "a");
  EXPECT_EQ(p->modules_size(), 3);
}

// D-123: a worker given no core leaves it unset (bessd picks the CPU); an
// explicit core, 0 included, is sent.
TEST(SdkClient, ThePipelineBuilderLeavesAnAutomaticWorkersCoreUnset) {
  auto p = PipelineBuilder().Worker(0, std::nullopt, "").Worker(1, 0, "").Build();
  ASSERT_TRUE(p);
  ASSERT_EQ(p->workers_size(), 2);
  EXPECT_FALSE(p->workers(0).has_core());
  ASSERT_TRUE(p->workers(1).has_core());
  EXPECT_EQ(p->workers(1).core(), 0);
}

TEST(SdkClient, ApplyPipelineRetriesBusyAndNeverAConflict) {
  const Step busy = Refusal(grpc::StatusCode::FAILED_PRECONDITION, pb::ErrorDetail::RESOURCE_BUSY);
  {
    Harness h(4);
    h.f->pipelines = {busy, busy};
    auto snap = h.c->Pipeline();
    ASSERT_TRUE(snap);
    auto r = h.c->ApplyPipeline(snap->pipeline, snap->version);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->generation(), 4u);
    ASSERT_EQ(h.f->applied.size(), 3u);
    EXPECT_EQ(h.f->applied[0].expected_generation(), 3u);
    EXPECT_EQ(h.f->applied[0].daemon_epoch(), 1u) << "the snapshot's epoch";
    ASSERT_EQ(h.sleeps.size(), 2u);
    EXPECT_EQ(h.sleeps[1], milliseconds(20));
  }
  {
    Harness h(4);
    h.f->pipelines = {Refusal(grpc::StatusCode::ABORTED, pb::ErrorDetail::CONFLICT)};
    EXPECT_EQ(KindOf(h.c->ApplyPipeline(pb::Pipeline(), PipelineVersion{1, 1})),
              ErrorKind::kPipelineConflict);
    EXPECT_EQ(h.f->applied.size(), 1u);
  }
  Harness h(2);
  h.f->pipelines = {busy, busy};
  EXPECT_EQ(KindOf(h.c->ApplyPipeline(pb::Pipeline())), ErrorKind::kBusy);
}

// Every mutating request names the epoch it was built under (D-106): a
// transaction its resources', an apply its expected version's, else the one
// the client learned. A restarted daemon refuses them: kDaemonRestarted,
// nothing applied, not retried, and the client now knows the new epoch.
TEST(SdkClient, MutatingRequestsNameTheirEpochAndARestartRefusesThem) {
  {
    Harness h(4);
    EXPECT_TRUE(h.c->ApplyPipeline(pb::Pipeline()));
    EXPECT_EQ(h.f->listed, 1) << "the epoch was not learned before the apply";
    EXPECT_EQ(h.f->applied[0].daemon_epoch(), 1u);
    EXPECT_FALSE(h.f->applied[0].has_expected_generation());
  }
  {
    Harness h(4);
    auto snap = h.c->Pipeline();
    ASSERT_TRUE(snap);
    h.f->epoch = 2;  // restarted after the snapshot
    auto r = h.c->ApplyPipeline(snap->pipeline, snap->version);
    ASSERT_EQ(KindOf(r), ErrorKind::kDaemonRestarted);
    EXPECT_EQ(r.error().from_epoch, 1u);
    EXPECT_EQ(r.error().to_epoch, 2u);
    EXPECT_NE(r.error().message.find("nothing applied"), std::string::npos) << r.error().message;
    EXPECT_EQ(h.f->applied.size(), 1u) << "a refusal was retried";
    EXPECT_EQ(h.c->DaemonEpoch(), 2u);
    // An expected version is sent as given: one without its epoch is the
    // daemon's to refuse, never completed with the current epoch.
    EXPECT_EQ(KindOf(h.c->ApplyPipeline(snap->pipeline, PipelineVersion{3, 0})),
              ErrorKind::kInvalidRequest);
    EXPECT_EQ(h.f->applied.back().daemon_epoch(), 0u);
  }
  Harness h(4);
  h.f->epoch = 1;
  h.f->applies = {Answer(kApplied, 2)};  // the daemon restarted: epoch 2
  auto r = h.c->Resource(kResource);
  ASSERT_TRUE(r);
  Transaction tx = h.c->NewTransaction();
  ASSERT_TRUE(Upsert(tx, *r));
  auto a = tx.Commit();
  ASSERT_EQ(KindOf(a), ErrorKind::kDaemonRestarted);
  EXPECT_EQ(h.f->sent[0].daemon_epoch(), 1u) << "the resource's epoch";
  EXPECT_EQ(h.f->sent.size(), 1u);
  EXPECT_TRUE(h.f->asked.empty()) << "a refusal is an answer: nothing to ask";
  EXPECT_EQ(h.c->DaemonEpoch(), 2u);
}

// The Admin service's mutating calls (D-107) follow D-106: sent under the
// epoch the client knows (learned first), refused under another as a restart,
// and not retried.
TEST(SdkClient, AdminCallsNameTheKnownEpochAndARestartRefusesThem) {
  Harness h(4);
  pb::SetCaptureRequest req;
  req.set_module("m");
  ASSERT_TRUE(h.c->SetCapture(req));
  EXPECT_EQ(h.f->listed, 1) << "the epoch was not learned before the call";
  EXPECT_EQ(h.f->captures[0].daemon_epoch(), 1u);
  EXPECT_EQ(h.f->captures[0].module(), "m");
  h.f->epoch = 2;  // restarted
  auto r = h.c->SetCapture(req);
  ASSERT_EQ(KindOf(r), ErrorKind::kDaemonRestarted);
  EXPECT_EQ(r.error().from_epoch, 1u);
  EXPECT_EQ(r.error().to_epoch, 2u);
  EXPECT_EQ(h.f->captures.size(), 2u) << "a refusal was retried";
  EXPECT_EQ(h.c->DaemonEpoch(), 2u);
  ASSERT_TRUE(h.c->SetCapture(req));
  EXPECT_EQ(h.f->captures.back().daemon_epoch(), 2u) << "the next call names the new epoch";
}

TEST(SdkClient, ApplyPipelineWithoutAnAnswerIsUnknownAndNotResent) {
  {
    Harness h(4);
    h.f->pipelines = {Timeout()};
    EXPECT_EQ(KindOf(h.c->ApplyPipeline(pb::Pipeline())), ErrorKind::kTransport);
    EXPECT_EQ(h.f->applied.size(), 1u);
  }
  // UNAVAILABLE with BESS's detail is an answer (a resource failure).
  Harness h(4);
  h.f->pipelines = {Refusal(grpc::StatusCode::UNAVAILABLE, pb::ErrorDetail::RESOURCE_FAILURE)};
  auto r = h.c->ApplyPipeline(pb::Pipeline());
  ASSERT_EQ(KindOf(r), ErrorKind::kInvalidRequest);
  ASSERT_TRUE(r.error().detail);
  EXPECT_EQ(r.error().detail->code(), pb::ErrorDetail::RESOURCE_FAILURE);
}

}  // namespace
}  // namespace bess::sdk
