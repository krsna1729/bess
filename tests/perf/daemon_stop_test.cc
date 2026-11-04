// SPDX-License-Identifier: BSD-3-Clause

// live_perf's daemon stop (daemon_stop.h), against a stand-in for a bessd
// copy named for its role: a script called bessd.parent that writes its pid
// to -i's file, as bessd does, and runs until it is signalled, under a
// launcher that waits for it and exits with its status, as sudo does.

#include "daemon_stop.h"

#include <gtest/gtest.h>
#include <spawn.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

extern char **environ;

namespace {

namespace fs = std::filesystem;

// Run as a script, its command line is "/bin/sh DIR/bessd.parent -f -p PORT
// -i PIDFILE", which no `pkill -f "bessd .* -p PORT "` matches. "ready" on
// standard output once the pid is written and SIGTERM is handled.
constexpr const char *kStandIn = R"(#!/bin/sh
while [ $# -gt 1 ]; do
  if [ "$1" = -i ]; then echo $$ > "$2"; fi
  shift
done
trap 'kill $!; wait $!; exit 143' TERM
sleep 1000 &
echo ready
wait
)";

TEST(DaemonStopTest, StopsACopyNamedForItsRoleByThePidItWrote) {
  std::string tmpl = (fs::temp_directory_path() / "daemon_stop.XXXXXX").string();
  ASSERT_NE(nullptr, mkdtemp(tmpl.data()));
  const fs::path dir = tmpl;
  const std::string bin = (dir / "bessd.parent").string();
  const std::string pidfile = (dir / "bessd.pid").string();
  std::ofstream(bin) << kStandIn;
  fs::permissions(bin, fs::perms::owner_all);

  int out[2];
  ASSERT_EQ(0, pipe(out));
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, out[0]);
  posix_spawn_file_actions_addclose(&actions, out[1]);
  std::vector<std::string> argv = {"/bin/sh", "-c", "\"$@\"; exit $?", "launcher", bin,
                                   "-f", "-p", "1", "-i", pidfile};
  std::vector<char *> args;
  args.reserve(argv.size() + 1);
  for (std::string &a : argv) {
    args.push_back(a.data());
  }
  args.push_back(nullptr);
  pid_t launcher = -1;
  const int spawned = posix_spawn(&launcher, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(out[1]);
  ASSERT_EQ(0, spawned);
  char ready[6] = {};
  ASSERT_EQ(6, read(out[0], ready, sizeof(ready)));
  close(out[0]);
  ASSERT_EQ("ready\n", std::string(ready, sizeof(ready)));
  const pid_t daemon = bess::perf::PidfilePid(pidfile);
  ASSERT_GT(daemon, 0);
  ASSERT_NE(launcher, daemon);  // a launcher of its own, as sudo is

  std::vector<std::pair<pid_t, int>> sent;
  const int status = bess::perf::StopDaemon(launcher, pidfile, [&](pid_t pid, int sig) {
    sent.emplace_back(pid, sig);
    kill(pid, sig);
  });

  // One SIGTERM, to the daemon; it exits on it, and the launcher with it.
  ASSERT_EQ(1U, sent.size());
  EXPECT_EQ(daemon, sent[0].first);
  EXPECT_EQ(SIGTERM, sent[0].second);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(143, WEXITSTATUS(status));
  const int alive = kill(daemon, 0);
  const int error = errno;
  EXPECT_EQ(-1, alive);
  EXPECT_EQ(ESRCH, error);
  fs::remove_all(dir);
}

}  // namespace
