// SPDX-License-Identifier: BSD-3-Clause

#include "control/dataplane_transactions.h"

#include <cerrno>
#include <expected>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "framework/resource_bindings.h"

namespace bess::control {

namespace {

namespace v2 = pb::v2;
using dataplane::Op;
using dataplane::TransactionEngine;

uint64_t DrawEpoch() {
  std::random_device rd;
  uint64_t epoch = 0;
  while (epoch == 0) {
    epoch = (uint64_t{rd()} << 32) | rd();
  }
  return epoch;
}

// A digest of what the request asks for (everything but its id).
uint64_t Digest(const v2::ApplyTransactionRequest &request) {
  v2::ApplyTransactionRequest contents = request;
  contents.clear_request_id();
  std::string bytes;
  bool serialized = false;
  google::protobuf::io::StringOutputStream stream(&bytes);
  {
    google::protobuf::io::CodedOutputStream out(&stream);
    out.SetSerializationDeterministic(true);
    serialized = contents.SerializeToCodedStream(&out);
  }
  // Only a message beyond protobuf's 2 GiB limit fails; gRPC would not have
  // delivered it. Hash what there is regardless.
  return std::hash<std::string>{}(bytes) ^ (serialized ? 0 : 1);
}

v2::TransactionRecord::Outcome ToProto(TransactionEngine::Outcome outcome) {
  switch (outcome) {
    case TransactionEngine::Outcome::kApplied:
      return v2::TransactionRecord::OUTCOME_APPLIED;
    case TransactionEngine::Outcome::kRejected:
      return v2::TransactionRecord::OUTCOME_REJECTED;
    case TransactionEngine::Outcome::kConflict:
      return v2::TransactionRecord::OUTCOME_CONFLICT;
    case TransactionEngine::Outcome::kBusy:
      return v2::TransactionRecord::OUTCOME_BUSY;
    case TransactionEngine::Outcome::kUnsupported:
      break;  // answered as a typed error before a record is built
  }
  return v2::TransactionRecord::OUTCOME_REJECTED;
}

v2::TransactionOpResult::Status ToProto(TransactionEngine::OpStatus status) {
  switch (status) {
    case TransactionEngine::OpStatus::kApplied:
      return v2::TransactionOpResult::STATUS_APPLIED;
    case TransactionEngine::OpStatus::kFailed:
      return v2::TransactionOpResult::STATUS_FAILED;
    case TransactionEngine::OpStatus::kNotApplied:
      return v2::TransactionOpResult::STATUS_NOT_APPLIED;
  }
  return v2::TransactionOpResult::STATUS_NOT_APPLIED;
}

// Rejected at decoding: operation `failed` carries the reason, the others
// were not applied (the engine's own rejection shape).
v2::TransactionRecord Undecodable(int ops, int failed, std::string error,
                                  uint64_t generation) {
  v2::TransactionRecord record;
  record.set_outcome(v2::TransactionRecord::OUTCOME_REJECTED);
  record.set_generation(generation);
  for (int i = 0; i < ops; i++) {
    auto *op = record.add_ops();
    if (i == failed) {
      op->set_status(v2::TransactionOpResult::STATUS_FAILED);
      op->set_error(error);
    } else {
      op->set_status(v2::TransactionOpResult::STATUS_NOT_APPLIED);
    }
  }
  return record;
}

}  // namespace

DataplaneTransactions::DataplaneTransactions(
    TransactionEngine &engine, const framework::ResourceBindings &bindings)
    : DataplaneTransactions(engine, bindings, DrawEpoch()) {}

ControlResult<v2::ApplyTransactionResponse> DataplaneTransactions::Apply(
    const v2::ApplyTransactionRequest &request, TransactionEngine::OnBusy on_busy) {
  v2::ApplyTransactionResponse response;
  response.set_daemon_epoch(epoch_);
  // A level this build does not know is refused: reading it as referential
  // would serve a stronger request with a weaker transaction.
  dataplane::Consistency consistency = dataplane::Consistency::kReferential;
  switch (request.consistency()) {
    case v2::ApplyTransactionRequest::CONSISTENCY_UNSPECIFIED:
    case v2::ApplyTransactionRequest::CONSISTENCY_REFERENTIAL:
      break;
    case v2::ApplyTransactionRequest::CONSISTENCY_SCOPE_SNAPSHOT:
      consistency = dataplane::Consistency::kScopeSnapshot;
      break;
    default:
      return std::unexpected(ControlError{
          .code = ControlErrorCode::kInvalidArgument,
          .err = EINVAL,
          .message = "unknown consistency level " +
                     std::to_string(static_cast<int>(request.consistency())),
          .object = "",
          .field = "consistency"});
  }
  const std::string &id = request.request_id();
  const uint64_t digest = id.empty() ? 0 : Digest(request);
  if (!id.empty()) {
    if (auto it = records_.find(id); it != records_.end()) {
      if (it->second.digest != digest) {
        return std::unexpected(ControlError{
            .code = ControlErrorCode::kConflict,
            .err = EEXIST,
            .message = "request_id '" + id +
                       "' was used for a different transaction",
            .object = id});
      }
      *response.mutable_record() = it->second.record;
      response.set_replayed(true);
      return response;
    }
  }

  // A resource whose values workers read unsynchronized is refused while a
  // worker runs its module (D-129): nothing is attempted or recorded, as for
  // a thread-unsafe command; pause the workers and send the same request.
  for (const v2::TransactionOp &op : request.ops()) {
    const dataplane::Resource *resource = engine_.FindResource(op.resource());
    const framework::ResourceCodec *codec =
        resource == nullptr ? nullptr : bindings_.Find(*resource);
    if (codec != nullptr && codec->Busy()) {
      return std::unexpected(ControlError{
          .code = ControlErrorCode::kResourceBusy,
          .err = EBUSY,
          .message = "resource '" + op.resource() +
                     "' changes only with its module's workers paused (a worker runs it)",
          .object = op.resource(),
          .field = "ops"});
    }
  }

  // Decode every operation through its resource's codec; the first that
  // cannot be decoded rejects the transaction.
  const int n = request.ops_size();
  std::vector<Op> ops;
  ops.reserve(static_cast<size_t>(n));
  auto decode = [&](const v2::TransactionOp &op)
      -> std::expected<Op, std::string> {
    const dataplane::Resource *resource = engine_.FindResource(op.resource());
    if (resource == nullptr) {
      return std::unexpected("unknown resource '" + op.resource() + "'");
    }
    const framework::ResourceCodec *codec = bindings_.Find(*resource);
    if (codec == nullptr) {
      return std::unexpected("resource '" + op.resource() +
                             "' is not reachable over the RPC");
    }
    auto key = codec->Key(op.key().type_url(), op.key().value());
    if (!key) {
      return std::unexpected(key.error());
    }
    if (op.erase()) {
      if (op.has_value()) {
        return std::unexpected("an erase takes no value");
      }
      return Op::Erase(op.resource(), std::move(*key));
    }
    auto value = codec->Value(op.value().type_url(), op.value().value());
    if (!value) {
      return std::unexpected(value.error());
    }
    return Op::Upsert(op.resource(), std::move(*key), std::move(*value));
  };
  // D-198: memory refused while decoding or reserving (a value's storage, a
  // table's growth) answers RESOURCE_EXHAUSTED with nothing visible. Decoding
  // touches no table, and the engine allocates only in its Reserve phase,
  // whose abort guard undoes every reservation on unwind; publication
  // allocates nothing (D-172). Not recorded under the id, as nothing was
  // applied. Only these two steps are covered: once the engine has returned,
  // a throw would report an applied transaction as refused.
  int failed = -1;
  std::string error;
  try {
    for (int i = 0; i < n; i++) {
      auto op = decode(request.ops(i));
      if (!op) {
        failed = i;
        error = std::move(op.error());
        break;
      }
      ops.push_back(std::move(*op));
    }
  } catch (const std::bad_alloc &e) {
    ControlError refused = OutOfMemory(e);
    refused.field = "ops";
    return std::unexpected(std::move(refused));
  }

  v2::TransactionRecord record;
  if (failed >= 0) {
    record = Undecodable(n, failed, std::move(error), engine_.generation());
  } else {
    std::optional<uint64_t> expected;
    if (request.has_expected_generation()) {
      expected = request.expected_generation();
    }
    std::optional<TransactionEngine::Result> applied;
    try {
      applied.emplace(engine_.Apply(ops, expected, consistency, on_busy));
    } catch (const std::bad_alloc &e) {
      ControlError refused = OutOfMemory(e);
      refused.field = "ops";
      return std::unexpected(std::move(refused));
    }
    const TransactionEngine::Result &result = *applied;
    if (result.outcome == TransactionEngine::Outcome::kUnsupported) {
      // A typed error, not a record: the request names a promise a resource
      // cannot keep, which retrying will not change. Nothing was attempted,
      // so nothing is recorded under the id.
      ControlError error{.code = ControlErrorCode::kUnsupportedTransaction,
                         .err = ENOTSUP,
                         .message = "unsupported consistency",
                         .object = "",
                         .field = "consistency"};
      for (size_t i = 0; i < result.ops.size(); i++) {
        if (result.ops[i].status == TransactionEngine::OpStatus::kFailed) {
          error.message = result.ops[i].error;
          error.object = ops[i].resource;
          break;
        }
      }
      return std::unexpected(std::move(error));
    }
    record.set_outcome(ToProto(result.outcome));
    record.set_generation(result.generation);
    for (const auto &op : result.ops) {
      auto *out = record.add_ops();
      out->set_status(ToProto(op.status));
      out->set_error(op.error);
    }
  }
  record.set_request_id(id);
  record.set_visibility(consistency == dataplane::Consistency::kScopeSnapshot
                            ? v2::TransactionRecord::VISIBILITY_SCOPE_SNAPSHOT
                            : v2::TransactionRecord::VISIBILITY_DEPENDENCY_ORDERED);
  // BUSY and CONFLICT attempted nothing: the client retries under the id.
  const bool attempted = record.outcome() == v2::TransactionRecord::OUTCOME_APPLIED ||
                         record.outcome() == v2::TransactionRecord::OUTCOME_REJECTED;
  if (!id.empty() && attempted) {
    Record(id, digest, record);
  }
  *response.mutable_record() = std::move(record);
  return response;
}

void DataplaneTransactions::Record(const std::string &request_id,
                                   uint64_t digest,
                                   const v2::TransactionRecord &record) {
  if (order_.size() == kMaxRecords) {
    records_.erase(order_.front());
    order_.pop_front();
  }
  records_.emplace(request_id, Recorded{digest, record});
  order_.push_back(request_id);
}

v2::GetTransactionResponse DataplaneTransactions::Get(
    const std::string &request_id) const {
  v2::GetTransactionResponse response;
  response.set_daemon_epoch(epoch_);
  if (auto it = records_.find(request_id); it != records_.end()) {
    response.set_known(true);
    *response.mutable_record() = it->second.record;
  }
  return response;
}

v2::ListTransactionResourcesResponse DataplaneTransactions::List() const {
  v2::ListTransactionResourcesResponse response;
  response.set_daemon_epoch(epoch_);
  response.set_generation(engine_.generation());
  for (const std::string &name : engine_.ResourceNames()) {
    const dataplane::Resource *resource = engine_.FindResource(name);
    const framework::ResourceCodec *codec =
        resource == nullptr ? nullptr : bindings_.Find(*resource);
    if (codec == nullptr) {
      continue;  // not reachable over the RPC
    }
    auto *out = response.add_resources();
    out->set_name(name);
    out->set_key_type(codec->key_type());
    out->set_value_type(codec->value_type());
    out->set_kind(codec->kind() == framework::ResourceCodec::Kind::kSingleton
                      ? v2::TransactionResource::KIND_SINGLETON
                      : v2::TransactionResource::KIND_MAP);
    out->set_requires_paused_workers(codec->requires_paused_workers());
  }
  return response;
}

v2::GetResourceStatusResponse DataplaneTransactions::ResourceStatus() const {
  v2::GetResourceStatusResponse response;
  response.set_daemon_epoch(epoch_);
  response.set_generation(engine_.generation());
  VisitReachableResources([&](const dataplane::Resource &resource) {
    auto *out = response.add_resources();
    out->set_name(resource.name());
    out->set_live(resource.LiveCount());
    if (const std::optional<uint64_t> capacity = resource.Capacity()) {
      out->set_has_capacity(true);
      out->set_capacity(*capacity);
    }
  });
  return response;
}

}  // namespace bess::control
