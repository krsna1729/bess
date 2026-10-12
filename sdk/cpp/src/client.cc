// SPDX-License-Identifier: BSD-3-Clause

#include "bess/sdk/client.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <random>
#include <stop_token>
#include <thread>
#include <utility>

namespace bess::sdk {
namespace {

Error MakeError(ErrorKind kind, std::string message) {
  Error e;
  e.kind = kind;
  e.message = std::move(message);
  return e;
}

// A failed call that says nothing about the outcome: timeouts, losses,
// cancellation, and INTERNAL/UNKNOWN without BESS's error detail (a reset
// stream, a handler that died after recording).
bool NoAnswer(const grpc::Status &status, const Trailers &trailers) {
  switch (status.error_code()) {
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::CANCELLED:
      return true;
    case grpc::StatusCode::INTERNAL:
    case grpc::StatusCode::UNKNOWN:
      return !trailers.contains(kErrorDetailKey);
    default:
      return false;
  }
}

// The server's ErrorDetail from a failed call's trailers.
std::optional<pb::ErrorDetail> DecodeDetail(const Trailers &trailers) {
  const auto it = trailers.find(kErrorDetailKey);
  if (it == trailers.end()) {
    return std::nullopt;
  }
  pb::ErrorDetail d;
  if (!d.ParseFromString(it->second)) {
    return std::nullopt;
  }
  return d;
}

Error TransportFailure(grpc::StatusCode code, std::string request_id = {}) {
  Error e = MakeError(ErrorKind::kTransport,
                      std::format("no answer for \"{}\": status {}", request_id,
                                  static_cast<int>(code)));
  e.code = code;
  e.request_id = std::move(request_id);
  return e;
}

// A refused call, with the server's ErrorDetail decoded.
Error Translate(const grpc::Status &status, const Trailers &trailers) {
  Error e = MakeError(ErrorKind::kInvalidRequest, status.error_message());
  e.code = status.error_code();
  e.detail = DecodeDetail(trailers);
  if (e.detail && !e.detail->message().empty()) {
    e.message = e.detail->message();
  }
  if (e.detail && e.detail->code() == pb::ErrorDetail::CONFLICT) {
    e.message = "request id reused with different contents: " + e.message;
  }
  return e;
}

Error DaemonRestarted(uint64_t from, uint64_t to) {
  Error e = MakeError(ErrorKind::kDaemonRestarted,
                      std::format("the daemon restarted (epoch {} -> {})", from, to));
  e.from_epoch = from;
  e.to_epoch = to;
  return e;
}

Error StaleResource(std::string resource, uint64_t bound, uint64_t daemon) {
  const std::string what = resource.empty()
                               ? std::string("the transaction")
                               : std::format("resource \"{}\"", resource);
  Error e = MakeError(
      ErrorKind::kStaleResource,
      std::format("{} belongs to daemon epoch {}, not {}: look the resource up again",
                  what, bound, daemon));
  e.resource = std::move(resource);
  e.from_epoch = bound;
  e.to_epoch = daemon;
  return e;
}

Error Busy(int attempts) {
  Error e = MakeError(ErrorKind::kBusy,
                      std::format("still busy after {} attempts", attempts));
  e.attempts = attempts;
  return e;
}

Result<Applied> Finish(const pb::TransactionRecord &rec, bool replayed,
                       uint64_t epoch, bool unknown_attempt) {
  switch (rec.outcome()) {
    case pb::TransactionRecord::OUTCOME_APPLIED:
      return Applied{.request_id = rec.request_id(),
                     .generation = rec.generation(),
                     .visibility = rec.visibility(),
                     .replayed = replayed,
                     .daemon_epoch = epoch,
                     .record = rec};
    case pb::TransactionRecord::OUTCOME_CONFLICT: {
      std::string message =
          std::format("generation moved on (now {})", rec.generation());
      if (unknown_attempt) {
        message += "; an earlier unanswered attempt may have applied";
      }
      Error e = MakeError(ErrorKind::kConflict, std::move(message));
      e.record = rec;
      e.after_unknown_attempt = unknown_attempt;
      return std::unexpected(std::move(e));
    }
    case pb::TransactionRecord::OUTCOME_REJECTED: {
      Error e = MakeError(ErrorKind::kRejected, "transaction rejected:");
      e.record = rec;
      for (int i = 0; i < rec.ops_size(); i++) {
        if (rec.ops(i).status() == pb::TransactionOpResult::STATUS_FAILED) {
          e.failures.push_back({i, rec.ops(i).error()});
          e.message += std::format(" #{} {};", i, rec.ops(i).error());
        }
      }
      return std::unexpected(std::move(e));
    }
    case pb::TransactionRecord::OUTCOME_BUSY:
      return std::unexpected(Busy(1));
    default:
      return std::unexpected(MakeError(
          ErrorKind::kInvalidRequest,
          std::format("unexpected outcome {}", static_cast<int>(rec.outcome()))));
  }
}

std::string NewRequestId() {
  std::random_device rd;
  std::string id;
  for (int i = 0; i < 4; i++) {
    id += std::format("{:08x}", rd());
  }
  return id;
}

Result<void> CheckType(const Resource &r, std::string_view what,
                       const google::protobuf::Message &m, const std::string &want) {
  const std::string got(m.GetDescriptor()->full_name());
  if (got != want) {
    return std::unexpected(MakeError(
        ErrorKind::kInvalidRequest,
        std::format("{} of {} must be {}, not {}", what, r.name, want, got)));
  }
  return {};
}

}  // namespace

// -- the client ---------------------------------------------------------------

Client::Client(std::shared_ptr<Transport> transport, RetryPolicy retry,
               SleepFn sleep)
    : transport_(std::move(transport)), retry_(retry), sleep_(std::move(sleep)) {
  if (retry_.attempts < 1) {
    retry_.attempts = 1;
  }
  if (!sleep_) {
    sleep_ = [](std::chrono::nanoseconds d) { std::this_thread::sleep_for(d); };
  }
}

std::optional<uint64_t> Client::DaemonEpoch() const {
  std::lock_guard lock(mu_);
  return epoch_;
}

Deadline Client::AttemptDeadline() const {
  return std::chrono::system_clock::now() +
         std::chrono::duration_cast<std::chrono::system_clock::duration>(
             retry_.attempt_timeout);
}

// One unary call with no retry: no answer is kTransport, a refusal is
// kInvalidRequest. `call(deadline, &resp, &trailers)` makes the RPC.
template <typename Resp, typename Call>
Result<Resp> Client::Unary(Call &&call) {
  Resp resp;
  Trailers trailers;
  const grpc::Status status = call(AttemptDeadline(), &resp, &trailers);
  if (!status.ok()) {
    if (NoAnswer(status, trailers)) {
      return std::unexpected(TransportFailure(status.error_code()));
    }
    return std::unexpected(Translate(status, trailers));
  }
  return resp;
}

bool Client::ObserveEpoch(uint64_t epoch) {
  std::lock_guard lock(mu_);
  const bool changed = epoch_.has_value() && *epoch_ != epoch;
  epoch_ = epoch;
  if (changed) {
    resources_.reset();  // the restarted daemon's modules may differ
  }
  return changed;
}

Result<void> Client::CheckEpoch(uint64_t answered, uint64_t expected) {
  ObserveEpoch(answered);
  if (answered != expected) {
    return std::unexpected(DaemonRestarted(expected, answered));
  }
  return {};
}

Result<std::map<std::string, sdk::Resource>> Client::Resources(bool refresh) {
  {
    std::lock_guard lock(mu_);
    if (resources_ && !refresh) {
      return *resources_;  // a copy: the caller may edit it
    }
  }
  auto r = Unary<pb::ListTransactionResourcesResponse>(
      [&](Deadline d, auto *resp, Trailers *t) {
        return transport_->ListTransactionResources(
            d, pb::ListTransactionResourcesRequest(), resp, t);
      });
  if (!r) {
    return std::unexpected(std::move(r.error()));
  }
  std::map<std::string, sdk::Resource> found;
  for (const auto &res : r->resources()) {
    found[res.name()] = sdk::Resource{.name = res.name(),
                                      .key_type = res.key_type(),
                                      .value_type = res.value_type(),
                                      .daemon_epoch = r->daemon_epoch()};
  }
  ObserveEpoch(r->daemon_epoch());
  std::lock_guard lock(mu_);
  resources_ = found;
  return found;
}

Result<sdk::Resource> Client::Resource(std::string_view name) {
  for (const bool refresh : {false, true}) {
    auto all = Resources(refresh);
    if (!all) {
      return std::unexpected(std::move(all.error()));
    }
    if (auto it = all->find(std::string(name)); it != all->end()) {
      return it->second;
    }
  }
  return std::unexpected(MakeError(
      ErrorKind::kInvalidRequest,
      std::format("no transactional resource \"{}\"", name)));
}

Result<pb::GetCapabilitiesResponse> Client::Capabilities() {
  auto r = Unary<pb::GetCapabilitiesResponse>(
      [&](Deadline d, auto *resp, Trailers *t) {
        return transport_->GetCapabilities(d, pb::GetCapabilitiesRequest(), resp, t);
      });
  if (r) {
    ObserveEpoch(r->daemon_epoch());
  }
  return r;
}

Result<bool> Client::Supports(std::string_view rpc) {
  auto r = Capabilities();
  if (!r) {
    if (r.error().kind == ErrorKind::kInvalidRequest &&
        r.error().code == grpc::StatusCode::UNIMPLEMENTED) {
      return false;  // too old to say: none of the newer RPCs
    }
    return std::unexpected(std::move(r.error()));
  }
  const std::string suffix = "/" + std::string(rpc);
  for (const std::string &name : r->rpcs()) {
    if (name == rpc || name.ends_with(suffix)) {
      return true;
    }
  }
  return false;
}

Result<std::vector<pb::MetricSample>> Client::Metrics() {
  auto r = Unary<pb::ListMetricsResponse>([&](Deadline d, auto *resp, Trailers *t) {
    return transport_->ListMetrics(d, pb::ListMetricsRequest(), resp, t);
  });
  if (!r) {
    return std::unexpected(std::move(r.error()));
  }
  ObserveEpoch(r->daemon_epoch());
  return std::vector<pb::MetricSample>(r->samples().begin(), r->samples().end());
}

Result<void> Client::WatchEvents(
    uint64_t from, uint64_t epoch, const std::vector<std::string> &types,
    const std::function<bool(const pb::Event &)> &on_event,
    std::stop_token stop) {
  uint64_t next = from;
  auto backoff = retry_.busy_backoff;
  while (!stop.stop_requested()) {
    pb::WatchEventsRequest req;
    req.set_from_sequence(next);
    req.set_daemon_epoch(epoch);
    for (const auto &type : types) {
      req.add_types(type);
    }
    grpc::Status status;
    std::unique_ptr<EventStream> stream = transport_->WatchEvents(req, &status);
    if (stream) {
      std::stop_callback cancel(stop, [&] { stream->Cancel(); });
      pb::Event e;
      while (stream->Read(&e)) {
        if (e.type() == "bess.restart" ||
            (epoch != 0 && e.daemon_epoch() != epoch)) {
          ObserveEpoch(e.daemon_epoch());
          stream->Cancel();
          (void)stream->Finish();
          return std::unexpected(DaemonRestarted(epoch, e.daemon_epoch()));
        }
        epoch = e.daemon_epoch();
        if (e.type() == "bess.start") {
          next = e.sequence();
          continue;
        }
        if (e.type() == "bess.progress") {
          next = e.sequence() + 1;
          continue;
        }
        next = e.type() == "bess.gap" ? e.gap_to() : e.sequence() + 1;
        if (!on_event(e)) {
          stream->Cancel();
          (void)stream->Finish();
          return {};
        }
      }
      status = stream->Finish();
    }
    if (stop.stop_requested()) {
      return {};
    }
    if (!NoAnswer(status, Trailers())) {
      return std::unexpected(Translate(status, Trailers()));
    }
    sleep_(backoff);
    backoff = std::min<std::chrono::nanoseconds>(backoff * 2, std::chrono::seconds(1));
  }
  return {};
}

// -- desired state --------------------------------------------------------------

// A desired-state RPC. An answered refusal carries an ErrorDetail: CONFLICT
// is kPipelineConflict, RESOURCE_BUSY is retried (retry_busy) within the
// policy; no answer is kTransport.
template <typename Resp, typename Call>
Result<Resp> Client::PipelineCall(Call &&call, bool retry_busy,
                                  std::stop_token stop) {
  auto backoff = retry_.busy_backoff;
  for (int attempt = 1;; attempt++) {
    Resp resp;
    Trailers trailers;
    const grpc::Status status = call(AttemptDeadline(), &resp, &trailers);
    if (status.ok()) {
      return resp;
    }
    const std::optional<pb::ErrorDetail> detail = DecodeDetail(trailers);
    if (!detail && NoAnswer(status, trailers)) {
      return std::unexpected(TransportFailure(status.error_code()));
    }
    if (detail && detail->code() == pb::ErrorDetail::CONFLICT) {
      Error e = MakeError(ErrorKind::kPipelineConflict,
                          "pipeline conflict: " + detail->message());
      e.detail = detail;
      return std::unexpected(std::move(e));
    }
    if (detail && detail->code() == pb::ErrorDetail::RESOURCE_BUSY) {
      if (retry_busy && attempt < retry_.attempts && !stop.stop_requested()) {
        sleep_(backoff);
        backoff *= 2;
        continue;
      }
      return std::unexpected(Busy(attempt));
    }
    return std::unexpected(Translate(status, trailers));
  }
}

Result<PipelineSnapshot> Client::Pipeline() {
  auto r = PipelineCall<pb::GetPipelineResponse>(
      [&](Deadline d, auto *resp, Trailers *t) {
        return transport_->GetPipeline(d, pb::GetPipelineRequest(), resp, t);
      },
      false, {});
  if (!r) {
    return std::unexpected(std::move(r.error()));
  }
  return PipelineSnapshot{.pipeline = r->pipeline(), .generation = r->generation()};
}

Result<pb::Pipeline> Client::ValidatePipeline(const pb::Pipeline &p) {
  pb::ValidatePipelineRequest req;
  *req.mutable_pipeline() = p;
  auto r = PipelineCall<pb::ValidatePipelineResponse>(
      [&](Deadline d, auto *resp, Trailers *t) {
        return transport_->ValidatePipeline(d, req, resp, t);
      },
      false, {});
  if (!r) {
    return std::unexpected(std::move(r.error()));
  }
  return r->normalized();
}

Result<PipelineDiffResult> Client::DiffPipeline(const pb::Pipeline &p) {
  pb::DiffPipelineRequest req;
  *req.mutable_pipeline() = p;
  auto r = PipelineCall<pb::DiffPipelineResponse>(
      [&](Deadline d, auto *resp, Trailers *t) {
        return transport_->DiffPipeline(d, req, resp, t);
      },
      false, {});
  if (!r) {
    return std::unexpected(std::move(r.error()));
  }
  return PipelineDiffResult{.diff = r->diff(), .generation = r->generation()};
}

Result<PipelinePlan> Client::PlanPipeline(const pb::Pipeline &p) {
  pb::PlanPipelineRequest req;
  *req.mutable_pipeline() = p;
  auto r = PipelineCall<pb::PlanPipelineResponse>(
      [&](Deadline d, auto *resp, Trailers *t) {
        return transport_->PlanPipeline(d, req, resp, t);
      },
      false, {});
  if (!r) {
    return std::unexpected(std::move(r.error()));
  }
  return PipelinePlan{
      .steps = std::vector<pb::PlanStep>(r->steps().begin(), r->steps().end()),
      .generation = r->generation()};
}

Result<pb::ApplyPipelineResponse> Client::ApplyPipeline(
    const pb::Pipeline &p, std::optional<uint64_t> expected_generation,
    std::stop_token stop) {
  pb::ApplyPipelineRequest req;
  *req.mutable_pipeline() = p;
  if (expected_generation) {
    req.set_expected_generation(*expected_generation);
  }
  return PipelineCall<pb::ApplyPipelineResponse>(
      [&](Deadline d, auto *resp, Trailers *t) {
        return transport_->ApplyPipeline(d, req, resp, t);
      },
      true, stop);
}

// -- transactions ---------------------------------------------------------------

Transaction Client::NewTransaction(TransactionOptions options) {
  return Transaction(this, std::move(options));
}

Result<TransactionStatus> Client::GetTransaction(const std::string &request_id) {
  pb::GetTransactionRequest req;
  req.set_request_id(request_id);
  Trailers trailers;
  pb::GetTransactionResponse resp;
  const grpc::Status status =
      transport_->GetTransaction(AttemptDeadline(), req, &resp, &trailers);
  if (!status.ok()) {
    if (NoAnswer(status, trailers)) {
      return std::unexpected(TransportFailure(status.error_code(), request_id));
    }
    return std::unexpected(Translate(status, trailers));
  }
  return TransactionStatus{.known = resp.known(),
                           .record = resp.record(),
                           .daemon_epoch = resp.daemon_epoch()};
}

// Applies `req` exactly once, whatever the transport does. Every RPC counts
// against the attempt budget. After a send without an answer the outcome is
// asked for; the request is sent again only once GetTransaction answered
// "not known" under the epoch the commit started with.
Result<Applied> Client::Commit(const pb::ApplyTransactionRequest &req,
                               uint64_t epoch, std::stop_token stop) {
  if (!DaemonEpoch()) {
    if (auto r = Resources(true); !r) {  // learn the epoch before sending
      return std::unexpected(std::move(r.error()));
    }
  }
  if (const uint64_t current = DaemonEpoch().value_or(0); current != epoch) {
    return std::unexpected(StaleResource({}, epoch, current));
  }
  auto backoff = retry_.busy_backoff;
  grpc::StatusCode transport_failure = grpc::StatusCode::OK;
  grpc::StatusCode last_unanswered = grpc::StatusCode::OK;
  bool unknown_attempt = false;  // a send of this commit went unanswered
  bool send = true;
  for (int i = 0; i < retry_.attempts; i++) {
    if (stop.stop_requested()) {
      break;
    }
    if (send) {
      pb::ApplyTransactionResponse resp;
      Trailers trailers;
      const grpc::Status status =
          transport_->ApplyTransaction(AttemptDeadline(), req, &resp, &trailers);
      if (!status.ok()) {
        if (!NoAnswer(status, trailers)) {
          return std::unexpected(Translate(status, trailers));
        }
        transport_failure = last_unanswered = status.error_code();
        unknown_attempt = true;
        send = false;  // ask before sending again
        continue;
      }
      if (auto ok = CheckEpoch(resp.daemon_epoch(), epoch); !ok) {
        return std::unexpected(std::move(ok.error()));
      }
      if (resp.record().outcome() == pb::TransactionRecord::OUTCOME_BUSY) {
        transport_failure = grpc::StatusCode::OK;
        sleep_(backoff);
        backoff *= 2;
        continue;
      }
      return Finish(resp.record(), resp.replayed(), resp.daemon_epoch(),
                    unknown_attempt);
    }
    auto asked = GetTransaction(req.request_id());
    if (!asked) {
      if (asked.error().kind == ErrorKind::kTransport) {
        transport_failure = last_unanswered = asked.error().code;
        continue;  // still no answer: ask again, never resend blind
      }
      // The question was refused, but the unanswered send may have applied:
      // the outcome is unknown, not a refusal.
      return std::unexpected(TransportFailure(asked.error().code, req.request_id()));
    }
    if (auto ok = CheckEpoch(asked->daemon_epoch, epoch); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    if (asked->known) {
      return Finish(asked->record, true, asked->daemon_epoch, unknown_attempt);
    }
    transport_failure = grpc::StatusCode::OK;
    send = true;  // confirmed unseen under this epoch: sending again is safe
  }
  // Out of attempts (or stopped). After any unanswered send the outcome is
  // unknown: "not known" means "not applied yet", and an unanswered request
  // can still apply later.
  if (transport_failure != grpc::StatusCode::OK || unknown_attempt) {
    const grpc::StatusCode code = transport_failure != grpc::StatusCode::OK
                                      ? transport_failure
                                      : last_unanswered;
    return std::unexpected(TransportFailure(code, req.request_id()));
  }
  if (stop.stop_requested()) {
    return std::unexpected(MakeError(ErrorKind::kCancelled, "stopped"));
  }
  return std::unexpected(Busy(retry_.attempts));
}

Transaction::Transaction(Client *client, TransactionOptions options)
    : client_(client),
      request_id_(options.request_id.empty() ? NewRequestId()
                                             : std::move(options.request_id)),
      expected_generation_(options.expected_generation),
      snapshot_(options.snapshot) {}

// `r` must belong to the epoch the client has seen (when it has seen one;
// Commit checks again) and to this transaction's epoch.
Result<void> Transaction::Bind(const Resource &r) {
  if (const auto epoch = client_->DaemonEpoch(); epoch && r.daemon_epoch != *epoch) {
    return std::unexpected(StaleResource(r.name, r.daemon_epoch, *epoch));
  }
  if (epoch_ && r.daemon_epoch != *epoch_) {
    return std::unexpected(StaleResource(r.name, r.daemon_epoch, *epoch_));
  }
  epoch_ = r.daemon_epoch;
  return {};
}

Result<void> Transaction::Upsert(const Resource &r,
                                 const google::protobuf::Message &key,
                                 const google::protobuf::Message &value) {
  if (auto ok = Bind(r); !ok) {
    return ok;
  }
  if (auto ok = CheckType(r, "key", key, r.key_type); !ok) {
    return ok;
  }
  if (auto ok = CheckType(r, "value", value, r.value_type); !ok) {
    return ok;
  }
  pb::TransactionOp op;
  op.set_resource(r.name);
  if (!op.mutable_key()->PackFrom(key) || !op.mutable_value()->PackFrom(value)) {
    return std::unexpected(MakeError(ErrorKind::kInvalidRequest, "cannot pack the key or value"));
  }
  ops_.push_back(std::move(op));
  return {};
}

Result<void> Transaction::Erase(const Resource &r,
                                const google::protobuf::Message &key) {
  if (auto ok = Bind(r); !ok) {
    return ok;
  }
  if (auto ok = CheckType(r, "key", key, r.key_type); !ok) {
    return ok;
  }
  pb::TransactionOp op;
  op.set_resource(r.name);
  op.set_erase(true);
  if (!op.mutable_key()->PackFrom(key)) {
    return std::unexpected(MakeError(ErrorKind::kInvalidRequest, "cannot pack the key"));
  }
  ops_.push_back(std::move(op));
  return {};
}

Result<Applied> Transaction::Commit(std::stop_token stop) {
  if (result_) {
    return *result_;
  }
  if (ops_.empty()) {
    return std::unexpected(MakeError(ErrorKind::kInvalidRequest, "empty transaction"));
  }
  pb::ApplyTransactionRequest req;
  req.set_request_id(request_id_);
  for (const auto &op : ops_) {
    *req.add_ops() = op;
  }
  req.set_consistency(snapshot_
                          ? pb::ApplyTransactionRequest::CONSISTENCY_SCOPE_SNAPSHOT
                          : pb::ApplyTransactionRequest::CONSISTENCY_REFERENTIAL);
  if (expected_generation_) {
    req.set_expected_generation(*expected_generation_);
  }
  auto applied = client_->Commit(req, epoch_.value_or(0), stop);
  if (applied) {
    result_ = *applied;
  }
  return applied;
}

// -- pipeline builder -------------------------------------------------------------

// Packs `arg` into `out()` (called only for an argument: no empty Any).
template <typename Out>
void PipelineBuilder::Pack(const google::protobuf::Message *arg, Out &&out) {
  if (arg == nullptr || error_) {
    return;
  }
  if (!out()->PackFrom(*arg)) {
    error_ = MakeError(ErrorKind::kInvalidRequest,
                       "cannot pack " + std::string(arg->GetDescriptor()->full_name()));
  }
}

PipelineBuilder &PipelineBuilder::Port(std::string_view name,
                                       std::string_view driver,
                                       const google::protobuf::Message *arg) {
  pb::Port port;
  port.set_name(std::string(name));
  port.set_driver(std::string(driver));
  Pack(arg, [&] { return port.mutable_arg(); });
  return PortSpec(std::move(port));
}

PipelineBuilder &PipelineBuilder::PortSpec(pb::Port port) {
  *p_.add_ports() = std::move(port);
  return *this;
}

PipelineBuilder &PipelineBuilder::Module(std::string_view name,
                                         std::string_view mclass,
                                         const google::protobuf::Message *arg) {
  pb::Module *m = p_.add_modules();
  m->set_name(std::string(name));
  m->set_mclass(std::string(mclass));
  Pack(arg, [&] { return m->mutable_arg(); });
  return *this;
}

PipelineBuilder &PipelineBuilder::Connect(std::string_view upstream,
                                          uint32_t ogate,
                                          std::string_view downstream,
                                          uint32_t igate) {
  pb::Connection *c = p_.add_connections();
  c->set_upstream(std::string(upstream));
  c->set_ogate(ogate);
  c->set_downstream(std::string(downstream));
  c->set_igate(igate);
  return *this;
}

PipelineBuilder &PipelineBuilder::Chain(
    std::initializer_list<std::string_view> names) {
  const std::string_view *prev = nullptr;
  for (const std::string_view &name : names) {
    if (prev != nullptr) {
      Connect(*prev, 0, name, 0);
    }
    prev = &name;
  }
  return *this;
}

PipelineBuilder &PipelineBuilder::Worker(int32_t wid, int32_t core,
                                         std::string_view scheduler) {
  pb::Worker *w = p_.add_workers();
  w->set_wid(wid);
  w->set_core(core);
  w->set_scheduler(std::string(scheduler));
  return *this;
}

PipelineBuilder &PipelineBuilder::TrafficClass(pb::TrafficClass tc) {
  *p_.add_traffic_classes() = std::move(tc);
  return *this;
}

namespace {
template <typename Field, typename Pred>
void EraseIf(Field *field, Pred &&pred) {
  for (int i = field->size() - 1; i >= 0; i--) {
    if (pred(field->Get(i))) {
      field->DeleteSubrange(i, 1);
    }
  }
}
}  // namespace

PipelineBuilder &PipelineBuilder::Remove(std::string_view name) {
  EraseIf(p_.mutable_ports(), [&](const pb::Port &p) { return p.name() == name; });
  EraseIf(p_.mutable_modules(), [&](const pb::Module &m) { return m.name() == name; });
  EraseIf(p_.mutable_traffic_classes(),
          [&](const pb::TrafficClass &tc) { return tc.name() == name; });
  EraseIf(p_.mutable_connections(), [&](const pb::Connection &c) {
    return c.upstream() == name || c.downstream() == name;
  });
  return *this;
}

Result<pb::Pipeline> PipelineBuilder::Build() const {
  if (error_) {
    return std::unexpected(*error_);
  }
  return p_;
}

}  // namespace bess::sdk
