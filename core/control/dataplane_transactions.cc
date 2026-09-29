// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "control/dataplane_transactions.h"

#include <expected>
#include <functional>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "control/resource_codec.h"

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

DataplaneTransactions::DataplaneTransactions(TransactionEngine &engine)
    : DataplaneTransactions(engine, DrawEpoch()) {}

ControlResult<v2::ApplyTransactionResponse> DataplaneTransactions::Apply(
    const v2::ApplyTransactionRequest &request) {
  v2::ApplyTransactionResponse response;
  response.set_daemon_epoch(epoch_);
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
    const dataplane::ResourceCodec *codec = resource->codec();
    if (codec == nullptr) {
      return std::unexpected("resource '" + op.resource() +
                             "' is not reachable over the RPC");
    }
    auto key = codec->Key(op.key());
    if (!key) {
      return std::unexpected(key.error());
    }
    if (op.erase()) {
      if (op.has_value()) {
        return std::unexpected("an erase takes no value");
      }
      return Op::Erase(op.resource(), std::move(*key));
    }
    auto value = codec->Value(op.value());
    if (!value) {
      return std::unexpected(value.error());
    }
    return Op::Upsert(op.resource(), std::move(*key), std::move(*value));
  };
  int failed = -1;
  std::string error;
  for (int i = 0; i < n; i++) {
    auto op = decode(request.ops(i));
    if (!op) {
      failed = i;
      error = std::move(op.error());
      break;
    }
    ops.push_back(std::move(*op));
  }

  v2::TransactionRecord record;
  if (failed >= 0) {
    record = Undecodable(n, failed, std::move(error), engine_.generation());
  } else {
    std::optional<uint64_t> expected;
    if (request.has_expected_generation()) {
      expected = request.expected_generation();
    }
    const TransactionEngine::Result result = engine_.Apply(ops, expected);
    record.set_outcome(ToProto(result.outcome));
    record.set_generation(result.generation);
    for (const auto &op : result.ops) {
      auto *out = record.add_ops();
      out->set_status(ToProto(op.status));
      out->set_error(op.error);
    }
  }
  record.set_request_id(id);
  record.set_visibility(v2::TransactionRecord::VISIBILITY_DEPENDENCY_ORDERED);
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
    const dataplane::ResourceCodec *codec =
        resource == nullptr ? nullptr : resource->codec();
    if (codec == nullptr) {
      continue;  // not reachable over the RPC
    }
    auto *out = response.add_resources();
    out->set_name(name);
    out->set_key_type(codec->key_type());
    out->set_value_type(codec->value_type());
  }
  return response;
}

}  // namespace bess::control
