// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <sys/prctl.h>
#include <sys/resource.h>

#include <cstdlib>

#include "utils/logging.h"
#include <gtest/gtest.h>

namespace {

// Tests that assert on a fatal path -- EXPECT_DEATH/ASSERT_DEATH -- make gtest
// fork a child which really does abort, because that is what the assertion is
// about. The kernel then writes a core for every one of those children, and
// systemd-coredump reports each as a crash: bessd_test and
// runtime_memory_test between them produce a handful per run, which buries
// the crashes that matter (the real daemon cores from G0 development were
// found in exactly this log).
//
// Make the test process non-dumpable (and belt-and-braces zero its core limit);
// its death-test children inherit both, so expected deaths stop reaching
// systemd-coredump at all -- no core file, no journal entry, no desktop
// notification. Nothing about the verdict is lost: an *unexpected* crash still
// fails its test, with the signal the test runner reports, and the daemon's own
// crashes still produce cores (this only affects the unit-test binaries).
//
// BESS_TEST_CORE_DUMPS=1 opts out, which is what you want when debugging a test
// crash with gdb: a non-dumpable process cannot be attached to by a non-root
// debugger either.
void LimitCoreDumps() {
  if (std::getenv("BESS_TEST_CORE_DUMPS") != nullptr) {
    return;
  }

  // RLIMIT_CORE alone is not enough on a machine whose core_pattern is a pipe
  // (systemd-coredump's default): the kernel skips the limit for piped cores,
  // so the death still reaches systemd, which logs it and notifies -- with
  // "Storage: none" because there is no core to keep. A non-dumpable process is
  // refused a core before that point, so nothing is reported at all.
  prctl(PR_SET_DUMPABLE, 0);

  struct rlimit limit = {0, 0};
  setrlimit(RLIMIT_CORE, &limit);
}

}  // namespace

int main(int argc, char **argv) {
  LimitCoreDumps();

  google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);

  // By default, suppress annoying warnings on every death test.
  testing::GTEST_FLAG(death_test_style) = "threadsafe";

  return RUN_ALL_TESTS();
}
