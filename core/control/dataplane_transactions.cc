// SPDX-License-Identifier: BSD-3-Clause

#include "control/dataplane_transactions.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <expected>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream.h>

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

// Hashes (FNV-1a) what is serialized into it, a buffer at a time, so the
// digest of a request costs no copy of it (D-207).
class HashStream final : public google::protobuf::io::ZeroCopyOutputStream {
 public:
  bool Next(void **data, int *size) override {
    Flush();
    *data = buffer_;
    *size = sizeof(buffer_);
    pending_ = sizeof(buffer_);
    count_ += pending_;
    return true;
  }
  void BackUp(int count) override {
    pending_ -= count;
    count_ -= count;
  }
  int64_t ByteCount() const override { return count_; }
  uint64_t Finish() {
    Flush();
    return hash_;
  }

 private:
  void Flush() {
    for (int i = 0; i < pending_; i++) {
      hash_ = (hash_ ^ buffer_[i]) * 0x100000001b3ULL;
    }
    pending_ = 0;
  }

  unsigned char buffer_[4096];
  int pending_ = 0;
  int64_t count_ = 0;
  uint64_t hash_ = 0xcbf29ce484222325ULL;
};

// A digest of the request, its deterministic serialization hashed as it is
// written. The id is part of it: digests are only compared under one id.
uint64_t Digest(const v2::ApplyTransactionRequest &request) {
  HashStream stream;
  bool serialized = false;
  {
    google::protobuf::io::CodedOutputStream out(&stream);
    out.SetSerializationDeterministic(true);
    serialized = request.SerializeToCodedStream(&out);
  }
  // Only a message beyond protobuf's 2 GiB limit fails; gRPC would not have
  // delivered it. Hash what there is regardless.
  return stream.Finish() ^ (serialized ? 0u : 1u);
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

// A window entry's record, which CommitRecord serialized: it parses.
void ReadRecord(std::string_view bytes, v2::TransactionRecord *record) {
  static_cast<void>(
      record->ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
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
      *response.mutable_record() = Expand(id, it->second);
      response.set_replayed(true);
      return response;
    }
  }

  // The request's operations in the engine's order: the plain ops, then each
  // scope replacement's objects and its scope last (D-213). A record's results
  // follow it. A request without replacements (the common path) keeps its
  // operations where they are: no per-transaction allocation.
  std::vector<const v2::TransactionOp *> flat;
  std::vector<TransactionEngine::ScopeReplacement> scopes;
  const bool phased = request.replace_scopes_size() > 0;
  if (phased) {
    try {
      size_t total = static_cast<size_t>(request.ops_size());
      for (const auto &replacement : request.replace_scopes()) {
        total += replacement.objects_size() + 1;
      }
      flat.reserve(total);  // scale: one entry per request operation
      scopes.reserve(static_cast<size_t>(request.replace_scopes_size()));
      for (const auto &op : request.ops()) {
        flat.push_back(&op);
      }
      for (const auto &replacement : request.replace_scopes()) {
        scopes.push_back({flat.size(), static_cast<size_t>(replacement.objects_size())});
        for (const auto &object : replacement.objects()) {
          flat.push_back(&object);
        }
        flat.push_back(&replacement.scope());
      }
    } catch (const std::bad_alloc &e) {
      ControlError refused = OutOfMemory(e);
      refused.field = "ops";
      return std::unexpected(std::move(refused));
    }
  }
  const auto op_at = [&](int i) -> const v2::TransactionOp & {
    return phased ? *flat[static_cast<size_t>(i)] : request.ops(i);
  };

  // Each operation's resource and codec, looked up once for the busy check
  // and the decoding below, under one acquisition of the engine's lock.
  // Inline up to kInlineOps operations (no allocation); longer requests on
  // the heap.
  struct Named {
    const dataplane::Resource *resource;
    const framework::ResourceCodec *codec;
  };
  const int n = phased ? static_cast<int>(flat.size()) : request.ops_size();
  std::array<Named, kInlineOps> inline_named;
  std::vector<Named> heap_named;
  if (static_cast<size_t>(n) > kInlineOps) {
    try {
      heap_named.resize(static_cast<size_t>(n));
    } catch (const std::bad_alloc &e) {
      ControlError refused = OutOfMemory(e);
      refused.field = "ops";
      return std::unexpected(std::move(refused));
    }
  }
  Named *const named = heap_named.empty() ? inline_named.data() : heap_named.data();
  engine_.FindResources(
      static_cast<size_t>(n),
      [&](size_t i) -> const std::string & { return op_at(static_cast<int>(i)).resource(); },
      [&](size_t i, const dataplane::Resource *resource) { named[i].resource = resource; });
  for (int i = 0; i < n; i++) {
    named[i].codec = named[i].resource == nullptr ? nullptr : bindings_.Find(*named[i].resource);
  }

  // A scope replacement's scope must switch in one store; refuse before
  // anything is attempted, as for a scope-snapshot transaction. Its old
  // Version's entries may be erased with it (D-213), in resources the request
  // need not name: those the scope's resource may reference get the D-129
  // check below too.
  for (const TransactionEngine::ScopeReplacement &span : scopes) {
    const size_t at = span.first + span.objects;
    const dataplane::Resource *scope = named[at].resource;
    if (scope == nullptr) {
      continue;  // decoding below rejects it
    }
    const std::string &resource = flat[at]->resource();
    if (scope->ProvidedConsistency() != dataplane::Consistency::kScopeSnapshot) {
      return std::unexpected(ControlError{
          .code = ControlErrorCode::kUnsupportedTransaction,
          .err = ENOTSUP,
          .message = "resource '" + resource + "' cannot switch a scope in one store",
          .object = resource,
          .field = "replace_scopes"});
    }
    // scale: the resources one scope resource declares, a handful.
    for (const std::string &referent : scope->declared_references()) {
      const dataplane::Resource *target = engine_.FindResource(referent);
      const framework::ResourceCodec *codec =
          target == nullptr ? nullptr : bindings_.Find(*target);
      if (codec != nullptr && codec->Busy()) {
        std::string message = "resource '" + referent;
        message += "', whose entries replacing scope resource '";
        message += resource;
        message +=
            "' may retire, changes only with its module's workers paused "
            "(a worker runs it)";
        return std::unexpected(ControlError{.code = ControlErrorCode::kResourceBusy,
                                            .err = EBUSY,
                                            .message = std::move(message),
                                            .object = referent,
                                            .field = "replace_scopes"});
      }
    }
  }

  // A resource whose values workers read unsynchronized is refused while a
  // worker runs its module (D-129): nothing is attempted or recorded, as for
  // a thread-unsafe command; pause the workers and send the same request.
  for (int i = 0; i < n; i++) {
    if (named[i].codec != nullptr && named[i].codec->Busy()) {
      const std::string &resource = op_at(i).resource();
      return std::unexpected(ControlError{
          .code = ControlErrorCode::kResourceBusy,
          .err = EBUSY,
          .message = "resource '" + resource +
                     "' changes only with its module's workers paused (a worker runs it)",
          .object = resource,
          .field = "ops"});
    }
  }

  // The outcome's record is charged before anything is attempted (D-206):
  // a refused charge refuses the transaction with nothing applied -- never an
  // applied transaction that a retry under its id would find unknown and
  // apply again. Given back below unless the outcome is recorded.
  Records::node_type pending;
  // D-207: an erase-only transaction's record draws on the erase headroom,
  // as the erases do in the engine; a replacement erasing its scope with no
  // objects is one.
  const auto erases = [](const v2::TransactionOp &op) { return op.erase(); };
  const utils::Headroom headroom(
      n != 0 && (phased ? std::all_of(flat.begin(), flat.end(),
                                      [&](const v2::TransactionOp *op) { return erases(*op); })
                        : std::all_of(request.ops().begin(), request.ops().end(), erases))
          ? utils::Headroom::kErase
          : utils::Headroom::kNone);
  if (!id.empty()) {
    try {
      pending = ReserveRecord(id, digest);
    } catch (const std::bad_alloc &e) {
      return std::unexpected(OutOfMemory(e));
    }
  }
  struct WindowSlot {
    Order *order;
    ~WindowSlot() {
      if (order != nullptr) {
        order->pop_back();
      }
    }
  } window_slot{pending.empty() ? nullptr : &order_};

  // Decode every operation through its resource's codec; the first that
  // cannot be decoded rejects the transaction.
  std::vector<Op> ops;
  ops.reserve(static_cast<size_t>(n));
  auto decode = [&](int i) -> std::expected<Op, std::string> {
    const v2::TransactionOp &op = op_at(i);
    if (named[i].resource == nullptr) {
      return std::unexpected("unknown resource '" + op.resource() + "'");
    }
    const framework::ResourceCodec *codec = named[i].codec;
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
      auto op = decode(i);
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
      // One transaction, the replacements included: BUSY and CONFLICT
      // attempted nothing, and REJECTED applied nothing.
      applied.emplace(phased ? engine_.Apply(ops, scopes, expected, consistency, on_busy)
                             : engine_.Apply(ops, expected, consistency, on_busy));
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
    // One growth of the repeated field, and an error string only for the
    // operation that has one: an empty proto3 string is the unset field,
    // and setting it allocated a std::string per applied operation.
    record.mutable_ops()->Reserve(static_cast<int>(result.ops.size()));
    for (const auto &op : result.ops) {
      auto *out = record.add_ops();
      out->set_status(ToProto(op.status));
      if (!op.error.empty()) {
        out->set_error(op.error);
      }
      if (!op.observed.empty()) {
        out->set_observed(op.observed);
      }
    }
  }
  record.set_request_id(id);
  // The level the transaction ran under (D-050 §1, D-213): plain operations
  // at the one requested, each replaced scope in one store. A transaction
  // that is only replacements ran wholly at the scope level.
  record.set_visibility(
      consistency == dataplane::Consistency::kScopeSnapshot ||
              (phased && request.ops_size() == 0)
          ? v2::TransactionRecord::VISIBILITY_SCOPE_SNAPSHOT
          : v2::TransactionRecord::VISIBILITY_DEPENDENCY_ORDERED);
  // BUSY and CONFLICT attempted nothing: the client retries under the id.
  const bool attempted = record.outcome() == v2::TransactionRecord::OUTCOME_APPLIED ||
                         record.outcome() == v2::TransactionRecord::OUTCOME_REJECTED;
  if (!pending.empty() && attempted) {
    try {
      CommitRecord(std::move(pending), record);
    } catch (const std::bad_alloc &e) {
      // Only a rejection's errors can outgrow the reserved bytes (an applied
      // record never does): nothing was applied.
      return std::unexpected(OutOfMemory(e));
    }
    window_slot.order = nullptr;
  }
  *response.mutable_record() = std::move(record);
  return response;
}

DataplaneTransactions::Records::node_type DataplaneTransactions::ReserveRecord(
    const std::string &request_id, uint64_t digest) {
  // The record of an applied transaction as kept: no operation result (they
  // are all the common one), no id (the key), every varint at its widest.
  v2::TransactionRecord largest;
  largest.set_outcome(v2::TransactionRecord::OUTCOME_APPLIED);
  largest.set_generation(std::numeric_limits<uint64_t>::max());
  largest.set_visibility(v2::TransactionRecord::VISIBILITY_SCOPE_SNAPSHOT);
  const utils::AdmittedGlibcAllocator<char> alloc(lease_.growth());
  Text bytes(alloc);
  bytes.reserve(largest.ByteSizeLong());
  // scale: at most kMaxRecords (4096) records; CommitRecord inserts
  // unallocated.
  records_.reserve(records_.size() + 1);
  Records::node_type node = records_.extract(
      records_
          .emplace(Text(request_id, alloc),
                   Recorded{.digest = digest,
                            .record = std::move(bytes),
                            .at = Positions(Positions::allocator_type(alloc))})
          .first);
  order_.push_back(nullptr);  // the window slot, filled by CommitRecord
  return node;
}

void DataplaneTransactions::CommitRecord(Records::node_type node,
                                         const v2::TransactionRecord &record) {
  Recorded &kept = node.mapped();
  kept.ops = static_cast<uint32_t>(record.ops_size());
  // The common result: the most frequent status among results with no error
  // (the engine's shapes: all APPLIED; one FAILED among NOT_APPLIED).
  std::array<int, 4> bare{};
  for (const auto &op : record.ops()) {
    if (op.error().empty() && op.status() >= 0 &&
        static_cast<size_t>(op.status()) < bare.size()) {
      bare[static_cast<size_t>(op.status())]++;
    }
  }
  kept.common = static_cast<v2::TransactionOpResult::Status>(
      std::max_element(bare.begin(), bare.end()) - bare.begin());
  v2::TransactionRecord compact;
  compact.set_outcome(record.outcome());
  compact.set_generation(record.generation());
  compact.set_visibility(record.visibility());
  for (int i = 0; i < record.ops_size(); i++) {
    const auto &op = record.ops(i);
    // What a resource returned is not the common shape: an operation that
    // carries bytes is kept even when its status is the common one, or the
    // replay would answer it with a bare status.
    if (op.status() != kept.common || !op.error().empty() ||
        !op.observed().empty()) {
      *compact.add_ops() = op;
      kept.at.push_back(static_cast<uint32_t>(i));
    }
  }
  kept.record.resize(compact.ByteSizeLong());
  // Sized by ByteSizeLong just above: it fits.
  static_cast<void>(compact.SerializeToArray(
      kept.record.data(), static_cast<int>(kept.record.size())));
  order_.back() = &records_.insert(std::move(node)).position->first;
  while (order_.size() > kMaxRecords ||
         (order_.size() > 1 && lease_.bytes() > kMaxRecordBytes)) {
    records_.erase(records_.find(std::string_view(*order_.front())));
    order_.pop_front();
  }
}

v2::TransactionRecord DataplaneTransactions::Expand(std::string_view request_id,
                                                    const Recorded &kept) {
  v2::TransactionRecord compact;
  ReadRecord(kept.record, &compact);
  v2::TransactionRecord record;
  record.set_request_id(request_id.data(), request_id.size());
  record.set_outcome(compact.outcome());
  record.set_generation(compact.generation());
  record.mutable_ops()->Reserve(static_cast<int>(kept.ops));
  size_t next = 0;
  for (uint32_t i = 0; i < kept.ops; i++) {
    if (next < kept.at.size() && kept.at[next] == i) {
      record.add_ops()->Swap(compact.mutable_ops(static_cast<int>(next++)));
    } else {
      record.add_ops()->set_status(kept.common);
    }
  }
  record.set_visibility(compact.visibility());
  return record;
}

v2::GetTransactionResponse DataplaneTransactions::Get(
    const std::string &request_id) const {
  v2::GetTransactionResponse response;
  response.set_daemon_epoch(epoch_);
  if (auto it = records_.find(request_id); it != records_.end()) {
    response.set_known(true);
    *response.mutable_record() = Expand(request_id, it->second);
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
