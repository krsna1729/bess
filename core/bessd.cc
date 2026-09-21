// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "bessd.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glog/logging.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <list>
#include <string>
#include <tuple>

#include "debug.h"
#include "opts.h"
#include "port.h"

// How log messages are processed in BESS?
//
// Daemon mode (one-way, deliberately):
//   - LOG(*) / glog  -> glog's own sinks and log files (/tmp/bessd.*)
//   - printf / fprintf(stdout|stderr) / puts -> the process's stdout/stderr
//     file descriptors, which point at /dev/null: daemon output through C
//     stdio is discarded, not captured
//   - std::cout -> StreambufLogger -> LOG(INFO) -> glog's sinks
//   - std::cerr -> StreambufLogger -> LOG(WARNING) -> glog's sinks
//
// The C stdio streams are *not* routed back into glog. They used to be, via
// fopencookie() callbacks that called LOG(), which is a feedback loop on any
// glog that writes through libc's stderr (absl-based glog >= 0.7 does):
//
//   LOG -> glog -> fwrite(stderr) -> cookie callback -> LOG -> glog -> ...
//
// It recursed until the stack was exhausted (reproduced on glog 0.7.1, SIGSEGV
// inside absl's formatter). The invariant now is that glog never writes into a
// sink that calls glog again. Code that needs a daemon-visible diagnostic uses
// the logging API, not printf.
//
// Process mode (foreground; -f option) is untouched:
//   - LOG(*) -> standard error (colored, if applicable)
//   - stdout/cout -> standard output
//   - stderr/cerr -> standard error (colored, currently always)

namespace {

// Intercepts all output messages to an ostream-based class and redirect them
// to glog, with a specified log severity level. This behavior lasts as long as
// the object is alive, and afterwards the original behavior is restored.
class StreambufLogger : public std::streambuf {
 public:
  StreambufLogger(std::ostream &stream, google::LogSeverity severity)
      : stream_(stream), log_level_(severity) {
    org_streambuf_ = stream_.rdbuf(this);
  }

  // Restores the original streambuf
  virtual ~StreambufLogger() { stream_.rdbuf(org_streambuf_); }

  // Redirects all << operands to glog
  std::streamsize xsputn(const char_type *s, std::streamsize count) {
    WriteToGlog(s, count);
    return count;
  }

  // NOTE: This function is never going to be called for std::cout and
  // std::cerr, but implemented for general streams, for completeness)
  int_type overflow(int_type v) {
    char_type c = traits_type::to_char_type(v);
    WriteToGlog(&c, 1);
    return traits_type::not_eof(v);
  }

 private:
  void WriteToGlog(const char_type *s, std::streamsize count) {
    // prevent glog from creating an empty line even with no message
    if (count <= 0) {
      return;
    }

    // same as above. ignore << std::endl
    if (count == 1 && s[0] == '\n') {
      return;
    }

    // ignore trailing '\n', since glog will append it automatically
    if (s[count - 1] == '\n') {
      count--;
    }

    // since this is not an macro, we do not have __FILE__, and __LINE__ of
    // the caller.
    google::LogMessage("<unknown>", 0, log_level_).stream()
        << std::string(s, count);
  }

  std::ostream &stream_;
  std::streambuf *org_streambuf_;
  google::LogSeverity log_level_;
};

}  // namespace

namespace bess {
namespace bessd {

void ProcessCommandLineArgs() {
  if (FLAGS_t) {
    bess::debug::DumpTypes();
    exit(EXIT_SUCCESS);
  }

  if (FLAGS_f) {
    google::LogToStderr();
  }
}

void CheckRunningAsRoot() {
  if (!FLAGS_skip_root_check) {
    uid_t euid = geteuid();
    if (euid != 0) {
      LOG(ERROR) << "You need root privilege to run the BESS daemon";
      exit(EXIT_FAILURE);
    }
  }

  // Great power comes with great responsibility.
  umask(S_IWGRP | S_IWOTH);
}

void WritePidfile(int fd, pid_t pid) {
  if (ftruncate(fd, 0)) {
    PLOG(FATAL) << "ftruncate(pidfile, 0)";
  }
  if (lseek(fd, 0, SEEK_SET)) {
    PLOG(FATAL) << "lseek(pidfile, 0, SEEK_SET)";
  }

  std::string pid_str = std::to_string(pid) + "\n";
  if (write(fd, pid_str.data(), pid_str.size() + 1) < 0) {
    PLOG(FATAL) << "write(pidfile, pid)";
  }
  fsync(fd);
}

std::tuple<bool, pid_t> ReadPidfile(int fd) {
  if (lseek(fd, 0, SEEK_SET)) {
    PLOG(FATAL) << "lseek(pidfile, 0, SEEK_SET)";
  }

  char buf[BUFSIZ];
  int readlen = read(fd, buf, sizeof(buf) - 1);
  if (readlen <= 0) {
    if (readlen < 0) {
      PLOG(ERROR) << "read(pidfile=" << FLAGS_i << ")";
    } else {
      LOG(ERROR) << "read(pidfile=" << FLAGS_i << ")"
                 << " at EOF";
    }

    return std::make_tuple(false, 0);
  }
  buf[readlen] = '\0';

  pid_t pid;
  sscanf(buf, "%d", &pid);

  return std::make_tuple(true, pid);
}

std::tuple<bool, pid_t> TryAcquirePidfileLock(int fd) {
  bool lockacquired = false;
  pid_t pid = 0;

  if (flock(fd, LOCK_EX | LOCK_NB)) {
    // Lock is already held by another process.
    if (errno != EWOULDBLOCK) {
      PLOG(FATAL) << "flock(pidfile=" << FLAGS_i << ")";
    } else {
      VLOG(1) << "flock";
    }

    // Try to read the pid of the other process.
    bool success = false;
    std::tie(success, pid) = ReadPidfile(fd);
    if (!success) {
      PLOG(FATAL) << "Couldn't read pidfile";
    }
  } else {
    lockacquired = true;
  }

  return std::make_tuple(lockacquired, pid);
}

int CheckUniqueInstance(const std::string &pidfile_path) {
  const int kMaxPidfileLockTrials = 5;

  int fd = open(pidfile_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd == -1) {
    PLOG(FATAL) << "open(pidfile=" << FLAGS_i << ")";
  }

  int trials = 0;
  int delay_us = 100000;
  auto [lock_acquired, pid] = TryAcquirePidfileLock(fd);

  if (!lock_acquired) {
    if (!FLAGS_k) {
      LOG(ERROR) << "You cannot run more than one BESS instance at a time "
                 << "(add -k option?)";
      exit(EXIT_FAILURE);
    }
    LOG(INFO) << "There is another BESS daemon running (PID=" << pid << ")";

    while (!lock_acquired) {
      trials++;

      if (trials <= 3) {
        LOG(INFO) << "Sending SIGTERM signal...";
        if (kill(pid, SIGTERM) < 0) {
          PLOG(FATAL) << "kill(pid, SIGTERM)";
        }
      } else if (trials <= kMaxPidfileLockTrials) {
        LOG(INFO) << "Sending SIGKILL signal...";
        if (kill(pid, SIGKILL) < 0) {
          PLOG(FATAL) << "kill(pid, SIGKILL)";
        }
      } else {
        LOG(FATAL) << "ERROR: Cannot kill the process";
      }

      usleep(delay_us);
      std::tie(lock_acquired, pid) = TryAcquirePidfileLock(fd);
      delay_us *= 2;
    }

    LOG(INFO) << "Old instance has been successfully terminated.";
  }

  return fd;
}

static void CloseStdStreams() {
  int fd = open("/dev/null", O_RDWR, 0);
  if (fd < 0) {
    PLOG(ERROR) << "Cannot open /dev/null";
    return;
  }

  // Anything buffered before the descriptors are replaced belongs to the
  // process that was just forked; flush it here so it is not carried across the
  // redirection.
  fflush(nullptr);
  std::cout.flush();
  std::cerr.flush();

  // do not log to stderr anymore. (Correctness does not depend on this: fd 2 is
  // /dev/null below, so even a glog that insists on stderr cannot feed back
  // into anything that logs.)
  FLAGS_stderrthreshold = google::FATAL + 1;

  // Replace standard input/output/error with /dev/null. The libc FILE objects
  // (stdout, stderr) stay as they are and now refer to these descriptors: C
  // stdio output in daemon mode is discarded by design, never routed into glog.
  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);

  // The C++ streams are bridged into glog. This is safe in one direction: glog
  // writes through libc's stderr (fd 2 -> /dev/null), never through these
  // streambufs, so there is no path back into LOG().
  static StreambufLogger stdout_buf(std::cout, google::GLOG_INFO);
  static StreambufLogger stderr_buf(std::cerr, google::GLOG_WARNING);

  // For whatever reason if fd happens to be assigned 0, 1, or 2, do not close
  // it since it now points to /dev/null
  if (fd > 2) {
    close(fd);
  }
}

int Daemonize() {
  int pipe_fds[2];
  const int read_end = 0;
  const int write_end = 1;

  if (pipe(pipe_fds) < 0) {
    PLOG(FATAL) << "pipe()";
  }

  pid_t pid = fork();
  if (pid < 0) {
    PLOG(FATAL) << "fork()";
  } else if (pid > 0) {
    // Parent process.
    close(pipe_fds[write_end]);

    uint64_t tmp;
    if (read(pipe_fds[read_end], &tmp, sizeof(tmp)) == sizeof(uint64_t)) {
      LOG(INFO) << "Done (PID=" << pid << ")";
      exit(EXIT_SUCCESS);
    } else {
      LOG(ERROR) << "Failed to launch a daemon process";
      exit(EXIT_FAILURE);
    }
  } else {
    // Child process.
    close(pipe_fds[read_end]);
    // Fall through.
  }

  // Start a new session.
  if (setsid() < 0) {
    PLOG(WARNING) << "setsid()";
  }

  CloseStdStreams();

  return pipe_fds[write_end];
}

bool SetResourceLimit() {
  struct rlimit limit = {.rlim_cur = 65536, .rlim_max = 262144};

  for (;;) {
    if (setrlimit(RLIMIT_NOFILE, &limit) == 0) {
      return true;
    }

    if (errno == EPERM && limit.rlim_cur >= 1024) {
      limit.rlim_max /= 2;
      limit.rlim_cur = std::min(limit.rlim_cur, limit.rlim_max);
      continue;
    }

    LOG(WARNING) << "setrlimit() failed";
    return false;
  }
}

// Return true if string s has specified suffix.
static inline bool HasSuffix(const std::string &s, const std::string &suffix) {
  return (s.size() >= suffix.size()) &&
         std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

// Store handles of loaded plugins
// key: plugin path (std::string), value: handle (void *)
static std::unordered_map<std::string, void *> plugin_handles;

std::vector<std::string> ListPlugins() {
  std::vector<std::string> list;
  for (auto &kv : plugin_handles) {
    list.push_back(kv.first);
  }
  return list;
}

bool LoadPlugin(const std::string &path) {
  void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (handle != nullptr) {
    plugin_handles.emplace(path, handle);
    return true;
  }
  return false;
}

bool UnloadPlugin(const std::string &path) {
  auto it = plugin_handles.find(path);
  if (it == plugin_handles.end()) {
    VLOG(1) << "Plugin " << path << " not found.";
    return false;
  }
  bool success = (dlclose(it->second) == 0);
  if (success) {
    plugin_handles.erase(it);
  } else {
    LOG(WARNING) << "Error unloading module " << path << ": " << dlerror();
  }
  return success;
}

bool LoadPlugins(const std::string &directory) {
  DIR *dir = opendir(directory.c_str());
  if (!dir) {
    return false;
  }

  std::list<std::string> remaining;
  dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if ((entry->d_type == DT_REG || entry->d_type == DT_LNK) &&
        HasSuffix(entry->d_name, ".so")) {
      const std::string full_path = directory + "/" + entry->d_name;
      remaining.push_back(full_path);
    }
  }

  for (int pass = 1; pass <= kInheritanceLimit && remaining.size() > 0;
       ++pass) {
    for (auto it = remaining.begin(); it != remaining.end();) {
      const std::string full_path = *it;
      LOG(INFO) << "Loading plugin (attempt " << pass << "): " << full_path;
      if (!LoadPlugin(full_path)) {
        VLOG(1) << "Error loading plugin " << full_path
                << "dlerror=" << dlerror();
        ++it;
      } else {
        it = remaining.erase(it);
      }
    }
  }

  for (auto it = remaining.begin(); it != remaining.end(); ++it) {
    LOG(ERROR)
        << "Failed to load plugin " << *it
        << ". Run daemon in verbose mode (--v=1) to see dlopen() attempts.";
  }

  closedir(dir);
  return (remaining.size() == 0);
}

std::string GetCurrentDirectory() {
  char dest[PATH_MAX + 1];
  ssize_t res = readlink("/proc/self/exe", dest, PATH_MAX);
  if (res == -1) {
    PLOG(FATAL) << "readlink()";
  }
  dest[res] = '\0';
  const char *slash = strrchr(dest, '/');
  if (slash == nullptr) {
    PLOG(FATAL) << "strrchr()";
  }
  return std::string(dest, slash - dest + 1);
}

}  // namespace bessd
}  // namespace bess
