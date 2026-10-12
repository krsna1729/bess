// SPDX-License-Identifier: BSD-3-Clause

#include <grpcpp/grpcpp.h>

#include <memory>
#include <mutex>
#include <string>

#include "bess/sdk/client.h"
#include "bess/sdk/transport.h"
#include "control_v2.grpc.pb.h"

namespace bess::sdk {
namespace {

void CopyTrailers(const grpc::ClientContext &ctx, Trailers *out) {
  for (const auto &[k, v] : ctx.GetServerTrailingMetadata()) {
    out->emplace(std::string(k.data(), k.size()), std::string(v.data(), v.size()));
  }
}

class GrpcEventStream final : public EventStream {
 public:
  GrpcEventStream(pb::Control::Stub *stub, const pb::WatchEventsRequest &req)
      : reader_(stub->WatchEvents(&ctx_, req)) {}

  bool Read(pb::Event *event) override { return reader_->Read(event); }
  grpc::Status Finish() override { return reader_->Finish(); }
  void Cancel() override { ctx_.TryCancel(); }

 private:
  grpc::ClientContext ctx_;  // before reader_: the reader uses it
  std::unique_ptr<grpc::ClientReader<pb::Event>> reader_;
};

class GrpcTransportImpl final : public Transport {
 public:
  explicit GrpcTransportImpl(std::shared_ptr<grpc::Channel> channel)
      : stub_(pb::Control::NewStub(channel)) {}

#define BESS_SDK_GRPC_UNARY(Rpc)                                          \
  grpc::Status Rpc(Deadline deadline, const pb::Rpc##Request &req,        \
                   pb::Rpc##Response *resp, Trailers *trailers) override { \
    grpc::ClientContext ctx;                                              \
    ctx.set_deadline(deadline);                                           \
    grpc::Status status = stub_->Rpc(&ctx, req, resp);                    \
    if (!status.ok()) {                                                   \
      CopyTrailers(ctx, trailers);                                        \
    }                                                                     \
    return status;                                                        \
  }
  BESS_SDK_GRPC_UNARY(ApplyTransaction)
  BESS_SDK_GRPC_UNARY(GetTransaction)
  BESS_SDK_GRPC_UNARY(ListTransactionResources)
  BESS_SDK_GRPC_UNARY(GetCapabilities)
  BESS_SDK_GRPC_UNARY(GetSchema)
  BESS_SDK_GRPC_UNARY(ListMetrics)
  BESS_SDK_GRPC_UNARY(GetPipeline)
  BESS_SDK_GRPC_UNARY(ValidatePipeline)
  BESS_SDK_GRPC_UNARY(DiffPipeline)
  BESS_SDK_GRPC_UNARY(PlanPipeline)
  BESS_SDK_GRPC_UNARY(ApplyPipeline)
#undef BESS_SDK_GRPC_UNARY

  std::unique_ptr<EventStream> WatchEvents(const pb::WatchEventsRequest &req,
                                           grpc::Status *status) override {
    *status = grpc::Status::OK;
    return std::make_unique<GrpcEventStream>(stub_.get(), req);
  }

 private:
  std::unique_ptr<pb::Control::Stub> stub_;
};

}  // namespace

std::shared_ptr<Transport> GrpcTransport(const std::string &target) {
  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(kMaxMessageBytes);
  args.SetMaxSendMessageSize(kMaxMessageBytes);
  return std::make_shared<GrpcTransportImpl>(grpc::CreateCustomChannel(
      target, grpc::InsecureChannelCredentials(), args));
}

std::unique_ptr<Client> Client::Dial(const std::string &target,
                                     RetryPolicy retry) {
  return std::make_unique<Client>(GrpcTransport(target), retry);
}

}  // namespace bess::sdk
