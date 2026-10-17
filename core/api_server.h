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
  // Owned: the builder and its plugins (gRPC's reflection one among them) are
  // freed with it, when the server goes out of scope in main.cc, before the
  // workers are stopped. That ownership is not verified by a leak check
  // anywhere: no in-process test uses ApiServer - each builds its own
  // grpc::ServerBuilder, so only main.cc constructs one - and the ASan lane
  // never reaches it: it skips the live suite (--no-suite live), its install
  // check returns early for sanitizer lanes, and live-perf runs only in the
  // gcc lane (the "Live performance, report only" step in ci.yml). CI's own
  // LSan works (tools/lsan.supp suppresses a real report from that lane,
  // D-142), so the gap is coverage, not the tool.
  std::unique_ptr<grpc::ServerBuilder> builder_;
  std::unique_ptr<bess::control::HttpServer> http_;
};

#endif  // BESS_API_SERVER_H_
