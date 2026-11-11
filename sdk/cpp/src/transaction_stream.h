// SPDX-License-Identifier: BSD-3-Clause
#ifndef BESS_SDK_TRANSACTION_STREAM_H_
#define BESS_SDK_TRANSACTION_STREAM_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "bess/sdk/client.h"
#include "control_v2.grpc.pb.h"

namespace bess::sdk {

class TransactionStreamPool {
  class Lease {
   public:
    explicit Lease(pb::Control::Stub *stub)
        : stream(stub->PrepareAsyncTransactionChannel(&context, &queue)) {}
    ~Lease() {
      context.TryCancel();
      Drain();
      if (started && !finished) Finish();
      queue.Shutdown();
      void *tag;
      bool ok;
      while (queue.Next(&tag, &ok)) {}
    }
    grpc::Status Start(Deadline deadline) {
      if (started) return grpc::Status::OK;
      started = true;
      stream->StartCall(this);
      pending = 1;
      return Wait(deadline);
    }
    void Drain() {
      void *tag = nullptr;
      bool ok = false;
      while (pending != 0 && queue.Next(&tag, &ok)) --pending;
    }
    grpc::Status Wait(Deadline deadline) {
      void *tag = nullptr;
      bool ok = false;
      bool ended = false;
      while (pending != 0) {
        const auto result = queue.AsyncNext(&tag, &ok, deadline);
        if (result == grpc::CompletionQueue::TIMEOUT) {
          context.TryCancel();
          // Drain both directions before releasing any borrowed protobuf.
          Drain();
          Finish();
          return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                              "transaction attempt expired");
        }
        if (result != grpc::CompletionQueue::GOT_EVENT || tag != this) {
          if (result == grpc::CompletionQueue::GOT_EVENT) --pending;
          context.TryCancel();
          Drain();
          Finish();
          return grpc::Status(grpc::StatusCode::UNKNOWN, "transaction stream completion lost");
        }
        --pending;
        ended |= !ok;
      }
      if (ended) {
        auto status = Finish();
        return status.ok() ? grpc::Status(grpc::StatusCode::UNKNOWN,
                                         "transaction stream ended before acknowledgement") : status;
      }
      return grpc::Status::OK;
    }
    grpc::Status Finish() {
      if (!finished) {
        stream->Finish(&final_status, this);
        void *tag = nullptr;
        bool ok = false;
        if (!queue.Next(&tag, &ok) || tag != this || !ok) {
          final_status = grpc::Status(grpc::StatusCode::UNKNOWN, "transaction stream finish lost");
        }
        finished = true;
      }
      return final_status;
    }
    grpc::ClientContext context;
    grpc::CompletionQueue queue;
    std::unique_ptr<grpc::ClientAsyncReaderWriter<pb::TransactionChannelRequest,
                                                pb::TransactionChannelResponse>> stream;
    uint64_t sequence = 0;
    size_t pending = 0;
    bool started = false;
    bool finished = false;
    grpc::Status final_status;
  };

 public:
  TransactionStreamPool(pb::Control::Stub *stub, size_t limit)
      : stub_(stub), limit_(limit) { idle_.reserve(limit); }

  grpc::Status Apply(Deadline deadline, const pb::ApplyTransactionRequest &request,
                     pb::ApplyTransactionResponse *response, Trailers *trailers) {
    // Generated envelopes own submessages. This serialization-only borrow never
    // mutates request and always releases it before the envelope is destroyed.
    // The CQ write completes before either borrowed object can leave scope.
    struct Frame {
      pb::TransactionChannelRequest value;
      explicit Frame(const pb::ApplyTransactionRequest &request) {
        value.unsafe_arena_set_allocated_transaction(
            const_cast<pb::ApplyTransactionRequest *>(&request));
      }
      ~Frame() { (void)value.unsafe_arena_release_transaction(); }
    } frame(request);
    pb::TransactionChannelResponse reply;
    std::unique_ptr<Lease> lease;
    {
      std::unique_lock lock(mutex_);
      if (!available_.wait_until(lock, deadline, [&] { return active_ < limit_; })) {
        return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                            "transaction stream lease expired");
      }
      ++active_;
      if (!idle_.empty()) {
        lease = std::move(idle_.back());
        idle_.pop_back();
      }
    }
    struct Release {
      TransactionStreamPool &pool;
      std::unique_ptr<Lease> &lease;
      bool reusable = false;
      ~Release() {
        if (!reusable) lease.reset();  // cancellation/drain is outside pool lock
        std::lock_guard lock(pool.mutex_);
        if (lease) pool.idle_.push_back(std::move(lease));
        --pool.active_;
        pool.available_.notify_one();
      }
    } release{*this, lease};
    if (std::chrono::system_clock::now() >= deadline) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "transaction attempt expired");
    }
    if (!lease) lease = std::make_unique<Lease>(stub_);
    auto status = lease->Start(deadline);
    if (!status.ok()) {
      CopyTrailers(lease->context, trailers);
      return status;
    }
    frame.value.set_attempt_id(++lease->sequence);
    const auto budget = deadline - std::chrono::system_clock::now();
    if (budget <= decltype(budget)::zero()) {
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "transaction attempt expired");
    }
    frame.value.set_timeout_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(budget).count());
    lease->stream->Write(frame.value, lease.get());
    ++lease->pending;
    status = lease->Wait(deadline);
    if (!status.ok()) {
      CopyTrailers(lease->context, trailers);
      return status;
    }
    lease->stream->Read(&reply, lease.get());
    ++lease->pending;
    status = lease->Wait(deadline);
    if (!status.ok()) {
      CopyTrailers(lease->context, trailers);
      return status;
    }
    if (reply.attempt_id() != frame.value.attempt_id()) {
      return grpc::Status(grpc::StatusCode::UNKNOWN, "transaction acknowledgement does not match attempt");
    }
    if (reply.status_code() == grpc::StatusCode::OK) {
      if (!reply.has_transaction() || reply.has_error() ||
          !reply.transaction().has_record() ||
          reply.transaction().record().request_id() != request.request_id()) {
        return grpc::Status(grpc::StatusCode::UNKNOWN, "invalid transaction acknowledgement record");
      }
      *response = std::move(*reply.mutable_transaction());
      release.reusable = true;
      return grpc::Status::OK;
    }
    if (reply.status_code() < 0 || reply.status_code() > grpc::StatusCode::UNAUTHENTICATED ||
        !reply.has_error() || reply.has_transaction()) {
      return grpc::Status(grpc::StatusCode::UNKNOWN, "invalid transaction error acknowledgement");
    }
    trailers->emplace(kErrorDetailKey, reply.error().SerializeAsString());
    release.reusable = true;
    return grpc::Status(static_cast<grpc::StatusCode>(reply.status_code()), reply.status_message());
  }

 private:
  static void CopyTrailers(const grpc::ClientContext &context, Trailers *out) {
    for (const auto &[key, value] : context.GetServerTrailingMetadata()) {
      out->emplace(std::string(key.data(), key.size()), std::string(value.data(), value.size()));
    }
  }
  pb::Control::Stub *stub_;
  const size_t limit_;
  size_t active_ = 0;
  std::mutex mutex_;
  std::condition_variable available_;
  std::vector<std::unique_ptr<Lease>> idle_;
};

}  // namespace bess::sdk
#endif
