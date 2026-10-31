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

#include <glog/logging.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <gtest/gtest.h>
#include <sched.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
// RunProgram in the working directory `dir`.
Output RunProgramIn(const std::string &dir, const std::vector<std::string> &argv,
                    std::string_view input = "",
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

// The daemon's types (GetSchema's descriptors), to build arguments, keys and
// values from protobuf JSON and read answers, as a client without generated
// code does.
class Types {
 public:
  explicit Types(bess::sdk::Client &client);
  // A message of `type` (full name) from JSON; fails the test on a bad one.
  std::unique_ptr<google::protobuf::Message> Make(const std::string &type,
                                                  const std::string &json);
  // An Any (a command's answer) as JSON, field names as in the .proto files.
  std::string ToJson(const google::protobuf::Any &any);
  const bess::pb::v2::GetSchemaResponse &schema() const { return schema_; }

 private:
  bess::pb::v2::GetSchemaResponse schema_;
  google::protobuf::DescriptorPool pool_;
  google::protobuf::DynamicMessageFactory factory_{&pool_};
};

// $NAME; fails the test when unset.
std::string Env(const char *name);
// A path under the source tree.
std::string SourcePath(std::string_view relative);
// A TCP port on loopback nothing listens on now.
int FreePort();

// The CPUs this process (and any daemon it started) may run on: a worker's
// core must be one of them.
std::vector<int> AllowedCores();

// A task placement on worker `wid` with no parent: desired state names the
// module and the worker, and the module's own leaf class follows.
bess::pb::v2::TaskPlacement OnWorker(const std::string &module, int wid);

class Daemon {
 public:
  // The binary's daemon, started on first use; stopped when the process
  // exits.
  static Daemon &Shared();
  ~Daemon();

  int pid() const { return pid_; }

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
  // Restart on the same address: a client's channel reaches the new daemon,
  // as after a restart in place (bessd -k).
  void RestartInPlace();

  // A daemon of its own, started with `flags` after the usual ones (Shared()
  // takes none). Stopped when destroyed, unless Shutdown() ran.
  static std::unique_ptr<Daemon> Launch(std::vector<std::string> flags);
  // As Launch, the daemon started by `launcher` (an argv that execs the
  // rest of its arguments in place, as `systemd-run --scope` does, so the
  // daemon keeps the pid it is waited on by).
  static std::unique_ptr<Daemon> LaunchUnder(std::vector<std::string> launcher,
                                             std::vector<std::string> flags);
  // What it has logged so far (standard output and error).
  std::string Log() const;
  // Asks it to shut down and waits (SIGKILL after 10 s): waitpid's raw
  // status, so a caller can tell a signal (WIFSIGNALED, WTERMSIG) from an
  // exit; -1 when it was not running.
  int Shutdown();

 private:
  Daemon() = default;
  explicit Daemon(std::vector<std::string> flags) : flags_(std::move(flags)) {}
  void Start(int port = 0);  // 0: a free port
  void Stop();

  std::vector<std::string> launcher_;
  std::vector<std::string> flags_;
  int pid_ = -1;
  std::string address_;
  std::string log_path_;
  // Shutdown() had to SIGKILL it: keep the log on disk and say where it is
  // (a daemon stuck in a request is the case worth a record).
  bool keep_log_ = false;
  std::unique_ptr<bess::sdk::Client> client_;
};

// A test that starts from Daemon::Shared(), reset.
class LiveTest : public ::testing::Test {
 protected:
  void SetUp() override { Daemon::Shared().Reset(); }
  void TearDown() override {
    // A daemon that died or stopped answering leaves no trace in a bare
    // ASSERT_TRUE, so say so and print its log: the failure then names the
    // cause instead of pointing at whichever call noticed it. A live daemon
    // still has context worth printing when a test failed.
    if (!Daemon::Shared().Alive()) {
      ADD_FAILURE() << "bessd is not answering; log:\n"
                    << Daemon::Shared().Log();
    } else if (::testing::Test::HasFailure()) {
      LOG(INFO) << "bessd log:\n" << Daemon::Shared().Log();
    }
    if (Daemon::Shared().Alive()) {
      Daemon::Shared().Reset();
    }
  }
  Daemon &daemon() { return Daemon::Shared(); }
  bess::sdk::Client &client() { return Daemon::Shared().client(); }
};

}  // namespace bess::live

#endif  // BESS_TESTS_LIVE_LIVE_DAEMON_H_
