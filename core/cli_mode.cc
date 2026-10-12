// SPDX-License-Identifier: BSD-3-Clause

#include "cli_mode.h"

#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <print>
#include <string_view>

namespace bess {
namespace cli_mode {

namespace {

bool Failed(std::string_view what) {
  std::println(stderr, "bessctl: cannot drop privileges: {}: {}", what, std::strerror(errno));
  return false;
}

}  // namespace

std::optional<int> CliArgvOffset(int argc, const char *const *argv) {
  if (argc < 1 || argv[0] == nullptr) {
    return std::nullopt;
  }
  std::string_view name = argv[0];
  if (const size_t slash = name.rfind('/'); slash != std::string_view::npos) {
    name.remove_prefix(slash + 1);
  }
  if (name == "bessctl") {
    return 0;
  }
  if (argc >= 2 && std::string_view(argv[1]) == "ctl") {
    return 1;
  }
  return std::nullopt;
}

bool DropPrivileges() {
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return Failed("PR_SET_NO_NEW_PRIVS");
  }
  // Group first: once the user id is dropped, the group id may not be.
  const gid_t gid = getgid();
  if (gid != getegid() && setresgid(gid, gid, gid) != 0) {
    return Failed("setresgid");
  }
  const uid_t uid = getuid();
  if (uid != geteuid() && setresuid(uid, uid, uid) != 0) {
    return Failed("setresuid");
  }
  __user_cap_header_struct header = {.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
  __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3] = {};
  if (syscall(SYS_capset, &header, data) != 0) {
    return Failed("capset");
  }
  if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0) {
    return Failed("PR_CAP_AMBIENT_CLEAR_ALL");
  }
  return true;
}

}  // namespace cli_mode
}  // namespace bess
