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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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

#include <sys/prctl.h>
#include <sys/resource.h>

#include <cstdlib>

#include <glog/logging.h>
#include <gtest/gtest.h>

namespace {

// Tests that assert on a fatal path -- EXPECT_DEATH/ASSERT_DEATH -- make gtest
// fork a child which really does abort, because that is what the assertion is
// about. The kernel then writes a core for every one of those children, and
// systemd-coredump reports each as a crash: bessd_test and memory_test between
// them produce a handful per run, which buries the crashes that matter (the
// real daemon cores from G0 development were found in exactly this log).
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
