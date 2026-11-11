// SPDX-License-Identifier: BSD-3-Clause
#include <chrono>
#include <string>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "src/transaction_stream.h"

namespace {
namespace sdk = bess::sdk;
namespace v2 = bess::pb::v2;

// An adversarial peer exercises the real codec/stream boundary. These are not
// successful echoed acknowledgements: neither body can identify the sent work.
class MalformedAckPeer final : public v2::Control::Service {
 public:
  explicit MalformedAckPeer(bool missing_record) : missing_record_(missing_record) {}
  grpc::Status TransactionChannel(
      grpc::ServerContext *,
      grpc::ServerReaderWriter<v2::TransactionChannelResponse,
                               v2::TransactionChannelRequest> *stream) override {
    v2::TransactionChannelRequest frame;
    if (!stream->Read(&frame)) return grpc::Status::OK;
    v2::TransactionChannelResponse reply;
    reply.set_attempt_id(frame.attempt_id());
    auto *transaction = reply.mutable_transaction();
    transaction->set_daemon_epoch(frame.transaction().daemon_epoch());
    if (!missing_record_) {
      transaction->mutable_record()->set_request_id("a-different-request");
      transaction->mutable_record()->set_outcome(v2::TransactionRecord::OUTCOME_APPLIED);
    }
    stream->Write(reply);
    return grpc::Status::OK;
  }
 private:
  const bool missing_record_;
};

TEST(SdkTransactionStream, InvalidRecordIsUnknownNotCachedSuccess) {
  for (const bool missing_record : {true, false}) {
    MalformedAckPeer peer(missing_record);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&peer);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto stub = v2::Control::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    {
      sdk::TransactionStreamPool pool(stub.get(), 1);
      v2::ApplyTransactionRequest request;
      request.set_daemon_epoch(1);
      request.set_request_id("the-original-request");
      v2::ApplyTransactionResponse response;
      sdk::Trailers trailers;
      auto result = pool.Apply(std::chrono::system_clock::now() + std::chrono::seconds(5),
                               request, &response, &trailers);
      EXPECT_EQ(result.error_code(), grpc::StatusCode::UNKNOWN);
      EXPECT_FALSE(response.has_record());
    }
    server->Shutdown();
  }
}
}  // namespace
