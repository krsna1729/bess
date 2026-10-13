// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_HTTP_SERVER_H_
#define BESS_CONTROL_HTTP_SERVER_H_

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace grpc {
class Channel;
}  // namespace grpc

namespace bess::control {

// bessd's HTTP adapter (D-104, D-125): off unless --http_addr names an
// address. Every request is a call on bessd's own gRPC server through an
// in-process channel, so HTTP and gRPC share one code path, one
// authorization point and the daemon_epoch rules:
//
//   GET  /             a static page (compiled in) over the JSON API
//   GET  /metrics      ListMetrics in the Prometheus text format (0.0.4)
//   POST /v2/<Method>  a bess.pb.v2.Control unary RPC: the body is the
//                      request in protobuf JSON, the answer the response
//   GET  /v2/events    WatchEvents as server-sent events, one JSON Event per
//                      message; ?from_sequence=&daemon_epoch=&types=a,b, or
//                      the Last-Event-ID a browser sends on reconnecting
//
// A failed call answers the gRPC code's HTTP status with
// {"code": <grpc code>, "status": "<NAME>", "message": ..., "detail":
// <ErrorDetail>}; a body that is not the request's JSON is 400. One request
// per connection (Connection: close); each connection is served on its own
// thread, none of them a worker.
class HttpServer {
 public:
  HttpServer() = default;
  ~HttpServer();  // Stop()
  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  // Binds and listens on `address`, "HOST:PORT" (an IPv6 host in brackets;
  // port 0: any free one). The host is required: serving beyond loopback
  // is spelled out (0.0.0.0, a routable address) and logged as a warning,
  // since anyone who reaches the port controls the daemon. False, with
  // `error` set, if the address is malformed or cannot be bound.
  bool Bind(const std::string &address, std::string *error);
  // The bound address ("127.0.0.1:8080"); valid after Bind.
  const std::string &address() const { return address_; }
  uint16_t port() const { return port_; }

  // Serves requests through `channel` (bessd's gRPC server, in process) on
  // a thread of its own, until Stop.
  void Start(std::shared_ptr<grpc::Channel> channel);
  // Stops accepting, ends open connections (event streams included) and
  // joins every thread. Idempotent.
  void Stop();

 private:
  struct Connection {
    int fd = -1;
    std::thread thread;
    std::atomic<bool> done{false};
  };

  void AcceptLoop();
  void Serve(Connection *conn);
  void ReapLocked();

  std::shared_ptr<grpc::Channel> channel_;
  std::string address_;
  uint16_t port_ = 0;
  int listen_fd_ = -1;
  int wake_fd_ = -1;  // eventfd: Stop wakes the accept loop
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  std::mutex mu_;
  std::list<std::unique_ptr<Connection>> connections_;  // guarded by mu_
};

}  // namespace bess::control

#endif  // BESS_CONTROL_HTTP_SERVER_H_
