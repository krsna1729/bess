// SPDX-License-Identifier: BSD-3-Clause

// bessd is also bessctl (D-120): invoked as `bessctl ...` or `bessd ctl ...`
// it runs the command line client, without privileges, before any daemon
// setup.

#ifndef BESS_CLI_MODE_H_
#define BESS_CLI_MODE_H_

#include <optional>

namespace bess {
namespace cli_mode {

// Where the client's arguments start in argv when this invocation is the
// client: 0 when argv[0]'s basename is "bessctl", 1 when argv[1] is "ctl".
// nullopt for the daemon.
std::optional<int> CliArgvOffset(int argc, const char *const *argv);

// Drops every privilege the process could have: no_new_privs, the real user
// and group ids if they differ from the effective ones (a setuid or setgid
// binary), and the effective, permitted, inheritable and ambient capability
// sets. On a failure it says why on stderr and returns false; the caller must
// not go on.
bool DropPrivileges();

}  // namespace cli_mode
}  // namespace bess

#endif  // BESS_CLI_MODE_H_
