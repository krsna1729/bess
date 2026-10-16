// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_API_SERVER_H_
#define BESS_API_SERVER_H_

#include <memory>
#include <string>

namespace grpc {
class ServerBuilder;
}  // namespace grpc

namespace bess::control {
class HttpServer;
}  // namespace bess::control

// bessd's control endpoint: the control_v2 services (Control and Admin) over
// gRPC, and over HTTP when one is given.
class ApiServer {
 public:
  ApiServer();
  ~ApiServer();

  // This class is neither copyable nor movable.
  ApiServer(const ApiServer &) = delete;
  ApiServer &operator=(const ApiServer &) = delete;

  // `addr` is a gRPC url.
  void Listen(const std::string &addr);

  // Run also serves HTTP on `http` (bound already), through an in-process
  // channel to this gRPC server (D-125); stopped when Run ends.
  void ServeHttp(std::unique_ptr<bess::control::HttpServer> http);

  // Runs the API server until Admin.Shutdown or SIGTERM/SIGINT.
  void Run();

 private:
  // Owned: its plugins (gRPC's reflection one among them) are freed with it,
  // and LSan sees nothing unreachable at exit (the ASan lane runs the plugin
  // tests with detect_leaks=1).
  std::unique_ptr<grpc::ServerBuilder> builder_;
  std::unique_ptr<bess::control::HttpServer> http_;
};

#endif  // BESS_API_SERVER_H_
