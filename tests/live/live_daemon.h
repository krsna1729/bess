// SPDX-License-Identifier: BSD-3-Clause

// Live tests: one bessd per test binary, started unprivileged (no DPDK, no
// hugepages, no root) on a free port, driven through the C++ SDK and the
// bessctl binary. Replaces bessctl/run_module_tests.py and
// BessModuleTestCase for what needs a running daemon; module behaviour is
// tested in process (core/testing/module_harness.h).
//
// meson passes the binaries' paths: BESSD, BESSCTL, BESS_SDK_CPP_CONFORMANCE,
// BESS_SOURCE_ROOT.

#ifndef BESS_TESTS_LIVE_LIVE_DAEMON_H_
#define BESS_TESTS_LIVE_LIVE_DAEMON_H_

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bess/sdk/client.h"

namespace bess::live {

struct Output {
  int status = -1;  // the exit status; 128 + N when killed by signal N
  std::string out;
  std::string err;
};

// Runs argv[0] (a path) with `input` on standard input; kills it after
// `timeout`.
Output RunProgram(const std::vector<std::string> &argv, std::string_view input = "",
                  std::chrono::seconds timeout = std::chrono::seconds(120));

// A program left running: its standard output read as it comes, then
// stopped (Ctrl-C ends `bessctl events` and `capture` cleanly).
class Child {
 public:
  explicit Child(const std::vector<std::string> &argv);
  ~Child();  // kills it if still running
  Child(const Child &) = delete;
  Child &operator=(const Child &) = delete;

  // Up to `n` bytes of standard output, waiting at most `timeout` for the
  // first; "" when nothing came (or it closed its output).
  std::string Read(size_t n, std::chrono::milliseconds timeout);
  // One line without its newline; nullopt when none came within `timeout`.
  std::optional<std::string> ReadLine(std::chrono::milliseconds timeout);
  // Sends `sig`, then waits for it to exit; its standard error and status.
  Output Stop(int sig, std::chrono::seconds timeout = std::chrono::seconds(10));

 private:
  int pid_ = -1;
  int out_ = -1;
  int err_ = -1;
  std::string buffered_;  // read past the last line returned
};

// $NAME; fails the test when unset.
std::string Env(const char *name);
// A path under the source tree.
std::string SourcePath(std::string_view relative);

class Daemon {
 public:
  // The binary's daemon, started on first use; stopped when the process
  // exits.
  static Daemon &Shared();
  ~Daemon();

  const std::string &address() const { return address_; }
  bess::sdk::Client &client() { return *client_; }
  // A new client (its own epoch knowledge, as a separate controller has).
  std::unique_ptr<bess::sdk::Client> NewClient() const;

  // Empty pipeline, workers paused: what each test starts from.
  void Reset();
  // bessctl -s ADDRESS args...
  Output Bessctl(const std::vector<std::string> &args, std::string_view input = "");
  // Still answering (and the process still running).
  bool Alive();
  // Starts it again after a test stopped it (a restart is a new epoch).
  void Restart();

 private:
  Daemon() = default;
  void Start();
  void Stop();

  int pid_ = -1;
  std::string address_;
  std::string log_path_;
  std::unique_ptr<bess::sdk::Client> client_;
};

// A test that starts from Daemon::Shared(), reset.
class LiveTest : public ::testing::Test {
 protected:
  void SetUp() override { Daemon::Shared().Reset(); }
  void TearDown() override {
    if (Daemon::Shared().Alive()) {
      Daemon::Shared().Reset();
    }
  }
  Daemon &daemon() { return Daemon::Shared(); }
  bess::sdk::Client &client() { return Daemon::Shared().client(); }
};

}  // namespace bess::live

#endif  // BESS_TESTS_LIVE_LIVE_DAEMON_H_
