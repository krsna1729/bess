// SPDX-License-Identifier: BSD-3-Clause

// The RPCs bess::sdk::Client makes, behind an interface: the gRPC channel
// Client::Dial opens, or a test's scripted daemon. Every unary call returns
// its status and, when it failed, the server's trailing metadata (BESS puts
// its ErrorDetail in the "bess-error-bin" trailer). The default of each call
// is UNIMPLEMENTED, so a test overrides only what it scripts.

#ifndef BESS_SDK_TRANSPORT_H_
#define BESS_SDK_TRANSPORT_H_

#include <grpcpp/support/status.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "control_v2.pb.h"

namespace bess::sdk {

// The trailer that carries a failed call's ErrorDetail.
inline constexpr char kErrorDetailKey[] = "bess-error-bin";

using Trailers = std::multimap<std::string, std::string>;
using Deadline = std::chrono::system_clock::time_point;

// A server stream of events.
class EventStream {
 public:
  virtual ~EventStream() = default;
  // The next event; false when the stream ended (then Finish).
  virtual bool Read(::bess::pb::v2::Event *event) = 0;
  virtual grpc::Status Finish() = 0;
  // Ends a blocked Read from another thread.
  virtual void Cancel() = 0;
};

class Transport {
 public:
  virtual ~Transport() = default;

#define BESS_SDK_UNARY(Rpc)                                                 \
  virtual grpc::Status Rpc(Deadline, const ::bess::pb::v2::Rpc##Request &, \
                           ::bess::pb::v2::Rpc##Response *, Trailers *) {  \
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, #Rpc);             \
  }
  BESS_SDK_UNARY(ApplyTransaction)
  BESS_SDK_UNARY(GetTransaction)
  BESS_SDK_UNARY(ListTransactionResources)
  BESS_SDK_UNARY(GetCapabilities)
  BESS_SDK_UNARY(GetSchema)
  BESS_SDK_UNARY(ListMetrics)
  BESS_SDK_UNARY(GetPipeline)
  BESS_SDK_UNARY(ValidatePipeline)
  BESS_SDK_UNARY(DiffPipeline)
  BESS_SDK_UNARY(PlanPipeline)
  BESS_SDK_UNARY(ApplyPipeline)
  BESS_SDK_UNARY(RunModuleCommand)
  // The Admin service (D-107).
  BESS_SDK_UNARY(ImportPlugin)
  BESS_SDK_UNARY(UnloadPlugin)
  BESS_SDK_UNARY(GetPortStatus)
  BESS_SDK_UNARY(GetPacketPools)
  BESS_SDK_UNARY(SetCapture)
  BESS_SDK_UNARY(PauseWorkers)
  BESS_SDK_UNARY(ResumeWorkers)
  BESS_SDK_UNARY(Shutdown)
#undef BESS_SDK_UNARY

  // Opens an event stream; nullptr with `*status` set when it cannot.
  virtual std::unique_ptr<EventStream> WatchEvents(
      const ::bess::pb::v2::WatchEventsRequest &, grpc::Status *status) {
    *status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "WatchEvents");
    return nullptr;
  }
};

// A transport over a gRPC channel to `target` (no TLS, BESS's message limits).
std::shared_ptr<Transport> GrpcTransport(const std::string &target);

}  // namespace bess::sdk

#endif  // BESS_SDK_TRANSPORT_H_
