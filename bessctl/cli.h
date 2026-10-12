// SPDX-License-Identifier: BSD-3-Clause

// bessctl's entry point: bessd's multi-call mode (D-120) and the standalone
// binary (standalone_main.cc) both run it.

#ifndef BESSCTL_CLI_H_
#define BESSCTL_CLI_H_

namespace bess::ctl {

// The command line client: argv[0] is the program name, the arguments follow.
// Returns the process exit status.
int Main(int argc, char **argv);

}  // namespace bess::ctl

#endif  // BESSCTL_CLI_H_
