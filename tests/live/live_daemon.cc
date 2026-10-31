// SPDX-License-Identifier: BSD-3-Clause

#include "live_daemon.h"

#include <fcntl.h>
#include <glog/logging.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/json_util.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

extern char **environ;

namespace bess::live {
namespace {

int ExitStatus(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

std::vector<char *> Argv(const std::vector<std::string> &argv) {
  std::vector<char *> out;
  for (const std::string &a : argv) {
    out.push_back(const_cast<char *>(a.c_str()));
  }
  out.push_back(nullptr);
  return out;
}

std::string ReadFile(const std::string &path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

std::string Env(const char *name) {
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    ADD_FAILURE() << "$" << name << " is not set (meson sets it)";
    return "";
  }
  return v;
}

std::vector<int> AllowedCores() {
  cpu_set_t set;
  CPU_ZERO(&set);
  std::vector<int> cores;
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    for (int c = 0; c < CPU_SETSIZE; c++) {
      if (CPU_ISSET(c, &set)) {
        cores.push_back(c);
      }
    }
  }
  return cores;
}

bess::pb::v2::TaskPlacement OnWorker(const std::string &module, int wid) {
  bess::pb::v2::TaskPlacement t;
  t.set_module(module);
  t.set_wid(wid);
  return t;
}

int FreePort() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(addr);
  if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ADD_FAILURE() << "no free port: " << std::strerror(errno);
  }
  close(fd);
  return ntohs(addr.sin_port);
}

std::string SourcePath(std::string_view relative) {
  return Env("BESS_SOURCE_ROOT") + "/" + std::string(relative);
}

Output RunProgram(const std::vector<std::string> &argv, std::string_view input,
                  std::chrono::seconds timeout) {
  return RunProgramIn("", argv, input, timeout);
}

Output RunProgramIn(const std::string &dir, const std::vector<std::string> &argv,
                    std::string_view input, std::chrono::seconds timeout) {
  Output result;
  int in[2], out[2], err[2];
  if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
    ADD_FAILURE() << "pipe: " << std::strerror(errno);
    return result;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err[1], 2);
  if (!dir.empty()) {
    posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
  }
  pid_t pid = -1;
  auto args = Argv(argv);
  const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(in[0]);
  close(out[1]);
  close(err[1]);
  if (rc != 0) {
    close(in[1]);
    close(out[0]);
    close(err[0]);
    ADD_FAILURE() << "cannot run " << argv[0] << ": " << std::strerror(rc);
    return result;
  }
  // Small inputs: written whole before reading (no deadlock below the pipe
  // buffer, 64 KiB).
  if (!input.empty() && write(in[1], input.data(), input.size()) < 0) {
    ADD_FAILURE() << "writing to " << argv[0] << ": " << std::strerror(errno);
  }
  close(in[1]);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
  std::string *sinks[2] = {&result.out, &result.err};
  int open_fds = 2;
  bool killed = false;
  while (open_fds > 0) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0 && !killed) {
      kill(pid, SIGKILL);
      killed = true;
      ADD_FAILURE() << argv[0] << " ran past " << timeout.count() << " s; killed";
    }
    if (poll(fds, 2, killed ? 1000 : static_cast<int>(std::min<int64_t>(left.count(), 1000))) <
        0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    for (int i = 0; i < 2; i++) {
      if (fds[i].fd >= 0 && (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
        char buf[4096];
        const ssize_t n = read(fds[i].fd, buf, sizeof(buf));
        if (n > 0) {
          sinks[i]->append(buf, static_cast<size_t>(n));
        } else {
          close(fds[i].fd);
          fds[i].fd = -1;
          open_fds--;
        }
      }
    }
  }
  int status = 0;
  waitpid(pid, &status, 0);
  result.status = ExitStatus(status);
  return result;
}

Child::Child(const std::vector<std::string> &argv) {
  int out[2], err[2];
  if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
    ADD_FAILURE() << "pipe: " << std::strerror(errno);
    return;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err[1], 2);
  auto args = Argv(argv);
  pid_t pid = -1;
  const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(out[1]);
  close(err[1]);
  out_ = out[0];
  err_ = err[0];
  if (rc != 0) {
    ADD_FAILURE() << "cannot run " << argv[0] << ": " << std::strerror(rc);
    return;
  }
  pid_ = pid;
}

Child::~Child() {
  if (pid_ > 0) {
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
  }
  if (out_ >= 0) close(out_);
  if (err_ >= 0) close(err_);
}

std::string Child::Read(size_t n, std::chrono::milliseconds timeout) {
  if (buffered_.empty()) {
    pollfd p = {out_, POLLIN, 0};
    if (out_ < 0 || poll(&p, 1, static_cast<int>(timeout.count())) <= 0) {
      return "";
    }
    char buf[4096];
    const ssize_t got = read(out_, buf, sizeof(buf));
    if (got <= 0) {
      return "";
    }
    buffered_.assign(buf, static_cast<size_t>(got));
  }
  std::string out = buffered_.substr(0, n);
  buffered_.erase(0, out.size());
  return out;
}

std::optional<std::string> Child::ReadLine(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (const size_t nl = buffered_.find('\n'); nl != std::string::npos) {
      std::string line = buffered_.substr(0, nl);
      buffered_.erase(0, nl + 1);
      return line;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    pollfd p = {out_, POLLIN, 0};
    if (left.count() <= 0 || poll(&p, 1, static_cast<int>(left.count())) <= 0) {
      return std::nullopt;
    }
    char buf[4096];
    const ssize_t got = read(out_, buf, sizeof(buf));
    if (got <= 0) {
      return std::nullopt;
    }
    buffered_.append(buf, static_cast<size_t>(got));
  }
}

Output Child::Stop(int sig, std::chrono::seconds timeout) {
  Output result;
  if (pid_ <= 0) {
    return result;
  }
  kill(pid_, sig);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (waitpid(pid_, &status, WNOHANG) == 0) {
    if (std::chrono::steady_clock::now() > deadline) {
      ADD_FAILURE() << "did not exit within " << timeout.count() << " s of signal " << sig;
      kill(pid_, SIGKILL);
      waitpid(pid_, &status, 0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  pid_ = -1;
  result.status = ExitStatus(status);
  char buf[4096];
  ssize_t got;
  while ((got = read(err_, buf, sizeof(buf))) > 0) {
    result.err.append(buf, static_cast<size_t>(got));
  }
  result.out = buffered_;
  while ((got = read(out_, buf, sizeof(buf))) > 0) {
    result.out.append(buf, static_cast<size_t>(got));
  }
  return result;
}

Types::Types(bess::sdk::Client &client) {
  auto schema = client.Schema();
  if (!schema) {
    ADD_FAILURE() << "schema: " << schema.error().message;
    return;
  }
  schema_ = std::move(*schema);
  for (const google::protobuf::FileDescriptorProto &file : schema_.descriptors().file()) {
    if (pool_.BuildFile(file) == nullptr) {
      ADD_FAILURE() << "schema: cannot build " << file.name();
    }
  }
}

std::unique_ptr<google::protobuf::Message> Types::Make(const std::string &type,
                                                       const std::string &json) {
  const google::protobuf::Descriptor *d = pool_.FindMessageTypeByName(type);
  if (d == nullptr) {
    ADD_FAILURE() << "no type " << type << " in the daemon's schema";
    return nullptr;
  }
  std::unique_ptr<google::protobuf::Message> m(factory_.GetPrototype(d)->New());
  const auto status = google::protobuf::util::JsonStringToMessage(json, m.get());
  EXPECT_TRUE(status.ok()) << type << " from " << json << ": " << status.message();
  return m;
}

std::string Types::ToJson(const google::protobuf::Any &any) {
  const std::string type = any.type_url().substr(any.type_url().rfind('/') + 1);
  const google::protobuf::Descriptor *d = pool_.FindMessageTypeByName(type);
  if (d == nullptr) {
    ADD_FAILURE() << "no type " << type << " in the daemon's schema";
    return "";
  }
  std::unique_ptr<google::protobuf::Message> m(factory_.GetPrototype(d)->New());
  EXPECT_TRUE(m->ParseFromString(any.value()));
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  std::string text;
  EXPECT_TRUE(google::protobuf::util::MessageToJsonString(*m, &text, options).ok());
  return text;
}

Daemon &Daemon::Shared() {
  static Daemon *daemon = [] {
    auto *d = new Daemon();
    d->Start();
    // Stopped at exit, after every test (a leaked daemon outlives the run).
    std::atexit([] { Shared().Stop(); });
    return d;
  }();
  return *daemon;
}

Daemon::~Daemon() { Stop(); }

void Daemon::Start(int port) {
  if (port == 0) {
    port = FreePort();
  }
  address_ = "localhost:" + std::to_string(port);
  char log_template[] = "/tmp/bessd-live-XXXXXX";
  const int log_fd = mkstemp(log_template);
  log_path_ = log_template;
  keep_log_ = false;  // a fresh daemon: this run's log is this run's to remove
  std::vector<std::string> argv = launcher_;
  argv.insert(argv.end(), {Env("BESSD"), "-f", "--mem_backing=normal", "-p",
                           std::to_string(port), "-skip_root_check"});
  argv.insert(argv.end(), flags_.begin(), flags_.end());
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, log_fd, 1);
  posix_spawn_file_actions_adddup2(&actions, log_fd, 2);
  auto args = Argv(argv);
  pid_t pid = -1;
  const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(log_fd);
  if (rc != 0) {
    ADD_FAILURE() << "cannot start " << argv[0] << ": " << std::strerror(rc);
    return;
  }
  pid_ = pid;
  client_ = bess::sdk::Client::Dial(address_);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (client_->Capabilities()) {
      return;
    }
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
      ADD_FAILURE() << "bessd exited (" << ExitStatus(status) << ") at startup:\n"
                    << ReadFile(log_path_);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ADD_FAILURE() << "bessd did not answer within 30 s:\n" << ReadFile(log_path_);
}

int Daemon::Shutdown() {
  if (pid_ <= 0) {
    return -1;
  }
  (void)client_->Shutdown();  // it answers, then stops
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int status = 0;
  while (waitpid(pid_, &status, WNOHANG) == 0) {
    if (std::chrono::steady_clock::now() > deadline) {
      // A daemon that will not stop when asked is the interesting case: it is
      // stuck after "BESS daemon has been gracefully shut down", and its log
      // is the only record of where. Its SIGABRT handler (core/debug.cc)
      // prints a full stack trace, and it is being killed anyway; SIGKILL is
      // the fallback for a process too wedged to take the signal.
      //
      // SIGUSR1 would be the non-fatal way to ask for the same dump, but it
      // produced nothing in the runs where this fired (2026-10-16).
      if (kill(pid_, SIGABRT) != 0) {
        LOG(ERROR) << "SIGABRT to bessd " << pid_ << ": " << std::strerror(errno);
      }
      // TrapHandler (core/debug.cc) builds the trace with one addr2line process
      // per frame, up to 64 frames, and GoPanic writes it in one LOG(FATAL) at
      // the end: the log is quiet for the whole symbolization, and a line some
      // other thread logs must not count as the trace and re-arm a quiet timer.
      // Wait for the process, or 2 s after the log actually holds the trace, up
      // to 20 s. (A process-directed SIGABRT runs the handler on an arbitrary
      // thread, so the trace may be another thread's -- enough to see where the
      // process was, not proof of where the request is stuck.)
      const auto abort_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(20);
      size_t logged = ReadFile(log_path_).size();
      auto quiet_since = std::chrono::steady_clock::now();
      while (waitpid(pid_, &status, WNOHANG) == 0) {
        const auto now = std::chrono::steady_clock::now();
        const std::string log = ReadFile(log_path_);
        if (log.size() != logged) {
          logged = log.size();
          quiet_since = now;
        }
        const bool wrote_trace =
            log.find("A critical error has occured") != std::string::npos ||
            log.find("Backtrace (recent calls first)") != std::string::npos;
        if ((wrote_trace && now - quiet_since > std::chrono::seconds(2)) ||
            now > abort_deadline) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      ADD_FAILURE() << "bessd does not stop on request; killing it. Its log:\n"
                    << ReadFile(log_path_);
      keep_log_ = true;
      if (waitpid(pid_, &status, WNOHANG) == 0) {
        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  pid_ = -1;
  return status;
}

void Daemon::Stop() {
  (void)Shutdown();
  if (!log_path_.empty()) {
    if (keep_log_) {
      LOG(INFO) << "bessd's log is kept at " << log_path_;
      log_path_.clear();
    } else {
      unlink(log_path_.c_str());
      log_path_.clear();
    }
  }
}

std::unique_ptr<Daemon> Daemon::Launch(std::vector<std::string> flags) {
  std::unique_ptr<Daemon> d(new Daemon(std::move(flags)));
  d->Start();
  return d;
}

std::unique_ptr<Daemon> Daemon::LaunchUnder(std::vector<std::string> launcher,
                                            std::vector<std::string> flags) {
  std::unique_ptr<Daemon> d(new Daemon(std::move(flags)));
  d->launcher_ = std::move(launcher);
  d->Start();
  return d;
}

std::string Daemon::Log() const { return ReadFile(log_path_); }

void Daemon::Restart() {
  Stop();
  Start();
}

void Daemon::RestartInPlace() {
  const int port = std::stoi(address_.substr(address_.rfind(':') + 1));
  Stop();
  Start(port);
}

std::unique_ptr<bess::sdk::Client> Daemon::NewClient() const {
  return bess::sdk::Client::Dial(address_);
}

void Daemon::Reset() {
  // An earlier test may have restarted the daemon: the first answer from
  // the new epoch teaches the client, and the apply is sent again.
  for (int attempt = 0; attempt < 2; attempt++) {
    auto applied = client_->ApplyPipeline(bess::pb::v2::Pipeline());
    if (applied) {
      return;
    }
    if (applied.error().kind != bess::sdk::ErrorKind::kDaemonRestarted) {
      ADD_FAILURE() << "reset: " << applied.error().message;
      return;
    }
  }
  ADD_FAILURE() << "reset: refused twice as from another epoch";
}

Output Daemon::Bessctl(const std::vector<std::string> &args, std::string_view input) {
  std::vector<std::string> argv = {Env("BESSCTL"), "-s", address_};
  argv.insert(argv.end(), args.begin(), args.end());
  return RunProgram(argv, input);
}

bool Daemon::Alive() {
  if (pid_ <= 0) {
    return false;
  }
  int status = 0;
  if (waitpid(pid_, &status, WNOHANG) == pid_) {
    ADD_FAILURE() << "bessd died (" << ExitStatus(status) << "):\n" << ReadFile(log_path_);
    pid_ = -1;
    return false;
  }
  return client_->Capabilities().has_value();
}

}  // namespace bess::live
