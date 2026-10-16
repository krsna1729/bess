// SPDX-License-Identifier: BSD-3-Clause

// The C++ SDK's commit protocol against every small-scope behaviour of the
// network and the daemon (roadmap Appendix L.1.2, M27; the search lived in
// pybess/test_sdk_model.py until D-140 removed the Python client, and the
// roadmap asks for each model in C++ against the code it guards).
//
// A simulated daemon keeps the truth: its epoch, and which request ids it
// applied in which epoch. For every RPC the environment chooses what happens
// -- answered, request lost, reply lost after the daemon acted, busy -- and
// whether the daemon restarts first (at most once). A depth-first search
// replays the production client (`sdk/cpp/src/client.cc`) against every
// sequence of choices and checks, per run:
//
//   * a request is applied at most once, over all epochs (no blind resend);
//   * Applied only if the daemon applied it, in the epoch that answered;
//   * Busy only if nothing was applied anywhere;
//   * TransportError only after an RPC that went unanswered, and never as a
//     failure (the outcome is unknown, D-101);
//   * DaemonRestarted only if the daemon restarted.
//
// The model holds only the semantic state (epoch, applied request ids); it is
// a test, not a proof: it excludes violations within the explored bound.

#include "bess/sdk/client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bess::sdk {
namespace {

// The client made more RPCs than the search bound (a bug: it must not).
struct Exhausted {
  std::string where;
};

// Two control messages stand in for a resource's key and value types.
const std::string kKeyType(pb::GetTransactionRequest::descriptor()->full_name());
const std::string kValueType(
    pb::ListTransactionResourcesRequest::descriptor()->full_name());
constexpr char kResource[] = "m0/table";

// What the environment can do to one RPC.
const std::vector<std::string> kApplyChoices = {"answer", "lose_request", "lose_reply",
                                                "busy"};
const std::vector<std::string> kGetChoices = {"answer", "lose_request"};
const std::vector<std::string> kRestartChoices = {"stay", "restart"};

// One run's choice state: the prefix the search fixed, the options taken so
// far (where to backtrack), and the bound the client must stay within.
class Run {
 public:
  Run(std::vector<size_t> prefix, size_t max_choices)
      : prefix_(std::move(prefix)), max_choices_(max_choices) {}

  std::string Choose(const std::vector<std::string> &options) {
    if (taken_.size() >= max_choices_) {
      throw Exhausted{"the client made more RPCs than its attempt budget allows"};
    }
    const size_t pick =
        taken_.size() < prefix_.size() ? prefix_[taken_.size()] : 0;
    taken_.emplace_back(pick, options.size());
    return options[pick];
  }

  const std::vector<std::pair<size_t, size_t>> &Taken() const { return taken_; }

 private:
  std::vector<size_t> prefix_;
  size_t max_choices_;
  std::vector<std::pair<size_t, size_t>> taken_;
};

// The truth, and a transport that answers from it under scripted choices.
class ModelDaemon final : public Transport {
 public:
  ModelDaemon(Run *run, int max_restarts = 1)
      : run_(run), restarts_left_(max_restarts) {}

  uint64_t epoch = 1;
  bool restarted = false;
  int applications = 0;  // every application, all epochs
  bool unanswered = false;  // some RPC went without an answer
  std::map<uint64_t, std::set<std::string>> applied;  // epoch -> request ids

  bool Applied(const std::string &request_id) const {
    for (const auto &[epoch, ids] : applied) {
      if (ids.count(request_id) != 0) {
        return true;
      }
    }
    return false;
  }

  grpc::Status ListTransactionResources(Deadline,
                                        const pb::ListTransactionResourcesRequest &,
                                        pb::ListTransactionResourcesResponse *out,
                                        Trailers *) override {
    pb::TransactionResource *r = out->add_resources();
    r->set_name(kResource);
    r->set_kind(pb::TransactionResource::KIND_MAP);
    r->set_key_type(kKeyType);
    r->set_value_type(kValueType);
    out->set_daemon_epoch(epoch);
    return grpc::Status::OK;
  }

  grpc::Status ApplyTransaction(Deadline, const pb::ApplyTransactionRequest &in,
                                pb::ApplyTransactionResponse *out,
                                Trailers *t) override {
    MaybeRestart();
    const std::string what = run_->Choose(kApplyChoices);
    if (what == "lose_request") {
      unanswered = true;
      return Timeout();
    }
    if (in.daemon_epoch() != epoch) {  // refused before any effect (D-106)
      if (what == "lose_reply") {
        unanswered = true;
        return Timeout();
      }
      pb::ErrorDetail d;
      d.set_code(pb::ErrorDetail::EPOCH_MISMATCH);
      d.set_daemon_epoch(epoch);
      d.set_message("built under another epoch");
      t->emplace(kErrorDetailKey, d.SerializeAsString());
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, d.message());
    }
    if (what == "busy") {
      out->set_daemon_epoch(epoch);
      pb::TransactionRecord *rec = out->mutable_record();
      rec->set_request_id(in.request_id());
      rec->set_outcome(pb::TransactionRecord::OUTCOME_BUSY);
      return grpc::Status::OK;
    }
    std::set<std::string> &seen = applied[epoch];
    const bool replayed = !seen.insert(in.request_id()).second;
    if (!replayed) {
      applications++;
    }
    if (what == "lose_reply") {
      unanswered = true;
      return Timeout();
    }
    out->set_daemon_epoch(epoch);
    out->set_replayed(replayed);
    pb::TransactionRecord *rec = out->mutable_record();
    rec->set_request_id(in.request_id());
    rec->set_outcome(pb::TransactionRecord::OUTCOME_APPLIED);
    rec->set_generation(1);
    return grpc::Status::OK;
  }

  grpc::Status GetTransaction(Deadline, const pb::GetTransactionRequest &in,
                              pb::GetTransactionResponse *out,
                              Trailers *) override {
    MaybeRestart();
    if (run_->Choose(kGetChoices) == "lose_request") {
      unanswered = true;
      return Timeout();
    }
    const bool known = applied[epoch].count(in.request_id()) != 0;
    out->set_known(known);
    out->set_daemon_epoch(epoch);
    pb::TransactionRecord *rec = out->mutable_record();
    rec->set_request_id(in.request_id());
    rec->set_outcome(known ? pb::TransactionRecord::OUTCOME_APPLIED
                           : pb::TransactionRecord::OUTCOME_UNSPECIFIED);
    return grpc::Status::OK;
  }

 private:
  static grpc::Status Timeout() {
    return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "deadline");
  }

  void MaybeRestart() {
    if (restarts_left_ > 0 && run_->Choose(kRestartChoices) == "restart") {
      restarts_left_--;
      restarted = true;
      epoch++;  // the new daemon remembers nothing
    }
  }

  Run *run_;
  int restarts_left_;
};

// One run: what the client reported, and the daemon's truth beside it.
struct RunResult {
  std::string kind;  // Applied, Busy, TransportError, DaemonRestarted, ...
  std::string summary;
  std::string request_id;
  uint64_t applied_epoch = 0;  // Applied: the epoch that answered
  std::shared_ptr<ModelDaemon> daemon;
};

RunResult Drive(Run *run, int attempts) {
  auto daemon = std::make_shared<ModelDaemon>(run);
  Client client(daemon,
                RetryPolicy{.attempts = attempts,
                            .busy_backoff = std::chrono::nanoseconds(0)},
                [](std::chrono::nanoseconds) {});
  RunResult out{.daemon = daemon};

  auto resource = client.Resource(kResource);
  if (!resource) {
    out.kind = "Refused";
    out.summary = "Resource: " + resource.error().message;
    return out;
  }
  Transaction tx = client.NewTransaction();
  if (auto put = tx.Upsert(*resource, pb::GetTransactionRequest(),
                           pb::ListTransactionResourcesRequest());
      !put) {
    out.kind = "Refused";
    out.summary = "Upsert: " + put.error().message;
    return out;
  }
  out.request_id = tx.request_id();
  auto committed = tx.Commit();
  if (committed) {
    out.kind = "Applied";
    out.applied_epoch = committed->daemon_epoch;
    out.summary = "applied in epoch " + std::to_string(committed->daemon_epoch) +
                  (committed->replayed ? " (replayed)" : "");
    return out;
  }
  switch (committed.error().kind) {
    case ErrorKind::kBusy:
      out.kind = "Busy";
      break;
    case ErrorKind::kTransport:
      out.kind = "TransportError";
      break;
    case ErrorKind::kDaemonRestarted:
      out.kind = "DaemonRestarted";
      break;
    default:
      out.kind = "OtherError";
      break;
  }
  out.summary = committed.error().message;
  return out;
}

// Runs every path of the search for `attempts`, checking each one's
// invariants; returns how many paths ran and how often each outcome appeared.
std::pair<int, std::map<std::string, int>> CheckAll(int attempts) {
  const size_t max_choices = static_cast<size_t>(4 * attempts + 4);
  int paths = 0;
  std::map<std::string, int> outcomes;
  std::vector<size_t> prefix;
  bool stop = false;
  while (!stop) {
    Run run(prefix, max_choices);
    RunResult result;
    try {
      result = Drive(&run, attempts);
    } catch (const Exhausted &e) {
      ADD_FAILURE() << e.where << " (attempts " << attempts << ")";
      break;
    }
    paths++;
    outcomes[result.kind]++;
    const std::string where = "attempts " + std::to_string(attempts) + ", path " +
                              std::to_string(paths) + ": " + result.kind + " (" +
                              result.summary + "), daemon applied " +
                              std::to_string(result.daemon->applications) +
                              " time(s), restarted=" +
                              (result.daemon->restarted ? "yes" : "no") +
                              ", unanswered=" +
                              (result.daemon->unanswered ? "yes" : "no");
    SCOPED_TRACE(where);

    // The invariants, checked against the daemon's truth.
    EXPECT_LE(result.daemon->applications, 1) << "applied more than once";
    if (result.kind == "Applied") {
      EXPECT_EQ(1, result.daemon->applications);
      EXPECT_TRUE(result.daemon->applied[result.applied_epoch].count(
          result.request_id))
          << "Applied in an epoch that did not apply it";
    } else if (result.kind == "Busy") {
      EXPECT_EQ(0, result.daemon->applications) << "Busy after an application";
    } else if (result.kind == "TransportError") {
      EXPECT_TRUE(result.daemon->unanswered) << "no RPC went unanswered";
    } else if (result.kind == "DaemonRestarted") {
      EXPECT_TRUE(result.daemon->restarted) << "no restart";
    } else {
      ADD_FAILURE() << "unexpected outcome " << result.kind;
    }

    // Backtrack: advance the deepest choice point that still has options.
    std::vector<std::pair<size_t, size_t>> steps = run.Taken();
    while (!steps.empty() && steps.back().first + 1 >= steps.back().second) {
      steps.pop_back();
    }
    if (steps.empty()) {
      stop = true;
      continue;
    }
    prefix.clear();
    for (size_t i = 0; i + 1 < steps.size(); i++) {
      prefix.push_back(steps[i].first);
    }
    prefix.push_back(steps.back().first + 1);
  }
  return {paths, outcomes};
}

}  // namespace

// The search, and with it the invariants: one run per sequence of choices
// within the attempt budget.
TEST(SdkCommitModel, EveryBehaviourWithinTheAttemptBudget) {
  int previous = 0;
  for (int attempts = 1; attempts <= 4; attempts++) {
    const auto [paths, outcomes] = CheckAll(attempts);
    SCOPED_TRACE("attempts " + std::to_string(attempts));
    // One attempt is exactly restart-or-not times the four Apply behaviours:
    // it sends once and never asks, so nothing else can be chosen.
    if (attempts == 1) {
      EXPECT_EQ(paths, 2 * static_cast<int>(kApplyChoices.size()));
    }
    EXPECT_GT(paths, previous) << "the search did not grow";
    previous = paths;
    std::cout << "attempts " << attempts << ": " << paths << " paths";
    for (const auto &[kind, count] : outcomes) {
      std::cout << ", " << kind << " " << count;
    }
    std::cout << std::endl;
    if (attempts >= 2) {
      for (const char *kind : {"Applied", "Busy", "TransportError", "DaemonRestarted"}) {
        EXPECT_NE(outcomes.find(kind), outcomes.end()) << "no " << kind << " path";
      }
    }
  }
}

}  // namespace bess::sdk
