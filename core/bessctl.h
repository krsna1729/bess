// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_BESSCTL_H_
#define BESS_BESSCTL_H_

#include <string>

namespace grpc {
class ServerBuilder;
}  // namespace grpc

// gRPC server encapsulation. Usage:
//   ApiServer server;
//   server.Listen('0.0.0.0:777');
//   server.Listen('127.0.0.1:888');
//   server.run();
class ApiServer {
 public:
  ApiServer() : builder_(nullptr) {}

  // This class is neither copyable nor movable.
  ApiServer(const ApiServer &) = delete;
  ApiServer &operator=(const ApiServer &) = delete;

  // `addr` is a gRPC url.
  void Listen(const std::string &addr);

  // Runs the API server until it is shutdown by KillBess RPC.
  void Run();

 private:
  grpc::ServerBuilder *builder_;
};

#endif  // BESS_BESSCTL_H_
