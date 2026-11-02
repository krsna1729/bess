// SPDX-License-Identifier: BSD-3-Clause

// How live_perf stops the bessd it started for a round.
//
// A round's bessd runs under a launcher (`sudo -n env ... bessd -i PIDFILE`)
// that waits for it and exits with it, and bessd writes its own pid to
// PIDFILE at startup. The daemon is stopped by that pid, never by its name: a
// build under test is often a copy named for its role (bessd.parent,
// bessd.fix), and the `pkill -f "bessd .* -p PORT "` this replaced matched no
// such copy. The copy kept running, so the wait for its launcher never
// returned: the run printed its placement line and nothing after it.

#ifndef BESS_TESTS_PERF_DAEMON_STOP_H_
#define BESS_TESTS_PERF_DAEMON_STOP_H_

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace bess::perf {

// The pid in `pidfile`, or 0 when it holds none.
inline pid_t PidfilePid(const std::string &pidfile) {
  std::ifstream in(pidfile);
  long pid = 0;
  return (in >> pid) && pid > 0 ? static_cast<pid_t>(pid) : 0;
}

// Sends signal `sig` to `pid` with the daemon's privileges (live_perf:
// `sudo -n kill`).
using SignalFn = std::function<void(pid_t pid, int sig)>;

// Stops the daemon `launcher` runs, then reaps the launcher and returns its
// wait status: SIGTERM to the pid in `pidfile`, SIGKILL after `grace` (a
// bessd stuck in shutdown must not hang the run). A daemon that never wrote
// its pid leaves nothing to signal but the launcher, which then gets the
// signals, so the wait ends either way.
inline int StopDaemon(pid_t launcher, const std::string &pidfile, const SignalFn &signal,
                      std::chrono::milliseconds grace = std::chrono::seconds(10)) {
  constexpr std::chrono::milliseconds kPoll{100};
  int status = 0;
  const auto exited = [&] { return waitpid(launcher, &status, WNOHANG) == launcher; };
  for (const int sig : {SIGTERM, SIGKILL}) {
    if (exited()) {
      return status;
    }
    const pid_t pid = PidfilePid(pidfile);
    signal(pid > 0 ? pid : launcher, sig);
    for (std::chrono::milliseconds waited{0}; sig == SIGTERM && waited < grace; waited += kPoll) {
      if (exited()) {
        return status;
      }
      std::this_thread::sleep_for(kPoll);
    }
  }
  waitpid(launcher, &status, 0);
  return status;
}

}  // namespace bess::perf

#endif  // BESS_TESTS_PERF_DAEMON_STOP_H_
