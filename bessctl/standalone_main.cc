// SPDX-License-Identifier: BSD-3-Clause

// bessctl on its own, without bessd (D-120): for a host that only runs the
// client, e.g. one whose CPU cannot run this bessd build. Not installed.

#include "cli.h"

int main(int argc, char **argv) {
  return bess::ctl::Main(argc, argv);
}
