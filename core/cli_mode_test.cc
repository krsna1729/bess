// SPDX-License-Identifier: BSD-3-Clause

// The multi-call dispatch and the client's privilege drop (D-120).

#include "cli_mode.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <string>

namespace {

using bess::cli_mode::CliArgvOffset;
using bess::cli_mode::DropPrivileges;

TEST(CliModeTest, DispatchesOnNameOrCtl) {
  const char *as_bessctl[] = {"/usr/bin/bessctl", "status"};
  EXPECT_EQ(CliArgvOffset(2, as_bessctl), 0);
  const char *bare_bessctl[] = {"bessctl"};
  EXPECT_EQ(CliArgvOffset(1, bare_bessctl), 0);
  const char *ctl[] = {"./bessd", "ctl", "--help"};
  EXPECT_EQ(CliArgvOffset(3, ctl), 1);
  const char *daemon[] = {"/usr/bin/bessd", "-f"};
  EXPECT_EQ(CliArgvOffset(2, daemon), std::nullopt);
  const char *ctl_later[] = {"bessd", "-f", "ctl"};
  EXPECT_EQ(CliArgvOffset(3, ctl_later), std::nullopt);
  const char *suffix[] = {"/bin/my-bessctl"};
  EXPECT_EQ(CliArgvOffset(1, suffix), std::nullopt);
  EXPECT_EQ(CliArgvOffset(0, nullptr), std::nullopt);
}

// The /proc/self/status fields a dropped process must show.
bool Dropped() {
  std::map<std::string, std::string> fields;
  std::ifstream status("/proc/self/status");
  for (std::string line; std::getline(status, line);) {
    const size_t colon = line.find(':');
    const size_t value = line.find_first_not_of(" \t", colon + 1);
    if (colon != std::string::npos && value != std::string::npos) {
      fields[line.substr(0, colon)] = line.substr(value);
    }
  }
  const std::string none = "0000000000000000";
  return fields["NoNewPrivs"] == "1" && fields["CapEff"] == none && fields["CapPrm"] == none &&
         fields["CapInh"] == none && fields["CapAmb"] == none;
}

TEST(CliModeTest, DropsPrivileges) {
  // In a child: no_new_privs cannot be undone in this process.
  EXPECT_EXIT(
      {
        if (!DropPrivileges()) {
          _exit(2);
        }
        _exit(Dropped() && getuid() == geteuid() && getgid() == getegid() ? 0 : 1);
      },
      testing::ExitedWithCode(0), "");
}

}  // namespace
