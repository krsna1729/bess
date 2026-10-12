// SPDX-License-Identifier: BSD-3-Clause

// The C++ client for BESS's generic control API (control_v2), with the same
// guarantees as the Go SDK (sdk/go) and Python's pybess.sdk
// (docs/control-sdk.md):
//
//  - one request id per logical transaction, kept across retries;
//  - after a send without an answer the outcome is asked for, never assumed,
//    and the request is sent again only once GetTransaction answered "not
//    known" under the daemon epoch the commit started with;
//  - an answer from any other epoch is a kDaemonRestarted error;
//  - typed resources are checked before anything is sent;
//  - a resource handle is bound to the daemon epoch it was discovered under,
//    and one from before a restart the client has seen is refused
//    (kStaleResource), never sent;
//  - outcomes are typed (Error::kind).
//
// It holds no application semantics: resources are whatever modules
// registered ("<module>/<table>"), keys and values their own messages.
//
//   auto client = bess::sdk::Client::Dial("localhost:10514");
//   auto rules = client->Resource("em0/rules");
//   auto tx = client->NewTransaction();
//   tx.Upsert(*rules, key, value);
//   auto applied = tx.Commit();
//
// Desired state: PipelineBuilder assembles a control_v2 Pipeline (or edits
// the snapshot Client::Pipeline returns); the daemon validates, diffs, plans
// and applies it. ApplyPipeline retries a busy answer and never a conflict or
// a refusal; no answer is kTransport and nothing is resent.
//
// Requires C++23 (std::expected). A Client is safe for concurrent use; a
// Transaction is not.

#ifndef BESS_SDK_CLIENT_H_
#define BESS_SDK_CLIENT_H_

#include <grpcpp/support/status.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "bess/sdk/transport.h"
#include "control_v2.pb.h"

namespace bess::sdk {

namespace pb = ::bess::pb::v2;

// bessd's message size limit, each way (D-026).
inline constexpr int kMaxMessageBytes = 64 << 20;

// -- errors -------------------------------------------------------------------

enum class ErrorKind {
  // Refused before anything was applied: a bad resource, key or value, or a
  // request id reused with other contents. `code` is the call's status
  // (OK: refused by the client), `detail` the server's, when it sent one.
  kInvalidRequest,
  // expected_generation no longer matched; this attempt tried nothing.
  // `after_unknown_attempt`: an earlier send of the same commit went
  // unanswered and may have applied (its record since aged out): re-read.
  kConflict,
  // An operation failed and nothing changed (`failures`).
  kRejected,
  // The dataplane stayed busy through every attempt, and every send was
  // answered (nothing applied).
  kBusy,
  // No answer. The outcome is unknown, not failed: ask
  // GetTransaction(request_id) later.
  kTransport,
  // ApplyPipeline's expected generation no longer matched; nothing changed.
  kPipelineConflict,
  // The daemon restarted since the request was built (`from_epoch` ->
  // `to_epoch`). It refuses requests built under another epoch (D-106), so
  // nothing of this one is applied in the running daemon, but what the
  // request was built from is gone: re-read before deciding again.
  kDaemonRestarted,
  // A resource handle, or a transaction built with one, from an earlier
  // daemon epoch than the one the client has seen. Nothing was sent: look the
  // resource up again and rebuild the transaction.
  kStaleResource,
  // The caller's stop token was requested before an outcome was known, and
  // nothing was left unanswered.
  kCancelled,
};

struct OpFailure {
  int index = 0;
  std::string error;
};

struct Error {
  ErrorKind kind = ErrorKind::kInvalidRequest;
  std::string message;  // what happened, for people
  grpc::StatusCode code = grpc::StatusCode::OK;      // kInvalidRequest, kTransport
  std::optional<pb::ErrorDetail> detail;  // kInvalidRequest, kPipelineConflict, answered kBusy
  std::optional<pb::TransactionRecord> record;       // kConflict, kRejected
  bool after_unknown_attempt = false;                // kConflict
  std::vector<OpFailure> failures;                   // kRejected
  int attempts = 0;                                  // kBusy
  std::string request_id;                            // kTransport
  std::string resource;  // kStaleResource; empty: the transaction as a whole
  uint64_t from_epoch = 0;  // kDaemonRestarted: from; kStaleResource: bound
  uint64_t to_epoch = 0;    // kDaemonRestarted: to; kStaleResource: daemon
};

template <typename T>
using Result = std::expected<T, Error>;

// -- results ------------------------------------------------------------------

// A transaction that was applied.
struct Applied {
  std::string request_id;
  uint64_t generation = 0;
  pb::TransactionRecord::Visibility visibility{};
  bool replayed = false;  // an earlier identical request's recorded outcome
  uint64_t daemon_epoch = 0;
  pb::TransactionRecord record;
};

// A transactional resource, its key and value message types, and the daemon
// epoch it was discovered under (valid only in that epoch).
struct Resource {
  std::string name;
  std::string key_type;
  std::string value_type;
  uint64_t daemon_epoch = 0;
};

// How long one RPC may take and how many RPCs a commit may make (status
// queries included: 2 is one send plus one question).
struct RetryPolicy {
  std::chrono::nanoseconds attempt_timeout = std::chrono::seconds(5);
  int attempts = 4;
  std::chrono::nanoseconds busy_backoff = std::chrono::milliseconds(10);  // doubled per BUSY
};

// A pipeline generation and the daemon epoch it belongs to: generations count
// from 1 again after a restart, so one means something only with its epoch
// (D-106). ApplyPipeline's expected version comes from a snapshot, diff, plan
// or earlier apply.
struct PipelineVersion {
  uint64_t generation = 0;
  uint64_t daemon_epoch = 0;
  bool operator==(const PipelineVersion &) const = default;
};

struct PipelineSnapshot {
  pb::Pipeline pipeline;
  PipelineVersion version;
};

struct PipelineDiffResult {
  pb::PipelineDiff diff;
  PipelineVersion version;  // what the diff was taken against
};

struct PipelinePlan {
  std::vector<pb::PlanStep> steps;
  PipelineVersion version;  // what the plan was made against
};

struct TransactionStatus {
  bool known = false;
  pb::TransactionRecord record;
  uint64_t daemon_epoch = 0;
};

class Transaction;

struct TransactionOptions {
  // Refuse the transaction (kConflict) unless the generation is still this.
  std::optional<uint64_t> expected_generation;
  bool snapshot = false;   // CONSISTENCY_SCOPE_SNAPSHOT (D-050); default referential
  std::string request_id;  // resume a known request id (a client that restarted)
};

// -- the client ---------------------------------------------------------------

// One daemon's control endpoint.
class Client {
 public:
  using SleepFn = std::function<void(std::chrono::nanoseconds)>;

  // Connects to bessd's gRPC endpoint (no TLS, like bessd).
  static std::unique_ptr<Client> Dial(const std::string &target,
                                      RetryPolicy retry = {});

  // Uses `transport` (a test's, or another channel's). `sleep` waits between
  // busy retries (default: std::this_thread::sleep_for).
  explicit Client(std::shared_ptr<Transport> transport, RetryPolicy retry = {},
                  SleepFn sleep = {});

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  // The last epoch the daemon reported (nullopt: none yet).
  std::optional<uint64_t> DaemonEpoch() const;

  // Every transactional resource, by name (cached unless `refresh`). A copy.
  Result<std::map<std::string, sdk::Resource>> Resources(bool refresh = false);
  // The resource `name`; kInvalidRequest if no module registered it.
  Result<sdk::Resource> Resource(std::string_view name);

  // What the daemon offers (version, RPCs, plugin API, module classes, ...).
  // A daemon too old to serve it answers kInvalidRequest with UNIMPLEMENTED.
  Result<pb::GetCapabilitiesResponse> Capabilities();
  // Whether the daemon serves `rpc` ("ApplyTransaction" or a full name). A
  // daemon too old to answer GetCapabilities serves none of the newer RPCs.
  Result<bool> Supports(std::string_view rpc);
  // The argument types of every module class, port driver and module command,
  // with the descriptors to encode them (GetSchema): what a client without
  // generated code for them (a manifest, a UI) needs. Not cached: plugins
  // change it.
  Result<pb::GetSchemaResponse> Schema();
  // Every metric sample.
  Result<std::vector<pb::MetricSample>> Metrics();

  // Streams operational events, oldest first, from `from` (0: the next
  // event), calling `on_event` for each until it returns false, `stop` is
  // requested (both: success), or an error. To resume from a saved sequence,
  // pass the epoch it belongs to (0: unknown). With `types`, only those (gaps
  // always). A "bess.gap" event names events the daemon no longer held.
  // After a transport failure the stream resumes where it stopped, even
  // before its first event. kDaemonRestarted: what was built from earlier
  // events is gone (re-read the state, watch from 0). "bess.start" and
  // "bess.progress" are not delivered. Blocks: run it on its own thread.
  Result<void> WatchEvents(uint64_t from, uint64_t epoch,
                           const std::vector<std::string> &types,
                           const std::function<bool(const pb::Event &)> &on_event,
                           std::stop_token stop = {});

  // -- desired state --
  Result<PipelineSnapshot> Pipeline();
  // The daemon's canonical form of `p` (kInvalidRequest if invalid).
  Result<pb::Pipeline> ValidatePipeline(const pb::Pipeline &p);
  Result<PipelineDiffResult> DiffPipeline(const pb::Pipeline &p);
  Result<PipelinePlan> PlanPipeline(const pb::Pipeline &p);
  // Makes `p` the active pipeline, all or nothing; with `expected` only if
  // the active version is still that (kPipelineConflict if the generation
  // moved, kDaemonRestarted if the epoch did). Every apply names the daemon
  // epoch it was built under: `expected`'s, else the last one the client saw.
  Result<pb::ApplyPipelineResponse> ApplyPipeline(
      const pb::Pipeline &p, std::optional<PipelineVersion> expected = {},
      std::stop_token stop = {});

  // -- transactions --
  // The transaction refers to this client: it must not outlive it.
  // A module's command (D-115; GetSchema lists each class's commands and
  // argument types): its answer, an empty Any when it has none. Sent under
  // the epoch the client knows, not retried: a command acts when it runs. A
  // command's EBUSY is kBusy with the daemon's ErrorDetail (a command not
  // marked thread-safe while a worker runs the module: PauseWorkers first).
  // Other failures are kInvalidRequest with the ErrorDetail, the command's
  // errno in errno_value.
  Result<google::protobuf::Any> RunModuleCommand(const std::string &module,
                                                 const std::string &command,
                                                 const google::protobuf::Message &arg);

  Transaction NewTransaction(TransactionOptions options = {});

  // `request_id`'s outcome under the current epoch.
  Result<TransactionStatus> GetTransaction(const std::string &request_id);

  // -- administration (the Admin service, D-107) --
  // The mutating calls name the epoch the client has seen (asked for if
  // none); under another the daemon refuses: kDaemonRestarted, nothing done.
  // No answer is kTransport: they are not retried.
  // Loads the plugin at `path` (a path on the daemon's host).
  Result<void> ImportPlugin(const std::string &path);
  Result<void> UnloadPlugin(const std::string &path);
  // Link and counters of the ports `names` in that order, or of every port by
  // name; an unknown name is kInvalidRequest (NOT_FOUND).
  Result<std::vector<pb::PortStatus>> Ports(const std::vector<std::string> &names = {});
  // The default packet pool of each NUMA socket.
  Result<std::vector<pb::PacketPool>> PacketPools();
  // Starts or stops a capture (`request` without its epoch).
  Result<void> SetCapture(pb::SetCaptureRequest request);
  // Pauses every worker; resumes every worker (running the resume hooks
  // first). A worker a pipeline creates starts paused.
  Result<void> PauseWorkers();
  Result<void> ResumeWorkers();
  // Asks the daemon to stop; it answers first.
  Result<void> Shutdown();

 private:
  friend class Transaction;

  template <typename Resp, typename Call>
  Result<Resp> Unary(Call &&call);
  // `sent_epoch`: the epoch a mutating request named (0 for reads).
  template <typename Resp, typename Call>
  Result<Resp> PipelineCall(Call &&call, bool retry_busy, std::stop_token stop,
                            uint64_t sent_epoch = 0);
  // A mutating Admin call: names the known epoch, checks the answer's.
  template <typename Resp, typename Req, typename Call>
  Result<void> Administer(Req req, Call &&call);
  bool ObserveEpoch(uint64_t epoch);
  // The epoch the client has seen, asked for if it has seen none.
  Result<uint64_t> KnownEpoch();
  Result<void> CheckEpoch(uint64_t answered, uint64_t expected);
  Result<Applied> Commit(const pb::ApplyTransactionRequest &req, uint64_t epoch,
                         std::stop_token stop);
  Deadline AttemptDeadline() const;

  std::shared_ptr<Transport> transport_;
  RetryPolicy retry_;
  SleepFn sleep_;

  mutable std::mutex mu_;
  std::optional<std::map<std::string, sdk::Resource>> resources_;
  std::optional<uint64_t> epoch_;
};

// Operations on any resources, applied all or nothing by Commit. It refers to
// the Client that made it, which must outlive it.
class Transaction {
 public:
  // One id for the whole life of the transaction: a retry after a timeout
  // must be recognised as the same request.
  const std::string &request_id() const { return request_id_; }

  // Adds key -> value to `r`, or replaces its value.
  Result<void> Upsert(const Resource &r, const google::protobuf::Message &key,
                      const google::protobuf::Message &value);
  // Removes `key` from `r`.
  Result<void> Erase(const Resource &r, const google::protobuf::Message &key);

  // Applies every operation or none. A committed transaction returns its
  // result again without sending.
  Result<Applied> Commit(std::stop_token stop = {});

 private:
  friend class Client;
  Transaction(Client *client, TransactionOptions options);
  Result<void> Bind(const Resource &r);

  Client *client_;
  std::string request_id_;
  std::vector<pb::TransactionOp> ops_;
  std::optional<uint64_t> expected_generation_;
  bool snapshot_ = false;
  std::optional<Applied> result_;
  std::optional<uint64_t> epoch_;  // the epoch of the resources the ops use
};

// Assembles a control_v2 Pipeline. It only builds the message: the daemon
// validates, diffs and plans it.
class PipelineBuilder {
 public:
  PipelineBuilder() = default;
  // Starts from `base` (a snapshot's pipeline, copied).
  explicit PipelineBuilder(pb::Pipeline base) : p_(std::move(base)) {}

  // A port with its argument (nullptr: none); PortSpec for queue settings.
  PipelineBuilder &Port(std::string_view name, std::string_view driver,
                        const google::protobuf::Message *arg);
  PipelineBuilder &PortSpec(pb::Port port);
  // A module with its Init argument (nullptr: none).
  PipelineBuilder &Module(std::string_view name, std::string_view mclass,
                          const google::protobuf::Message *arg);
  // upstream:ogate -> igate:downstream.
  PipelineBuilder &Connect(std::string_view upstream, uint32_t ogate,
                           std::string_view downstream, uint32_t igate);
  // Gate 0 to gate 0 along `names`.
  PipelineBuilder &Chain(std::initializer_list<std::string_view> names);
  PipelineBuilder &Worker(int32_t wid, int32_t core, std::string_view scheduler);
  PipelineBuilder &TrafficClass(pb::TrafficClass tc);
  // Where a module's task is scheduled: under a traffic class or at a worker.
  PipelineBuilder &Task(pb::TaskPlacement task);
  // Drops the port, module or traffic class `name` and the connections and
  // task placements that name it.
  PipelineBuilder &Remove(std::string_view name);

  // A copy of the pipeline built so far, or the first argument that could
  // not be packed.
  Result<pb::Pipeline> Build() const;

 private:
  template <typename Out>
  void Pack(const google::protobuf::Message *arg, Out &&out);

  pb::Pipeline p_;
  std::optional<Error> error_;
};

}  // namespace bess::sdk

#endif  // BESS_SDK_CLIENT_H_
