// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_BESSCTL_H_
#define BESS_BESSCTL_H_

#include <memory>
#include <string>

namespace grpc {
class ServerBuilder;
class Service;
}  // namespace grpc

namespace bess::control {
class ControlPlane;
class HttpServer;
}  // namespace bess::control

// The legacy BESSControl service over `control_plane`, as ApiServer::Run
// serves it beside v2: for tests that run it on an in-process channel.
std::unique_ptr<grpc::Service> NewLegacyControlService(
    bess::control::ControlPlane &control_plane);

// gRPC server encapsulation. Usage:
//   ApiServer server;
//   server.Listen('0.0.0.0:777');
//   server.Listen('127.0.0.1:888');
//   server.run();
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

  // Runs the API server until it is shutdown by KillBess RPC.
  void Run();

 private:
  grpc::ServerBuilder *builder_;
  std::unique_ptr<bess::control::HttpServer> http_;
};

#endif  // BESS_BESSCTL_H_
